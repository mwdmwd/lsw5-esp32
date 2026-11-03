#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Temporary NetworkManager AP with a whitelist. See README.md."""

import argparse
import fcntl
import getpass
import ipaddress
import json
import os
import pwd
import re
import secrets
import shlex
import shutil
import signal
import subprocess
import sys
import time
import tomllib
import uuid
from pathlib import Path

# Machine settings. Wi-Fi credentials live in hotspot.toml; traffic rules in policy.conf.
WIFI = "wlan0"
UPLINK = "eth0"
CHANNEL = 6  # 2.4 GHz, WPA2/CCMP, for older dongles
SUBNET = ipaddress.IPv4Network("10.77.0.0/24")
GATEWAY = str(SUBNET[1])
DNS_UPSTREAM = "1.1.1.1"  # used only for dns-pass lines
POLICY = Path(__file__).resolve().with_name("policy.conf")
SETTINGS = Path(__file__).resolve().with_name("hotspot.toml")
RUN = Path("/run/inverter-hotspot")
TABLE = "inverter_hotspot"
PROFILE = "inverter-lab"

# Block these even if explicitly allowed. Also block every non-default route
# present at start/reload, including public-address LAN/VPN prefixes.
SPECIAL = [
    "0.0.0.0/8",
    "10.0.0.0/8",
    "100.64.0.0/10",
    "127.0.0.0/8",
    "169.254.0.0/16",
    "172.16.0.0/12",
    "192.0.0.0/24",
    "192.0.2.0/24",
    "192.168.0.0/16",
    "198.18.0.0/15",
    "198.51.100.0/24",
    "203.0.113.0/24",
    "224.0.0.0/4",
    "240.0.0.0/4",
]


def command(*args, data=None, check=True):
    return subprocess.run(
        [str(a) for a in args], input=data, text=True, capture_output=True, check=check
    )


def output(*args):
    return command(*args).stdout.strip()


def atomic(path, text, mode=0o600):
    temporary = path.with_name(path.name + ".new")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, mode)
    with os.fdopen(fd, "w") as stream:
        stream.write(text)
    os.chmod(temporary, mode)
    temporary.replace(path)


def wifi_settings(path=SETTINGS):
    settings = {}
    try:
        with path.open("rb") as stream:
            settings = tomllib.load(stream)
    except FileNotFoundError:
        pass
    except tomllib.TOMLDecodeError:
        raise ValueError(f"Invalid TOML syntax in {path}") from None
    if settings.keys() - {"ssid", "password"}:
        raise ValueError(f"Only ssid and password settings are supported in {path}")
    ssid = settings.get("ssid", "inverter-lab")
    password = settings.get("password", "")
    if (
        not isinstance(ssid, str)
        or not 1 <= len(ssid.encode("utf-8")) <= 32
        or any(ord(c) < 32 or ord(c) == 127 for c in ssid)
    ):
        raise ValueError("SSID must be 1..32 UTF-8 bytes without control characters")
    if not isinstance(password, str):
        raise TypeError("Wi-Fi password must be a quoted string")
    if password:
        validate_password(password)
    return ssid, password


def validate_password(password):
    if not 8 <= len(password) <= 63 or not all(32 <= ord(c) <= 126 for c in password):
        raise ValueError("Use a WPA2 password of 8..63 printable ASCII characters")


def name(value):
    value = value.lower().rstrip(".")
    labels = value.split(".")
    if (
        len(value) > 253
        or len(labels) < 2
        or not all(re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label) for label in labels)
    ):
        raise ValueError(f"Invalid exact hostname: {value!r}")
    return value


def public_ip(value, blocked):
    address = ipaddress.IPv4Address(value)
    if not address.is_global or address.is_multicast or any(address in n for n in blocked):
        raise ValueError(f"Local, routed, or non-public destination is forbidden: {address}")
    return str(address)


def resolve(host):
    answer = output("dig", f"@{DNS_UPSTREAM}", host, "A", "+short", "+time=2", "+tries=1")
    addresses = []
    for line in answer.splitlines():
        try:
            addresses.append(str(ipaddress.IPv4Address(line)))
        except ValueError:
            pass  # CNAMEs are followed by the recursive resolver.
    if not addresses:
        raise ValueError(f"No IPv4 answer for {host} from {DNS_UPSTREAM}; policy not applied")
    return addresses


