# SPDX-License-Identifier: GPL-3.0-or-later
"""Save unmodified HTTP bodies and compare full files or ranges with the archive."""

import hashlib
import json
import logging
import re
from datetime import UTC, datetime
from pathlib import Path
from urllib.parse import unquote, urlsplit

from mitmproxy import ctx, http

LOGGER = logging.getLogger(__name__)
UPSTREAM = "eu2-fwupgrade.deyecloud.com"


def compare_archive(body: bytes, status: int, content_range: str, archive: Path) -> dict:
    """Only claim a whole-file match when the response covers the whole file."""
    offset, total, complete = 0, len(body), status == 200
    if status == 206:
        match = re.fullmatch(r"bytes (\d+)-(\d+)/(\d+)", content_range)
        if not match:
            return {"comparison": "invalid Content-Range", "matches": []}
        offset, end, total = map(int, match.groups())
        if not 0 <= offset <= end < total or end - offset + 1 != len(body):
            return {"comparison": "inconsistent Content-Range/body length", "matches": []}
        complete = offset == 0 and end + 1 == total
    elif status != 200:
        return {"comparison": "not a file response", "matches": []}
    matches = []
    for path in sorted(archive.rglob("*.bin")):
        if path.stat().st_size != total:
            continue
        with path.open("rb") as stream:
            stream.seek(offset)
            candidate = stream.read(len(body))
        if candidate == body:
            matches.append(str(path.relative_to(archive)))
    return {
        "comparison": "whole file" if complete else "partial range only",
        "offset": offset,
        "total_size": total,
        "matches": matches,
    }


class FirmwareCapture:
    def load(self, loader):
        loader.add_option(
            "fw_capture_dir", str, "", "Directory for captured firmware bodies and metadata"
        )
        loader.add_option(
            "fw_archive_dir", str, "", "Local DeyeFirmware archive to compare against"
        )

    def request(self, flow: http.HTTPFlow):
        # This listener is only for download inspection, with one fixed upstream.
        if flow.request.host.lower() != UPSTREAM or flow.request.method not in ("GET", "HEAD"):
            flow.response = http.Response.make(
                403, b"Firmware download proxy: GET/HEAD only, fixed upstream.\n"
            )

    def response(self, flow: http.HTTPFlow):
        if flow.response is None or flow.request.host.lower() != UPSTREAM:
            return
        if flow.request.method != "GET":
            return
        body = flow.response.raw_content
        if body is None:
            LOGGER.warning("No buffered response body for %s", flow.id)
            return
        root = Path(ctx.options.fw_capture_dir)
        root.mkdir(parents=True, exist_ok=True)
        name = Path(unquote(urlsplit(flow.request.url).path)).name or "response"
        name = re.sub(r"[^A-Za-z0-9_.-]", "_", name)[:120]
        stem = f"{datetime.now(UTC):%Y%m%dT%H%M%S.%fZ}-{flow.id[:8]}-{name}"
        body_path = root / f"{stem}.body.bin"
        body_path.write_bytes(body)
        digest = hashlib.sha256(body).hexdigest()
        archive = Path(ctx.options.fw_archive_dir) if ctx.options.fw_archive_dir else None
        result = (
            compare_archive(
                body,
                flow.response.status_code,
                flow.response.headers.get("content-range", ""),
                archive,
            )
            if archive is not None
            else {"comparison": "not requested", "matches": []}
        )
        metadata = {
            "url": flow.request.url,
            "method": flow.request.method,
            "request_range": flow.request.headers.get("range"),
            "status": flow.response.status_code,
            "response_headers": list(flow.response.headers.items(multi=True)),
            "body_file": body_path.name,
            "body_size": len(body),
            "sha256": digest,
            "archive": str(archive.resolve()) if archive is not None else None,
            **result,
        }
        (root / f"{stem}.json").write_text(json.dumps(metadata, indent=2) + "\n")
        LOGGER.info(
            "Captured %s: %d bytes SHA256=%s; %s; archive matches=%s",
            body_path.name,
            len(body),
            digest,
            result["comparison"],
            result["matches"],
        )


addons = [FirmwareCapture()]
