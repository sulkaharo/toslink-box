#!/usr/bin/env bash
# Build everything buildable on this host. Each component is independent -- the decoder needs only
# libavcodec, the firmware needs the Pico SDK -- so a missing toolchain skips that part rather than
# failing the run.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
rc=0

echo "== decoder =="
if pkg-config --exists libavcodec 2>/dev/null; then
    make -C decoder && make -C decoder check || rc=1
else
    echo "   skipped: no libavcodec (brew install ffmpeg / apt install libavcodec-dev)"
fi

echo
echo "== pico firmware =="
if command -v cmake >/dev/null; then
    ./pico/build.sh || rc=1
else
    echo "   skipped: no cmake (brew install cmake picotool)"
fi

echo
[ "$rc" = 0 ] && echo "OK" || echo "FAILED"
exit "$rc"
