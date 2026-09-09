#!/usr/bin/env bash
# Build (and optionally flash) the Pico TOSLINK firmware.
#
#   ./pico/build.sh            build only
#   ./pico/build.sh --flash    build, then copy the .uf2 to a Pico in BOOTSEL
#
# Needs cmake and picotool:  brew install cmake picotool
#
# It does NOT use Homebrew's arm-none-eabi-gcc. Measured 2026-09-09: that formula ships the
# compiler with no target libraries at all -- $(brew --prefix arm-none-eabi-gcc)/arm-none-eabi/lib
# does not exist, so there is no newlib, no libc.a and no spec files, and the SDK's very first
# link fails with
#
#     arm-none-eabi-gcc: fatal error: cannot read spec file 'nosys.specs'
#
# There is no arm-none-eabi-newlib formula to add alongside it. The cask gcc-arm-embedded would
# work but installs a .pkg and wants an admin password. So this fetches ARM's own prebuilt tarball
# into the deps directory instead: no sudo, no PATH surprises, and the version is pinned here.
#
# The out-of-tree repos and the toolchain are all fetched on first run. They live OUTSIDE this repo
# on purpose -- a vendored SDK is 400 MB of someone else's git history and goes stale silently.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS="${TOSLINK_PICO_DEPS:-$HOME/dev/pico}"
export PICO_SDK_PATH="${PICO_SDK_PATH:-$DEPS/pico-sdk}"
export PICO_SPDIF_RX_PATH="${PICO_SPDIF_RX_PATH:-$DEPS/pico_spdif_rx}"
SDK_TAG="${PICO_SDK_TAG:-2.2.0}"
ARM_TAG="${ARM_TOOLCHAIN_TAG:-14.3.rel1}"

mkdir -p "$DEPS"

# ARM's toolchain, which unlike Homebrew's actually carries newlib. See the note above.
if [ -z "${PICO_TOOLCHAIN_PATH:-}" ]; then
    ARM_DIR="$DEPS/arm-toolchain"
    if [ ! -x "$ARM_DIR/bin/arm-none-eabi-gcc" ]; then
        case "$(uname -s)-$(uname -m)" in
            Darwin-arm64) HOST=darwin-arm64 ;;
            Darwin-x86_64) HOST=darwin-x86_64 ;;
            Linux-aarch64) HOST=aarch64 ;;
            Linux-x86_64) HOST=x86_64 ;;
            *) echo "no known ARM toolchain build for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
        esac
        NAME="arm-gnu-toolchain-$ARM_TAG-$HOST-arm-none-eabi"
        echo "fetching $NAME (about 130 MB) into $DEPS"
        curl -fsSL --max-time 900 -o "$DEPS/arm-tc.tar.xz" \
            "https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_TAG/binrel/$NAME.tar.xz"
        tar xf "$DEPS/arm-tc.tar.xz" -C "$DEPS"
        rm -f "$DEPS/arm-tc.tar.xz"
        ln -sfn "$NAME" "$ARM_DIR"
    fi
    export PICO_TOOLCHAIN_PATH="$ARM_DIR"
fi
if [ ! -d "$PICO_SDK_PATH/.git" ]; then
    echo "cloning pico-sdk $SDK_TAG into $PICO_SDK_PATH"
    git clone -q --depth 1 --branch "$SDK_TAG" https://github.com/raspberrypi/pico-sdk.git "$PICO_SDK_PATH"
    git -C "$PICO_SDK_PATH" submodule update --init --depth 1 lib/tinyusb
fi
if [ ! -d "$PICO_SPDIF_RX_PATH/.git" ]; then
    echo "cloning pico_spdif_rx into $PICO_SPDIF_RX_PATH"
    git clone -q --recursive https://github.com/elehobica/pico_spdif_rx.git "$PICO_SPDIF_RX_PATH"
fi

BUILD="$HERE/build"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -G Ninja >/dev/null
cmake --build "$BUILD" -j

UF2="$BUILD/toslink-rx.uf2"
ls -l "$UF2"

if [ "${1:-}" = "--flash" ]; then
    # picotool, NOT a file copy to /Volumes/RPI-RP2.
    #
    # Measured 2026-09-09 on macOS 26 (Darwin 25.6.0): the BOOTSEL volume mounts through the new
    # FSKit msdos driver --
    #     /dev/disk8s1 on /Volumes/RPI-RP2 (msdos, local, nodev, nosuid, noowners, noatime, fskit)
    # -- and a `cp` of the .uf2 onto it fails with
    #     cp: /Volumes/RPI-RP2/toslink-rx.uf2: Device not configured
    # while the volume stays readable and `ioreg -p IOUSB` still shows "RP2 Boot" (0x2E8A:0x0003).
    # So the device is fine and it is the mass-storage write path that is broken. picotool talks to
    # the bootloader directly and worked first time.
    if ! command -v picotool >/dev/null; then
        echo "picotool not found: brew install picotool" >&2
        exit 1
    fi
    echo "flashing with picotool"
    if ! picotool load -x "$UF2"; then
        cat >&2 <<'MSG'

picotool could not find a device in BOOTSEL mode.

  - Hold the BOOTSEL button while plugging the USB lead in, then run this again.
  - A Pico running an app that has HUNG cannot be rebooted into BOOTSEL by picotool,
    because that path needs USB configuration to have completed. Physical BOOTSEL is
    the only way back.
  - If nothing ever appears, suspect the LEAD: a charge-only micro-USB cable
    enumerates as nothing at all, which looks exactly like broken firmware.
MSG
        exit 1
    fi
    echo
    echo "flashed. It reboots into the app and appears as USB serial:"
    echo "  macOS:  ls /dev | grep usbmodem     then   screen /dev/tty.usbmodem<id> 115200"
    echo "  Linux:  screen /dev/ttyACM0 115200"
    echo
    echo "If no serial node appears, read the onboard LED instead -- see the stage codes in"
    echo "toslink-rx.c. A frozen LED means it hung at the stage it last blinked."
fi
