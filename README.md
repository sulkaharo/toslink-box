# toslink-box

**Get the audio out of a TV's optical socket and into a computer — including Dolby Digital
surround, intact.**

TVs, set-top boxes, consoles and CD players almost all have a TOSLINK optical output. Computers
almost never have an optical *input*. This is a small board that fixes that, plus the software to
make sense of what arrives.

Under €15 of parts and an evening.

## Why this is harder than it sounds

An optical cable carries one of two completely different things:

* **Stereo PCM** — ordinary audio, easy.
* **A compressed Dolby Digital or DTS bitstream** — 5.1 surround, packed into what looks like a
  two-channel 16-bit stream. This is what a TV sends for films and TV drama.

The second one is why cheap USB "optical input" adapters are useless for surround. Anything in the
path that helpfully resamples, adjusts volume, dithers or converts formats turns a bitstream into
noise — **and reports success while doing it.** So the whole design principle here is that nothing
touches the bytes until a decoder has them.

## What you get

| | |
|---|---|
| [`hardware/`](hardware/) | Two board designs. One optical jack, two capacitors, and either one wire or three |
| [`pico/`](pico/) | Firmware for the Raspberry Pi Pico board: locks to the signal, reports the sample rate, names the encoding, and streams the payload to a host |
| [`pico/listen.py`](pico/listen.py) | Listen to the optical input on your computer, or record it to a WAV. The quickest way to confirm the whole chain works |
| [`decoder/`](decoder/) | `spdif-deframe` — turns a Dolby Digital bitstream into multichannel PCM, ~19 ms faster than ffmpeg's equivalent |

```sh
git clone https://github.com/sulkaharo/toslink-box && cd toslink-box
./build.sh                    # builds whatever your machine has toolchains for

./pico/listen.py              # hear the optical input, live
./pico/listen.py --out cap.wav --seconds 10        # or record it

# and for a compressed 5.1 bitstream, decode it to multichannel PCM
arecord -D hw:2,0 -f S16_LE -c2 -r48000 | ./decoder/spdif-deframe --channels 6 > pcm.raw
```

## Which board

**Board A — a Raspberry Pi Pico over USB.** The Pico does the S/PDIF decoding itself and connects
by USB, so **it is not Pi-specific and not Linux-specific**: it works with any computer that has a
USB port — Linux, macOS, Windows, a Raspberry Pi of any model, a NAS, a router. Nothing is wired to
the host at all, so nothing can conflict with whatever else is plugged in. This is the one to build
unless you have a specific reason not to.

**Board B — straight into a Raspberry Pi 5's GPIO.** Three wires to the 40-pin header, no
microcontroller and no firmware to flash, but it is **Pi 5 only** (it uses the RP1 chip's
programmable I/O), it needs a recent kernel, and it spends real CPU on the Pi.

Full comparison in [`hardware/`](hardware/).

## What this is, and what it is not

**It is a bitstream transport.** The board recovers the S/PDIF stream off the optical link and hands
the payload to software untouched. That payload is either stereo PCM or a compressed surround
bitstream — and *only software can tell which*, by looking for the burst headers inside it.

So the board never interprets what it carries, and **it is not a sound card and will not become
one.** That is the design, not a shortfall. An interface presenting itself as an ordinary audio
input invites the operating system to resample it, apply volume to it or convert its format, and
doing any of that to a bitstream turns it into noise while every component reports success. The
whole point is to get bytes to a decoder with nothing in between.

The same reason is why the decoder is a separate program you point at the stream, rather than
something the board does: decoding is a software concern with codec libraries and licensing behind
it, and it does not belong in a receiver.

**Working today:** both boards. The firmware locks to the signal, reports the measured sample rate,
counts surround bursts, names the encoding, watches the side channels, and streams the payload to
your computer so you can listen. The decoder turns a compressed 5.1 bitstream into multichannel PCM
and is tested against real AC-3.

**Missing:** a USB endpoint that presents the payload to an ordinary capture API, so that programs
expecting a capture device can read it without going through `listen.py`. That is a convenience for
*transport* — it would still be a payload rather than "audio in", and `hw:` rather than `plughw:`
would still be mandatory, for the same reason as above. Board B already has it: the payload arrives
on a pipe.

None of that is in the way of using this. `listen.py` plays the input or records it to a WAV, and
the decoder takes a file or a pipe, so both boards are usable as they stand.

## Licensing

Apache-2.0. Nothing is bundled: [`pico_spdif_rx`](https://github.com/elehobica/pico_spdif_rx)
(BSD-2-Clause) and the Pico SDK are fetched by `pico/build.sh`.

**No audio codec is bundled.** Bitstreams are carried untouched and the decoding is libavcodec's,
so no codec implementation ships with this project. Which payload types this build hands to a
decoder is one configuration table in [`decoder/policy.py`](decoder/policy.py) — conservative by
default, and yours to adjust.

## Troubleshooting

**Nothing locks, and the pin reads low.** `ORJ-8 pin 1 is Vout, not Vcc.` Port facing you, pins
down, they read 3-2-1 left to right, so Vcc is on your left. Feeding pin 1 with 3V3 can destroy the
part, which is why bring-up check 2 powers it through 100 Ω.

**Reversing the jack's orientation moves the capacitors as well as the wires.** GND is the middle
pin, so a mirror swaps the two outer ones. Move only the wires and C1's 100 nF lands across the
signal — 0.26 Ω at the 6.144 MHz cell rate, and every AC-3 frame dies.

**A pin that reads low under a pull-up may be a capacitor, not a short.** Against a ~55 kΩ pad
pull-up, 100 nF takes about 5.5 ms to charge; a probe that settles for 50 µs reports a dead short.

**The SDK's first link fails on `nosys.specs`.** Homebrew's `arm-none-eabi-gcc` ships the compiler
with no target libraries — no newlib, no `libc.a`, no spec files — and there is no newlib formula to
add. `pico/build.sh` fetches ARM's own tarball instead, needing no admin password.

**`cp` of a `.uf2` to the BOOTSEL volume fails with ENXIO** on macOS 26, where it mounts through
FSKit, while the volume stays readable and `ioreg` still shows `RP2 Boot`. Use `picotool load`;
`pico/build.sh --flash` does.

**The link dies on `-lpico_audio_32b`** after every object compiles, which reads like a broken
toolchain. `pico_spdif_rx` declares that dependency unconditionally and never uses a symbol from it;
`pico/CMakeLists.txt` satisfies it with an empty interface target.

**A charge-only micro-USB lead enumerates as nothing at all**, which is indistinguishable from
broken firmware. Check the lead before suspecting the board.

**Playback crackles but a recording of the same stream is clean.** The player's buffer is too small
for the host's scheduling jitter — writing a file has no real-time deadline, so it hides this.
`listen.py` defaults to 128 B recording and 4096 B playing for that reason.

**Changing the test-vector generator's byte order cannot reveal a byte-swap bug in the decoder**,
because the two conventions cancel. When testing a deliberate break, assert that the patch landed
and that the rebuild happened.
