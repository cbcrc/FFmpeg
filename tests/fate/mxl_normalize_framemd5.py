#!/usr/bin/env python3
"""Normalize and group per-stream packet timestamps in framemd5 output.

Read framemd5 from stdin and write normalized output to stdout. Each
stream's first PTS is subtracted from both DTS and PTS; packets are then
grouped by stream index while preserving their order within each stream.
"""

import sys
from collections import defaultdict


def main():
    headers = []
    packets = defaultdict(list)
    first_pts = {}

    for line in sys.stdin:
        if not line.lstrip()[:1].isdigit():
            headers.append(line)
            continue

        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 6:
            raise ValueError(f"invalid framemd5 packet record: {line.rstrip()}")

        stream, dts, pts, duration, size, digest = fields
        stream, dts, pts = int(stream), int(dts), int(pts)
        first_pts.setdefault(stream, pts)
        offset = first_pts[stream]

        packets[stream].append(
            f"{stream}, {dts - offset:10d}, {pts - offset:10d}, "
            f"{int(duration):8d}, {int(size):8d}, {digest}\n"
        )

    sys.stdout.write(
        "# Normalized framemd5 output for MXL FATE testing\n"
        "# DTS and PTS are offset by the first PTS of each stream.\n"
        "# Packets are grouped by stream index, preserving per-stream order.\n"
        "# Packet duration, size, and hash are unchanged.\n"
    )
    sys.stdout.writelines(headers)
    for stream in sorted(packets):
        sys.stdout.writelines(packets[stream])


if __name__ == "__main__":
    main()