def read_policy(text, blocked, resolver=resolve):
    """Parse the small policy language before making any live changes."""
    pinned, passthrough, pending, intercepted = {}, set(), [], []
    output_allowed = {"tcp": set(), "udp": set()}
    for number, line in enumerate(text.splitlines(), 1):
        words = shlex.split(line, comments=True)
        if not words:
            continue
        try:
            if words[0] == "dns" and len(words) >= 3:
                host = name(words[1])
                if host in pinned or host in passthrough:
                    raise ValueError(f"Duplicate DNS policy for {host}")
                pinned[host] = sorted({public_ip(ip, blocked) for ip in words[2:]})
            elif words[0] == "dns-pass" and len(words) == 2:
                host = name(words[1])
                if host in pinned or host in passthrough:
                    raise ValueError(f"Duplicate DNS policy for {host}")
                passthrough.add(host)
            elif words[0] == "intercept" and len(words) == 5 and words[1] == "tcp":
                port, local_port = map(int, words[3:])
                if not 1 <= port <= 65535 or port == 53 or not 1024 <= local_port <= 65535:
                    raise ValueError(
                        "intercept requires a remote port other than 53 and local port 1024..65535"
                    )
                intercepted.append((words[2], port, local_port))
            elif (
                words[0] in ("allow", "output-allow")
                and len(words) >= 4
                and words[1] in ("tcp", "udp")
            ):
                ports = [int(port) for port in words[3:]]
                if any(port < 1 or port > 65535 for port in ports):
                    raise ValueError("Ports must be 1..65535")
                if words[0] == "allow":
                    if 53 in ports:
                        raise ValueError("External port 53 is forbidden")
                    pending.append((words[1], words[2], ports))
                else:
                    address = ipaddress.IPv4Address(words[2])
                    if address not in SUBNET or address in (
                        SUBNET.network_address,
                        SUBNET.broadcast_address,
                        ipaddress.IPv4Address(GATEWAY),
                    ):
                        raise ValueError(f"output-allow requires a client IPv4 address in {SUBNET}")
                    output_allowed[words[1]].update((str(address), port) for port in ports)
            else:
                raise ValueError(
                    "Expected dns NAME IP..., dns-pass NAME, allow tcp|udp DEST PORT..., "
                    "output-allow tcp|udp CLIENT_IP PORT..., or intercept tcp DEST PORT LOCAL_PORT"
                )
        except ValueError as error:
            raise ValueError(f"Policy line {number}: {error}") from error
    resolved = dict(pinned)
    allowed = {"tcp": set(), "udp": set()}

    def addresses_for(destination):
        try:
            ipaddress.IPv4Address(destination)
        except ValueError:
            host = name(destination)
            if host not in pinned and host not in passthrough:
                raise ValueError(f"{host} needs an explicit dns or dns-pass line")
            if host not in resolved:
                resolved[host] = sorted({public_ip(ip, blocked) for ip in resolver(host)})
            return resolved[host]
        else:
            return [public_ip(destination, blocked)]

    for protocol, destination, ports in pending:
        addresses = addresses_for(destination)
        allowed[protocol].update((ip, port) for ip in addresses for port in ports)
    intercept_tcp = {}
    for destination, port, local_port in intercepted:
        for address in addresses_for(destination):
            key = (address, port)
            if key in intercept_tcp and intercept_tcp[key] != local_port:
                raise ValueError(f"Conflicting intercept listeners for {address}:{port}")
            if key in allowed["tcp"]:
                raise ValueError(
                    f"Remove direct allow for intercepted destination {address}:{port}"
                )
            intercept_tcp[key] = local_port
    return {
        "pinned": pinned,
        "passthrough": sorted(passthrough),
        "resolved": resolved,
        "allowed": {p: sorted(v) for p, v in allowed.items()},
        "output_allowed": {p: sorted(v) for p, v in output_allowed.items()},
        "intercept_tcp": sorted((*key, port) for key, port in intercept_tcp.items()),
    }


