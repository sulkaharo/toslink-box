#!/usr/bin/env python3
"""Which IEC 61937 payload types this build hands to a decoder.

## THIS FILE IS THE SINGLE SOURCE

Everything else is generated from it: `gen-policy.py` emits `spdif-policy.h` for the C decoder, and
`make check` fails if the checked-in header has drifted. One table, one place to change, rather than
a scatter of `if (dtype == 1)` across the source.

Adjust it to suit your own requirements.

## THE FAIL-SAFE RULE

Anything not `DECODE` is dropped, never passed through as audio. **A raw bitstream burst
interpreted as linear PCM is full-scale white noise** -- the loudest thing the chain can emit, at
whatever volume the listener set for dialogue. So `UNKNOWN` is a rejection and not a fallback: a
type absent from the table gets silence.
"""
from __future__ import annotations

#: Enable the DTS data types.
DECODE_DTS = False

DECODE, SILENT, REJECT, UNKNOWN = "decode", "silent", "reject", "unknown"

#: data type (Pc & 0x1F) -> (name, disposition, libavcodec decoder name or None, note)
TYPES = [
    (0x00, "NULL",         SILENT, None,   "valid framing, no payload"),
    (0x01, "AC3",          DECODE, "ac3",  "what a TV sends for surround over optical"),
    (0x03, "PAUSE",        SILENT, None,   "valid framing, no payload"),
    (0x04, "MPEG1_L1",     DECODE, "mp1",  "broadcast audio; usually transcoded before optical"),
    (0x05, "MPEG1_L2",     DECODE, "mp2",  "broadcast audio; usually transcoded before optical"),
    (0x06, "MPEG2_EXT",    REJECT, None,   "not seen on an optical link"),
    (0x07, "MPEG2_AAC",    REJECT, None,   "not seen on an optical link"),
    (0x08, "MPEG2_L1_LSF", DECODE, "mp1",  "broadcast audio"),
    (0x09, "MPEG2_L2_LSF", DECODE, "mp2",  "broadcast audio"),
    (0x0A, "MPEG2_L3_LSF", DECODE, "mp3",  "broadcast audio"),
    (0x0B, "DTS1",         REJECT, "dca",  "off unless DECODE_DTS"),
    (0x0C, "DTS2",         REJECT, "dca",  "off unless DECODE_DTS"),
    (0x0D, "DTS3",         REJECT, "dca",  "off unless DECODE_DTS"),
    (0x0E, "ATRAC",        REJECT, None,   "not seen on an optical link"),
    (0x0F, "ATRAC3",       REJECT, None,   "not seen on an optical link"),
    (0x11, "DTS_HD",       REJECT, None,   "needs a high-rate mode optical does not carry"),
    (0x13, "WMAPRO",       REJECT, None,   "not seen on an optical link"),
    (0x15, "EAC3",         REJECT, None,   "needs a high-rate mode optical does not carry"),
    (0x16, "MAT",          REJECT, None,   "needs a high-rate mode optical does not carry"),
]

_DTS = (0x0B, 0x0C, 0x0D)


def disposition(dtype: int) -> str:
    """DECODE / SILENT / REJECT / UNKNOWN. Anything unlisted is UNKNOWN, which is a rejection."""
    t = dtype & 0x1F
    if t in _DTS:
        return DECODE if DECODE_DTS else REJECT
    for code, _name, disp, _codec, _note in TYPES:
        if code == t:
            return disp
    return UNKNOWN


def codec_name(dtype: int):
    """The libavcodec decoder for this type, or None if this build does not decode it."""
    if disposition(dtype) != DECODE:
        return None
    for code, _name, _disp, codec, _note in TYPES:
        if code == (dtype & 0x1F):
            return codec
    return None


def type_name(dtype: int) -> str:
    for code, name, _d, _c, _n in TYPES:
        if code == (dtype & 0x1F):
            return name
    return f"0x{dtype & 0x1F:02x}"


if __name__ == "__main__":
    print(f"{'code':5} {'name':14} {'disposition':12} codec    note")
    for code, name, _d, _c, note in TYPES:
        print(f"0x{code:02x}  {name:14} {disposition(code):12} "
              f"{(codec_name(code) or '-'):8} {note}")
