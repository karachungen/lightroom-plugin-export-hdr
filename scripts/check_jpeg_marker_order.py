#!/usr/bin/env python3
"""Print and validate primary JPEG markers relevant to Ultra HDR interoperability."""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path


STANDALONE = {0x01, *range(0xD0, 0xDA)}


@dataclass(frozen=True)
class Segment:
    marker: int
    offset: int
    payload: bytes

    @property
    def name(self) -> str:
        if self.marker == 0xD8:
            return "SOI"
        if self.marker == 0xDA:
            return "SOS"
        if 0xE0 <= self.marker <= 0xEF:
            return f"APP{self.marker - 0xE0}"
        return {
            0xC0: "SOF0",
            0xC1: "SOF1",
            0xC2: "SOF2",
            0xC4: "DHT",
            0xDB: "DQT",
            0xDD: "DRI",
            0xFE: "COM",
        }.get(self.marker, f"FF{self.marker:02X}")

    @property
    def kind(self) -> str:
        p = self.payload
        if self.marker == 0xE0 and p.startswith(b"JFIF\0"):
            return "JFIF"
        if self.marker == 0xE1 and p.startswith(b"Exif\0\0"):
            return "Exif"
        if self.marker == 0xE1 and p.startswith(b"http://ns.adobe.com/xap/1.0/\0"):
            return "XMP"
        if self.marker == 0xE2 and p.startswith(b"ICC_PROFILE\0"):
            return "ICC"
        if self.marker == 0xE2 and p.startswith(b"MPF\0"):
            return "MPF"
        if self.marker == 0xE2 and p.startswith(b"urn:iso:std:iso:ts:21496:-1\0"):
            return "ISO 21496-1"
        return ""


def parse_to_sos(data: bytes) -> list[Segment]:
    if not data.startswith(b"\xff\xd8"):
        raise ValueError("not a JPEG: missing SOI")
    result = [Segment(0xD8, 0, b"")]
    pos = 2
    while pos < len(data):
        marker_offset = pos
        if data[pos] != 0xFF:
            raise ValueError(f"expected marker at offset {pos}, found 0x{data[pos]:02x}")
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        if pos >= len(data):
            raise ValueError("truncated marker")
        marker = data[pos]
        pos += 1
        if marker == 0x00:
            raise ValueError(f"unexpected stuffed byte before SOS at offset {marker_offset}")
        if marker in STANDALONE or marker in (0xD8, 0xD9):
            result.append(Segment(marker, marker_offset, b""))
            continue
        if pos + 2 > len(data):
            raise ValueError(f"truncated length at offset {marker_offset}")
        length = int.from_bytes(data[pos : pos + 2], "big")
        if length < 2 or pos + length > len(data):
            raise ValueError(f"invalid segment length {length} at offset {marker_offset}")
        payload = data[pos + 2 : pos + length]
        result.append(Segment(marker, marker_offset, payload))
        pos += length
        if marker == 0xDA:
            return result
    raise ValueError("primary JPEG has no SOS")


def validate(segments: list[Segment]) -> list[str]:
    errors: list[str] = []
    jfif = [i for i, s in enumerate(segments) if s.kind == "JFIF"]
    mpf = [i for i, s in enumerate(segments) if s.kind == "MPF"]
    sos = [i for i, s in enumerate(segments) if s.marker == 0xDA]

    if jfif != [1]:
        errors.append("APP0 JFIF must be the first segment immediately after SOI")
    if len(mpf) != 1:
        errors.append(f"expected exactly one APP2 MPF segment, found {len(mpf)}")
    if len(sos) != 1:
        errors.append(f"expected exactly one SOS, found {len(sos)}")
    if len(mpf) == 1 and len(sos) == 1 and mpf[0] + 1 != sos[0]:
        errors.append("APP2 MPF must be immediately before SOS (CIPA DC-007 ordering)")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Print JPEG segments through SOS and check PR #394 ordering"
    )
    parser.add_argument("jpeg", type=Path)
    parser.add_argument("--check", action="store_true", help="exit nonzero on invalid ordering")
    args = parser.parse_args()

    try:
        segments = parse_to_sos(args.jpeg.read_bytes())
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    for segment in segments:
        suffix = f" {segment.kind}" if segment.kind else ""
        print(f"0x{segment.offset:08x}  {segment.name}{suffix}")

    errors = validate(segments)
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1 if args.check else 0
    print("PASS: JFIF is first after SOI and MPF is immediately before SOS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
