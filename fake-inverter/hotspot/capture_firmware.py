#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture the logger's firmware downloads with a fixed-upstream mitmproxy."""

import argparse
import os
import shutil
from datetime import UTC, datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
UPSTREAM = "eu2-fwupgrade.deyecloud.com"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8443, help="local listener (default: 8443)")
    parser.add_argument(
        "--listen", default="10.77.0.1", help="bind address (default: hotspot gateway)"
    )
    parser.add_argument(
        "--archive", type=Path, help="optional firmware archive for byte comparison"
    )
    parser.add_argument(
        "--output", type=Path, help="capture directory; default: timestamp under captures/downloads"
    )
    parser.add_argument(
        "--web", action="store_true", help="use mitmweb with its UI bound to 127.0.0.1:8081"
    )
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("port must be 1024..65535")
    if args.archive is not None and not args.archive.is_dir():
        parser.error(f"archive directory not found: {args.archive}")
    program = "mitmweb" if args.web else "mitmdump"
    if not shutil.which(program):
        parser.error(f"install mitmproxy first ({program} is missing)")
    os.umask(0o077)
    output = (
        args.output
        or HERE.parent / "captures" / "downloads" / f"{datetime.now(UTC):%Y%m%dT%H%M%S.%fZ}"
    ).resolve()
    output.mkdir(parents=True, exist_ok=True)
    command = [
        program,
        "--mode",
        f"reverse:https://{UPSTREAM}@{args.port}",
        "--listen-host",
        args.listen,
        "--set",
        f"confdir={output / 'mitmproxy-config'}",
        "--set",
        f"fw_capture_dir={output}",
        "--set",
        f"fw_archive_dir={args.archive.resolve() if args.archive else ''}",
        "--set",
        "http2=false",
        "--set",
        "body_size_limit=32m",
        "--set",
        "keep_host_header=true",
        "-s",
        str(HERE / "firmware_capture_addon.py"),
        "-w",
        str(output / "flows.mitm"),
    ]
    if args.web:
        command += ["--web-host", "127.0.0.1", "--web-port", "8081", "--no-web-open-browser"]
    print(f"Captures: {output}", flush=True)
    print(f"Upstream: https://{UPSTREAM} (upstream certificate verification enabled)", flush=True)
    print(f"Policy: intercept tcp {UPSTREAM} 443 {args.port}", flush=True)
    os.execvp(program, command)


if __name__ == "__main__":
    main()
