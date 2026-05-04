#!/usr/bin/env python3
# Usage:
#   ./kicad-viewport.py lsw5-esp32.kicad_pro
#   ./kicad-viewport.py lsw5-esp32.kicad_pro readme

import argparse
import json
import math
from pathlib import Path
import sys

ROTATION_FIELDS = ("xx", "xy", "xz", "yx", "yy", "yz", "zx", "zy", "zz")


def viewport_rotation_matrix(viewport):
    missing = [field for field in ROTATION_FIELDS if field not in viewport]

    if missing:
        raise SystemExit("Viewport does not contain KiCad 3D matrix fields: " + ", ".join(missing))

    for field in ROTATION_FIELDS:
        if not isinstance(viewport[field], (int, float)):
            raise SystemExit(f"Viewport field {field!r} is not numeric")

    # KiCad's saved 3D viewport is a camera transform, but kicad-cli rotates the board, so we want the inverse of the saved matrix.
    # For an orthonormal matrix, that is just the transpose.
    return [
        [viewport["xx"], viewport["yx"], viewport["zx"]],
        [viewport["xy"], viewport["yy"], viewport["zy"]],
        [viewport["xz"], viewport["yz"], viewport["zz"]],
    ]


def matrix_to_kicad_rotate_deg(matrix):
    # KiCad's CLI rotation is specified in X, Y, Z order.
    # The saved 3D viewport is stored as a Rx * Ry * Rz camera matrix in xx..zz.
    sy = max(-1.0, min(1.0, matrix[0][2]))
    y = math.asin(sy)
    cy = math.cos(y)

    if abs(cy) > 1e-9:
        x = math.atan2(-matrix[1][2], matrix[2][2])
        z = math.atan2(-matrix[0][1], matrix[0][0])
    else:
        z = 0.0
        sign = 1.0 if sy >= 0 else -1.0
        x = math.atan2(sign * matrix[1][0], matrix[1][1])

    return tuple(clean_degrees(math.degrees(v)) for v in (x, y, z))


def clean_degrees(value):
    if abs(value) < 0.0005:
        return 0.0

    return value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("project", type=Path, help=".kicad_pro file")
    ap.add_argument("viewport", nargs="?", help="viewport name, omit to list")
    args = ap.parse_args()

    data = json.loads(args.project.read_text())
    viewports = data.get("board", {}).get("3dviewports", [])

    if not viewports:
        raise SystemExit("No /board/3dviewports found.")

    if args.viewport is None:
        print("Saved viewports:")
        for vp in viewports:
            print(" -", vp.get("name", "<unnamed>"))
        return

    matches = [vp for vp in viewports if vp.get("name") == args.viewport]
    if not matches:
        raise SystemExit(f"No viewport named {args.viewport!r}")

    vp = matches[0]
    matrix = viewport_rotation_matrix(vp)
    rotate = matrix_to_kicad_rotate_deg(matrix)

    print(f"# viewport: {args.viewport}", file=sys.stderr)
    print(f"--rotate {rotate[0]:.3f},{rotate[1]:.3f},{rotate[2]:.3f}")


if __name__ == "__main__":
    main()