def topology():
    routes = json.loads(output("ip", "-j", "-4", "route", "show", "table", "all"))
    defaults = [
        r
        for r in routes
        if r.get("dst") == "default"
        and r.get("dev") == UPLINK
        and r.get("table", "main") == "main"
        and r.get("gateway")
    ]
    if len(defaults) != 1:
        raise ValueError(f"Expected one IPv4 default gateway on {UPLINK}")
    blocked = [ipaddress.IPv4Network(n) for n in SPECIAL]
    for route in routes:
        if route.get("dst") not in (None, "default") and route.get("dev") != WIFI:
            network = ipaddress.IPv4Network(route["dst"], strict=False)
            if network.overlaps(SUBNET):
                raise ValueError(f"Hotspot subnet conflicts with existing route {network}")
            blocked.append(network)
    return str(ipaddress.IPv4Address(defaults[0]["gateway"])), list(
        ipaddress.collapse_addresses(blocked)
    )


def elements(policy, protocol, key="allowed"):
    return ", ".join(f"{ip} . {port}" for ip, port in policy[key][protocol])


def render_sets(policy, blocked):
    lines = [
        f"flush set inet {TABLE} blocked4",
        f"add element inet {TABLE} blocked4 {{ {', '.join(map(str, blocked))} }}",
    ]
    for key, prefix in (("allowed", ""), ("output_allowed", "output_")):
        for protocol in ("tcp", "udp"):
            set_name = f"{prefix}{protocol}4"
            lines.append(f"flush set inet {TABLE} {set_name}")
            if policy[key][protocol]:
                lines.append(
                    f"add element inet {TABLE} {set_name} {{ {elements(policy, protocol, key)} }}"
                )
    lines.extend([f"flush set inet {TABLE} intercept_tcp4", f"flush chain inet {TABLE} intercept"])
    if policy["intercept_tcp"]:
        tuples = ", ".join(
            f"{ip} . {port} . {local}" for ip, port, local in policy["intercept_tcp"]
        )
        lines.append(f"add element inet {TABLE} intercept_tcp4 {{ {tuples} }}")
        for ip, port, local in policy["intercept_tcp"]:
            lines.append(
                f'add rule inet {TABLE} intercept iifname "{WIFI}" ip saddr {SUBNET} '
                f"ip saddr != {GATEWAY} ip daddr {ip} tcp dport {port} counter redirect to :{local}"
            )
    return "\n".join(lines) + "\n"


