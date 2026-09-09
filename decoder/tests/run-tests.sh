#!/usr/bin/env bash
# End-to-end tests for spdif-deframe, against REAL AC-3 produced by libavcodec's encoder.
#
# The three that matter are the three ways this fails silently: a byte-swap error (decodes nothing
# but reports success), a policy bypass (a rejected type reaching the output), and a fail-open
# default (an unrecognised type being decoded anyway).
set -uo pipefail
cd "$(dirname "$0")"
D=../spdif-deframe
V=./make-vectors
fail=0
ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

# 1. Real AC-3 decodes, and to the right number of samples.
#    20 bursts x 1536 samples x 6 ch x 2 bytes = 368640 bytes exactly.
$V --type 1 --frames 20 2>/dev/null > ac3.raw
n=$($D --channels 6 < ac3.raw | wc -c | tr -d ' ')
[ "$n" = "368640" ] && ok "real AC-3 decodes to 368640 bytes (20 x 1536 x 6ch)" \
                     || bad "AC-3 decode gave $n bytes, expected 368640"

# 2. And it is not silence -- a deframer with the byte swap backwards finds no syncword, decodes
#    nothing, and exits 0. Sample count alone would not catch that; this does.
peak=$($D --channels 6 < ac3.raw | od -An -tu2 -v | tr -s ' ' '\n' | grep -v '^$' \
       | awk '{v=$1; if (v>32767) v=65536-v; if (v>m) m=v} END{print m+0}')
[ "$peak" -gt 1000 ] && ok "decoded audio is real, peak $peak" \
                      || bad "decoded output is silent (peak $peak) -- suspect the byte swap"

# 3. A type the policy rejects produces NO output. E-AC-3 (0x15) is in the table as REJECT.
$V --type 0x15 --frames 10 2>/dev/null > eac3.raw
n=$($D --channels 6 < eac3.raw 2>/dev/null | wc -c | tr -d ' ')
[ "$n" = "0" ] && ok "rejected type (E-AC-3) emits nothing" \
               || bad "rejected type emitted $n bytes -- the policy is being bypassed"

# 4. A type nobody has considered produces NO output. Fail-safe, not fail-open.
$V --type 0x1E --frames 10 2>/dev/null > unk.raw
n=$($D --channels 6 < unk.raw 2>/dev/null | wc -c | tr -d ' ')
[ "$n" = "0" ] && ok "unrecognised type (0x1e) emits nothing" \
               || bad "unrecognised type emitted $n bytes -- the default is fail-OPEN"

# 5. Output width is fixed regardless of what the stream decodes to, so a reader is never broken
#    mid-stream by a track change.
n=$($D --channels 2 < ac3.raw | wc -c | tr -d ' ')
[ "$n" = "122880" ] && ok "--channels 2 truncates to 122880 bytes" \
                    || bad "--channels 2 gave $n bytes, expected 122880"

rm -f ac3.raw eac3.raw unk.raw
[ "$fail" = 0 ] && echo "all decoder tests passed" || echo "DECODER TESTS FAILED"
exit "$fail"
