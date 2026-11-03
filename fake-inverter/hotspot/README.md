# Isolated inverter hotspot

Temporary Linux access point using NetworkManager, dnsmasq, and nftables. Requires Python 3.11+, NetworkManager, nftables, dnsmasq, iproute2, and `dig` (`bind` on Arch).

Defaults in `hotspot.py`: Wi-Fi `wlan0`, uplink `eth0`, channel 6, subnet `10.77.0.0/24`. Adjust before starting.

## Start and stop

```sh
cp -n hotspot.toml.example hotspot.toml
cp -n policy.conf.example policy.conf
chmod 600 hotspot.toml
# Set SSID/password in hotspot.toml, then:
sudo ./hotspot.py start
```

Leave it in the foreground; Ctrl-C stops it. Without a configured password, startup prompts for one or offers to generate one.

In another terminal, use `sudo ./hotspot.py logs`, `status`, `reload`, or `stop`. `reload` applies policy changes; interface, uplink, and launcher changes require a restart. Invalid reloads leave the previous policy active.

## Traffic policy

With an empty policy, clients get DHCP leases but no DNS answers or Internet access. DNS requests and connection attempts are logged. Add rules to `policy.conf`:

```text
dns collector.example.com 93.184.215.14
dns-pass updates.example.com
allow tcp collector.example.com 10000
allow tcp updates.example.com 443
allow udp 162.159.200.1 123
# Allow this PC to reach one hotspot client:
output-allow tcp 10.77.0.115 80 8899
```

DNS and connection permissions are separate. Names match exactly; resolved IPv4 addresses stay fixed until reload. Connection rules match IP/port, not HTTP hostnames. Private destinations, other LAN/VPN routes, external DNS, and IPv6 are blocked. `output-allow` allows replies from hotspot clients, not connections initiated toward this PC. Removing a rule also revokes matching established connections.

Firewall drops appear in the kernel journal with `INVHOT` prefixes; DHCP/DNS logs are in `/run/inverter-hotspot`. Packet capture:

```sh
sudo tcpdump -ni wlan0 -nn -s0 -U -w /tmp/inverter.pcap
```

## Firmware download capture

Requires mitmproxy. From `fake-inverter`, run as your normal user:

```sh
python3 hotspot/capture_firmware.py
```

Add `--archive` /path/to/[DeyeFirmware](https://github.com/Mights001/DeyeFirmware) for byte comparison, or `--web` for mitmweb at `127.0.0.1:8081`. The proxy listens on `10.77.0.1:8443`; enable it with:

```text
dns-pass eu2-fwupgrade.deyecloud.com
intercept tcp eu2-fwupgrade.deyecloud.com 443 8443
```

A direct `allow` for the same destination/port is rejected. Only intercepted connections reach the proxy; stopping it blocks downloads until it restarts or the policy changes.

LSW5 `LSW5_32_5406_SS_04_00.00.00.11` skips HTTPS server authentication for firmware downloads, so no proxy CA is needed. The proxy verifies the upstream certificate and leaves the separate cloud-control connection alone.

Bodies, flows, and JSON metadata go to `captures/downloads/` (gitignored). Bodies are saved unchanged; URLs and headers may contain tokens. Whole downloads are compared with archive files; HTTP 206 responses are marked as partial matches. Ranges are not assembled and encoded bodies are not decompressed.

## Recovery

The firewall is installed before the AP starts and removed after it stops. Only this launcher's nftables table is changed; previous forwarding/autoconnect settings are restored. Other firewall rules may still block traffic. Flowtables are rejected because they can bypass filtering.

After a crash, run `stop` before restarting. Do not remove the firewall while the AP is active. Add publicly addressed LAN ranges to `SPECIAL` if they are reachable through the default router without explicit routes.

Run `python3 test_hotspot.py` for unprivileged policy checks.