def render_firewall(policy, blocked, router, uplink_forwarding=False):
    # Never flush other software's rules. An earlier/later accept cannot override
    # our drops. Refuse flowtables at startup: offload can bypass forward hooks.
    guard = "" if uplink_forwarding else f'iifname "{UPLINK}" oifname != "{WIFI}" counter drop'
    return f'''table inet {TABLE} {{
    set blocked4 {{ type ipv4_addr; flags interval; auto-merge; }}
    set tcp4 {{ type ipv4_addr . inet_service; }}
    set udp4 {{ type ipv4_addr . inet_service; }}
    set output_tcp4 {{ type ipv4_addr . inet_service; }}
    set output_udp4 {{ type ipv4_addr . inet_service; }}
    set intercept_tcp4 {{ type ipv4_addr . inet_service . inet_service; }}
    chain intercept {{
        type nat hook prerouting priority dstnat; policy accept;
    }}
    chain denied_input {{
        limit rate 5/second burst 20 packets log prefix "INVHOT INPUT "
        counter drop
    }}
    chain denied_forward {{
        limit rate 10/second burst 30 packets log prefix "INVHOT BLOCK "
        counter drop
    }}
    chain denied_output {{
        limit rate 5/second burst 20 packets log prefix "INVHOT OUTPUT "
        counter drop
    }}
    chain input {{
        type filter hook input priority -10; policy accept;
        iifname != "{WIFI}" return
        ct state invalid jump denied_input
        ip saddr {{ 0.0.0.0, {SUBNET} }} ip daddr {{ {GATEWAY}, {SUBNET.broadcast_address}, 255.255.255.255 }} udp sport 68 udp dport 67 counter accept
        ip saddr {SUBNET} ip daddr {GATEWAY} udp dport 53 counter accept
        ip saddr {SUBNET} ip daddr {GATEWAY} tcp dport 53 counter accept
        ip saddr {SUBNET} ip daddr {GATEWAY} ct direction original ct status dnat ct original ip daddr . ct original proto-dst . tcp dport @intercept_tcp4 counter accept
        ip daddr {GATEWAY} ct original ip saddr {GATEWAY} ct direction reply ct state established ip saddr . tcp sport @output_tcp4 counter accept
        ip daddr {GATEWAY} ct original ip saddr {GATEWAY} ct direction reply ct state established ip saddr . udp sport @output_udp4 counter accept
        ip saddr {SUBNET} ip daddr {GATEWAY} ct original ip saddr {GATEWAY} ct direction reply ct state related ip protocol icmp ct original protocol tcp ct original ip daddr . ct original proto-dst @output_tcp4 counter accept
        ip saddr {SUBNET} ip daddr {GATEWAY} ct original ip saddr {GATEWAY} ct direction reply ct state related ip protocol icmp ct original protocol udp ct original ip daddr . ct original proto-dst @output_udp4 counter accept
        jump denied_input
    }}
    chain forward {{
        type filter hook forward priority -10; policy accept;
        iifname "{WIFI}" jump outbound
        oifname "{WIFI}" jump inbound
        {guard}
    }}
    chain outbound {{
        meta nfproto != ipv4 jump denied_forward
        ip saddr != {SUBNET} jump denied_forward
        ip saddr {GATEWAY} jump denied_forward
        ct state invalid jump denied_forward
        oifname != "{UPLINK}" jump denied_forward
        rt ip nexthop != {router} jump denied_forward
        ip daddr @blocked4 jump denied_forward
        ip daddr . tcp dport @tcp4 counter accept
        ip daddr . udp dport @udp4 counter accept
        jump denied_forward
    }}
    chain inbound {{
        meta nfproto != ipv4 jump denied_forward
        iifname != "{UPLINK}" jump denied_forward
        ip daddr != {SUBNET} jump denied_forward
        ct direction reply ct state established ip saddr . tcp sport @tcp4 counter accept
        ct direction reply ct state established ip saddr . udp sport @udp4 counter accept
        ct direction reply ct state related ip protocol icmp ct original protocol tcp ct original ip daddr . ct original proto-dst @tcp4 counter accept
        ct direction reply ct state related ip protocol icmp ct original protocol udp ct original ip daddr . ct original proto-dst @udp4 counter accept
        jump denied_forward
    }}
    chain output {{
        type filter hook output priority -10; policy accept;
        oifname != "{WIFI}" return
        ip saddr {GATEWAY} udp sport 67 udp dport 68 counter accept
        ip saddr {GATEWAY} ip daddr {SUBNET} udp sport 53 counter accept
        ip saddr {GATEWAY} ip daddr {SUBNET} tcp sport 53 ct state established counter accept
        ip saddr {GATEWAY} ip daddr {SUBNET} ct direction reply ct state established ct status dnat ct original ip daddr . ct original proto-dst . tcp sport @intercept_tcp4 counter accept
        ip saddr {GATEWAY} ct direction original ct state {{ new, established }} ip daddr . tcp dport @output_tcp4 counter accept
        ip saddr {GATEWAY} ct direction original ct state {{ new, established }} ip daddr . udp dport @output_udp4 counter accept
        jump denied_output
    }}
    chain nat {{
        type nat hook postrouting priority srcnat; policy accept;
        iifname "{WIFI}" oifname "{UPLINK}" ip saddr {SUBNET} masquerade
    }}
}}
''' + render_sets(policy, blocked)


def render_dns(policy, directory=RUN, upstream=DNS_UPSTREAM):
    config = f"""interface={WIFI}
except-interface=lo
listen-address={GATEWAY}
bind-interfaces
no-resolv
no-hosts
local=/#/
stop-dns-rebind
domain-needed
filter-AAAA
local-ttl=30
dhcp-range={SUBNET[100]},{SUBNET[150]},{SUBNET.netmask},1h
dhcp-option=option:router,{GATEWAY}
dhcp-option=option:dns-server,{GATEWAY}
dhcp-authoritative
dhcp-ignore-names
dhcp-leasefile={directory}/dns/leases
pid-file=
user=nobody
log-queries=extra
log-dhcp
log-facility=-
"""
    for host, addresses in policy["pinned"].items():
        for ip in addresses:
            config += f"host-record={host},{ip}\n"
    for host in policy["passthrough"]:
        # The extra wildcard local rule matches descendants but not the apex.
        config += f"server=/{host}/{upstream}\nlocal=/*.{host}/\n"
    return config


