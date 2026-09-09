#!/usr/bin/env python3
"""Which IEC 61937 payload types this build decodes, and why.

## THIS FILE IS THE SINGLE OWNER OF THAT DECISION

Everything else is generated from it: `gen-policy.py` emits `spdif-policy.h` for the C decoder, and
`make check` fails if the checked-in header has drifted. One table, one place to review, one place
to change -- rather than a scatter of `if (dtype == 1)` that nobody can audit.

## IT IS A LICENSING DECISION, NOT A TECHNICAL FACT

Whether you may decode a codec depends on your jurisdiction and on how you distribute the result.
This file cannot answer that for you. What it can do is make your answer explicit and reviewable.
The defaults below are conservative and the reasoning is written out, but **they are a starting
point for your own decision, not legal advice.**

Considerations, offered as such:

* **Patents and trademarks are different questions**, and conflating them is the usual error. A
  format whose patents have expired can still carry a live trademark: decoding AC-3 is not the same
  question as calling your product "Dolby Digital", and only the second needs a brand licence.
* **AC-3** was specified 1991-94 and its core patents have run out. It is also the only type that
  matters in practice here -- it is what a TV sends down an optical cable for surround.
* **MPEG-1/2 Layer I and II** patents expired long ago. DVB broadcast audio; most TVs transcode it
  before it reaches optical, so this is completeness rather than need.
* **E-AC-3, TrueHD (MAT), DTS-HD, WMA Pro** are substantially newer with live or unclear positions.
  None of them fits down an optical link anyway -- there is not the bandwidth.
* **DTS (Coherent Acoustics)** is the genuinely open question: mid-1990s filings, so the core is
  likely expired, and it is distinct from DTS-HD. Defaulted OFF behind one flag, because guessing
  wrong permissively is the expensive direction and flipping a line is the cheap one.

## AND THE SAFETY RULE, WHICH IS NOT A PREFERENCE

Anything not DECODE is dropped, never passed through as audio. **A raw bitstream burst interpreted
as linear PCM is full-scale white noise** -- the loudest thing the chain can emit, at whatever
volume the listener set for dialogue. So UNKNOWN is a rejection and not a fallback: a type nobody
considered gets silence.
"""
from __future__ import annotations

#: Set True to decode DTS. Your call -- see the notes above.
DECODE_DTS = False

DECODE, SILENT, REJECT, UNKNOWN = "decode", "silent", "reject", "unknown"

#: data type (Pc & 0x1F) -> (name, disposition, libavcodec decoder name or None, reason)
TYPES = [
    (0x00, "NULL",         SILENT, None,   "valid framing, no payload -- not a fault"),
    (0x01, "AC3",          DECODE, "ac3",  "core patents expired; what a TV actually sends"),
    (0x03, "PAUSE",        SILENT, None,   "valid framing, no payload -- not a fault"),
    (0x04, "MPEG1_L1",     DECODE, "mp1",  "expired"),
    (0x05, "MPEG1_L2",     DECODE, "mp2",  "expired"),
    (0x06, "MPEG2_EXT",    REJECT, None,   "multichannel MPEG-2; unclear, and never seen here"),
    (0x07, "MPEG2_AAC",    REJECT, None,   "licensing pools still active"),
    (0x08, "MPEG2_L1_LSF", DECODE, "mp1",  "expired"),
    (0x09, "MPEG2_L2_LSF", DECODE, "mp2",  "expired"),
    (0x0A, "MPEG2_L3_LSF", DECODE, "mp3",  "expired"),
    (0x0B, "DTS1",         REJECT, "dca",  "see DECODE_DTS -- owner's decision"),
    (0x0C, "DTS2",         REJECT, "dca",  "see DECODE_DTS -- owner's decision"),
    (0x0D, "DTS3",         REJECT, "dca",  "see DECODE_DTS -- owner's decision"),
    (0x0E, "ATRAC",        REJECT, None,   "Sony; obscure, unassessed"),
    (0x0F, "ATRAC3",       REJECT, None,   "Sony; obscure, unassessed"),
    (0x11, "DTS_HD",       REJECT, None,   "live patents; cannot fit down optical"),
    (0x13, "WMAPRO",       REJECT, None,   "Microsoft"),
    (0x15, "EAC3",         REJECT, None,   "live patents; cannot fit down optical"),
    (0x16, "MAT",          REJECT, None,   "Dolby TrueHD; live patents"),
]

_DTS = (0x0B, 0x0C, 0x0D)


def disposition(dtype: int) -> str:
    """DECODE / SILENT / REJECT / UNKNOWN. Anything unlisted is UNKNOWN, which is a rejection."""
    t = dtype & 0x1F
    if t in _DTS:
        return DECODE if DECODE_DTS else REJECT
    for code, _name, disp, _codec, _why in TYPES:
        if code == t:
            return disp
    return UNKNOWN


def codec_name(dtype: int):
    """The libavcodec decoder for this type, or None if it is not decoded in this build."""
    if disposition(dtype) != DECODE:
        return None
    for code, _name, _disp, codec, _why in TYPES:
        if code == (dtype & 0x1F):
            return codec
    return None


def type_name(dtype: int) -> str:
    for code, name, _d, _c, _w in TYPES:
        if code == (dtype & 0x1F):
            return name
    return f"0x{dtype & 0x1F:02x}"


if __name__ == "__main__":
    print(f"{'code':5} {'name':14} {'disposition':12} codec    reason")
    for code, name, _d, _c, why in TYPES:
        print(f"0x{code:02x}  {name:14} {disposition(code):12} "
              f"{(codec_name(code) or '-'):8} {why}")