def sysctl_path(interface, setting):
    return Path(f"/proc/sys/net/ipv4/conf/{interface}/{setting}")


def save_state(state):
    atomic(RUN / "state.json", json.dumps(state, indent=2) + "\n")


def load_state():
    return json.loads((RUN / "state.json").read_text())


def process_identity(pid):
    # PID reuse must not let reload/stop signal an unrelated process.
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


def running(state):
    try:
        return process_identity(state["pid"]) == state["starttime"]
    except FileNotFoundError:
        return False


def preflight():
    for program in ("nmcli", "ip", "nft", "dnsmasq", "dig"):
        if not shutil.which(program):
            raise ValueError(f"Install {program} before starting")
    for interface in (WIFI, UPLINK):
        if not re.fullmatch(r"[a-zA-Z0-9_-]{1,15}", interface):
            raise ValueError("Invalid interface name in machine settings")
    if WIFI == UPLINK:
        raise ValueError("Wi-Fi and uplink must be separate interfaces")
    device = json.loads(output("ip", "-j", "address", "show", "dev", WIFI))[0]
    if device.get("master") or device.get("addr_info"):
        raise ValueError(f"{WIFI} must be idle, unbridged, and have no IP addresses")
    if output("nmcli", "-g", "GENERAL.STATE", "device", "show", WIFI).split()[0] != "30":
        raise ValueError(f"{WIFI} must be disconnected in NetworkManager")
    if output("nmcli", "-g", "WIFI-PROPERTIES.AP", "device", "show", WIFI) != "yes":
        raise ValueError(f"{WIFI} does not support AP mode")
    rules = json.loads(output("nft", "-j", "list", "ruleset"))["nftables"]
    if any("flowtable" in item for item in rules):
        raise ValueError("Existing nftables flowtables may bypass filtering; disable offload first")
    if any(item.get("table", {}).get("name") == TABLE for item in rules):
        raise ValueError("Hotspot firewall already exists; run stop/recover first")
    for line in output("ss", "-H", "-lntu", "( sport = :53 or sport = :67 )").splitlines():
        address = line.split()[4]
        if not address.startswith(("127.", "[::1]:")):
            raise ValueError(f"Existing DNS/DHCP listener could be exposed: {address}")


def compile_policy():
    router, blocked = topology()
    policy = read_policy(POLICY.read_text(), blocked)
    destinations = {address for tuples in policy["allowed"].values() for address, _ in tuples}
    destinations.update(address for address, _, _ in policy["intercept_tcp"])
    for address in sorted(destinations):
        route = json.loads(output("ip", "-j", "-4", "route", "get", address))[0]
        if route.get("dev") != UPLINK or route.get("gateway") != router:
            raise ValueError(f"{address} does not route through the selected Internet gateway")
    return router, blocked, policy


def check_dns(config):
    atomic(RUN / "dnsmasq.next.conf", config, 0o644)
    command("dnsmasq", "--test", f"--conf-file={RUN}/dnsmasq.next.conf")


def launch_dns(config):
    atomic(RUN / "dnsmasq.conf", config, 0o644)
    with (RUN / "dnsmasq.log").open("a") as log:
        process = subprocess.Popen(
            ["dnsmasq", "--keep-in-foreground", f"--conf-file={RUN}/dnsmasq.conf"],
            stdout=log,
            stderr=log,
        )
    time.sleep(0.3)
    if process.poll() is not None:
        raise RuntimeError(f"dnsmasq failed; inspect {RUN}/dnsmasq.log")
    return process


def stop_dns(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def cleanup(state):
    # Stop the AP before removing its firewall. On failure, keep the rules
    # and saved state so `stop` can retry.
    profiles = output("nmcli", "-g", "UUID", "connection", "show").splitlines()
    if state["uuid"] in profiles:
        command("nmcli", "--wait", "15", "connection", "delete", "uuid", state["uuid"])
    if output("nmcli", "-g", "GENERAL.CON-UUID", "device", "show", WIFI) == state["uuid"]:
        raise RuntimeError("AP is still active; keeping firewall installed")
    if state.get("dns") and running(state["dns"]):
        os.kill(state["dns"]["pid"], signal.SIGTERM)
    for interface, settings in state["sysctls"].items():
        for setting, value in settings.items():
            sysctl_path(interface, setting).write_text(value)
    tables = json.loads(output("nft", "-j", "list", "tables"))["nftables"]
    if any(
        item.get("table", {}).get("name") == TABLE and item["table"].get("family") == "inet"
        for item in tables
    ):
        command("nft", "delete", "table", "inet", TABLE)
    command("nmcli", "device", "set", WIFI, "autoconnect", state["autoconnect"])
    (RUN / "password").unlink(missing_ok=True)
    (RUN / "state.json").unlink(missing_ok=True)


def run_hotspot():
    ssid, password = wifi_settings()
    preflight()
    router, blocked, policy = compile_policy()
    if not password:
        password = getpass.getpass(f"WPA2 password for {ssid} (empty = generate): ")
        if not password:
            password = secrets.token_urlsafe(15)
            print(f"Generated Wi-Fi password: {password}", flush=True)
    validate_password(password)
    state = {
        "pid": os.getpid(),
        "starttime": process_identity(os.getpid()),
        "uuid": str(uuid.uuid4()),
        "generation": 0,
        "error": None,
        "sysctls": {
            interface: {
                key: sysctl_path(interface, key).read_text()
                for key in ("forwarding", "send_redirects")
            }
            for interface in (WIFI, UPLINK)
        },
        "autoconnect": output("nmcli", "-g", "GENERAL.AUTOCONNECT", "device", "show", WIFI),
    }
    firewall = render_firewall(
        policy, blocked, router, state["sysctls"][UPLINK]["forwarding"].strip() == "1"
    )
    dns_config = render_dns(policy)
    check_dns(dns_config)
    command("nft", "--check", "-f", "-", data=firewall)
    save_state(state)
    dns = None
    reloading = False

    def reload_signal(_signum, _frame):
        nonlocal reloading
        reloading = True

    def stop_signal(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGHUP, reload_signal)
    signal.signal(signal.SIGTERM, stop_signal)
    try:
        command("nft", "-f", "-", data=firewall)
        atomic(RUN / "firewall.nft", firewall)
        command("nmcli", "device", "set", WIFI, "autoconnect", "no")
        command(
            "nmcli",
            "connection",
            "add",
            "save",
            "no",
            "type",
            "wifi",
            "ifname",
            WIFI,
            "con-name",
            PROFILE,
            "connection.uuid",
            state["uuid"],
            "autoconnect",
            "no",
            "ssid",
            ssid,
            "wifi.mode",
            "ap",
            "wifi.band",
            "bg",
            "wifi.channel",
            str(CHANNEL),
            "wifi.ap-isolation",
            "yes",
            "ipv4.method",
            "manual",
            "ipv4.addresses",
            f"{GATEWAY}/{SUBNET.prefixlen}",
            "ipv4.never-default",
            "yes",
            "ipv4.forwarding",
            "1",
            "ipv6.method",
            "disabled",
            "wifi-sec.key-mgmt",
            "wpa-psk",
            "wifi-sec.proto",
            "rsn",
            "wifi-sec.pairwise",
            "ccmp",
            "wifi-sec.group",
            "ccmp",
            "wifi-sec.pmf",
            "disable",
            "wifi-sec.psk-flags",
            "2",
        )
        atomic(RUN / "password", f"802-11-wireless-security.psk:{password}\n")
        command(
            "nmcli",
            "--wait",
            "30",
            "connection",
            "up",
            "uuid",
            state["uuid"],
            "passwd-file",
            RUN / "password",
        )
        (RUN / "password").unlink()
        for interface in (WIFI, UPLINK):
            sysctl_path(interface, "send_redirects").write_text("0")
            sysctl_path(interface, "forwarding").write_text("1")
        dns = launch_dns(dns_config)
        state["dns"] = {"pid": dns.pid, "starttime": process_identity(dns.pid)}
        state["policy"] = policy
        save_state(state)
        print(f"{ssid} is up on {WIFI}; DHCP/DNS {GATEWAY}; policy {POLICY}", flush=True)
        print(
            "Ctrl-C stops it. In another terminal: sudo ./hotspot.py logs / reload / status",
            flush=True,
        )
        while True:
            time.sleep(0.5)
            if dns.poll() is not None:
                raise RuntimeError("dnsmasq exited; shutting down hotspot")
            if output("nmcli", "-g", "GENERAL.CON-UUID", "device", "show", WIFI) != state["uuid"]:
                raise RuntimeError("Hotspot connection disappeared; shutting down")
            if not reloading:
                continue
            reloading = False
            try:
                new_router, new_blocked, new_policy = compile_policy()
                if new_router != router:
                    raise ValueError("Default gateway changed; stop and restart the hotspot")
                new_dns = render_dns(new_policy)
                check_dns(new_dns)
                changes = render_sets(new_policy, new_blocked)
                command("nft", "--check", "-f", "-", data=changes)
            except (ValueError, subprocess.CalledProcessError) as error:
                state["error"] = str(error)
                print(f"Reload rejected; previous policy retained: {error}", flush=True)
            else:
                # One atomic nft transaction revokes old tuples immediately,
                # including established flows. Restart DNS to discard its cache.
                command("nft", "-f", "-", data=changes)
                stop_dns(dns)
                dns = launch_dns(new_dns)
                state["dns"] = {"pid": dns.pid, "starttime": process_identity(dns.pid)}
                state["policy"], state["error"] = new_policy, None
                print("Policy reloaded.", flush=True)
            state["generation"] += 1
            save_state(state)
    finally:
        stop_dns(dns)
        cleanup(state)
        print("Hotspot stopped; prior interface settings restored.", flush=True)


def main():
    os.environ["LC_ALL"] = "C"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("start", "reload", "stop", "status", "logs"))
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("Run with sudo (including status/logs)")
    if RUN.is_symlink() or (RUN.exists() and RUN.stat().st_uid != 0):
        raise ValueError(f"Refusing unsafe runtime directory {RUN}")
    RUN.mkdir(mode=0o755, exist_ok=True)
    if args.action == "logs":
        follower = subprocess.Popen(["tail", "-n", "30", "-F", str(RUN / "dnsmasq.log")])
        try:
            subprocess.run(["journalctl", "-k", "-f", "-n", "30", "-g", "INVHOT "], check=True)
        finally:
            follower.terminate()
            follower.wait()
        return
    if args.action == "status":
        state = load_state()
        print(json.dumps(state.get("policy", {}), indent=2))
        print(output("nmcli", "-f", "GENERAL,IP4", "device", "show", WIFI))
        print(output("nft", "list", "table", "inet", TABLE))
        lease = RUN / "dns" / "leases"
        if lease.exists():
            print("DHCP leases:\n" + lease.read_text())
        return
    if args.action in ("stop", "reload"):
        state = load_state()
        if not running(state):
            if args.action == "stop":
                cleanup(state)
                print("Recovered stale hotspot state.")
                return
            raise ValueError("Launcher is not running; run stop to recover first")
        if args.action == "reload" and "output_allowed" not in state.get("policy", {}):
            raise ValueError(
                "Running launcher predates output-allow; stop and start it once to load the new code"
            )
        if args.action == "reload" and "intercept_tcp" not in state.get("policy", {}):
            raise ValueError(
                "Running launcher predates intercept; stop and start it once to load the new code"
            )
        os.kill(state["pid"], signal.SIGTERM if args.action == "stop" else signal.SIGHUP)
        for _ in range(60):
            time.sleep(0.5)
            if not (RUN / "state.json").exists():
                if args.action == "reload":
                    raise RuntimeError("Hotspot stopped during reload; inspect launcher output")
                print("Hotspot stopped.")
                return
            current = load_state()
            if args.action == "reload" and current["generation"] != state["generation"]:
                if current["error"]:
                    raise ValueError(current["error"])
                print("Policy reloaded.")
                return
        raise RuntimeError("No acknowledgement yet; inspect the launcher terminal")
    with (RUN / "lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if (RUN / "state.json").exists():
            raise ValueError("Previous state exists; use stop before starting again")
        (RUN / "dns").mkdir(mode=0o750, exist_ok=True)
        os.chown(RUN / "dns", pwd.getpwnam("nobody").pw_uid, 0)
        run_hotspot()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError, TypeError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
