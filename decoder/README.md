# spdif-deframe

IEC 61937 deframing plus in-process decode. Reads the S/PDIF subframe payload as S16_LE stereo on
stdin and writes interleaved PCM on stdout — a drop-in for `ffmpeg -f spdif -i -`, about **19 ms
faster**, and with an explicit policy about which payload types it will decode.

```sh
make                                    # build
make check                              # policy drift check + end-to-end tests
./spdif-deframe --list                  # what this build decodes, and what it drops

arecord -D hw:2,0 -f S16_LE -c2 -r48000 | ./spdif-deframe --channels 6 | your-sink
```

**`hw:`, never `plughw:`.** ALSA's plug layer will happily rate-convert and reformat, and doing that
to a bitstream turns every burst into noise while every component reports success.

## Why it is faster

`ffmpeg -f spdif -i -` spends ~19 ms on something unrelated to decoding. Its demuxer finds Pa/Pb,
reads `Pd` bytes of payload, then calls

```c
avio_skip(pb, offset - pkt->size - BURST_HEADER_SIZE);
```

**before** returning the packet — it skips the padding out to the next burst boundary first, and on
a pipe that skip blocks until those bytes arrive. At 640 kbps an AC-3 burst is about 2560 bytes of a
6144-byte period, so the frame reaches the decoder ~32 ms after its first byte instead of the ~13 ms
at which it was already complete. There is no option to disable it, and `-f ac3` does not help
either: the raw demuxer reads fixed 1024-byte chunks and the last chunk of every frame waits on
bytes of the next one.

The **decode** is still libavcodec's. dialnorm, DRC and channel order are exactly what a hand-rolled
decoder gets wrong for months. Only the framing is ours, because that is the part costing latency.

## Which formats it decodes

`policy.py` is the single source for that, and `spdif-policy.h` is **generated** from it —
`make check` fails if the header has drifted, so there is one table to change rather than a scatter
of `if (dtype == 1)`. `./spdif-deframe --list` prints what the current build will do.

Defaults are conservative. Adjust the table to suit your requirements; the DTS types sit behind a
single `DECODE_DTS` flag.

## The safety rule, which is not a preference

Anything not `DECODE` is **dropped, never emitted as audio**. A raw bitstream burst interpreted as
linear PCM is full-scale white noise, at whatever volume the listener set for dialogue. So an
unrecognised type is a rejection and not a fallback.

Dropped bursts produce no output, so the caller sees a gap rather than silence. That is honest for a
tool that does not know your timing model; fill it at the caller if you need continuity.

## Tests

Five, run by `make check`, against **real AC-3 produced by libavcodec's encoder** rather than a
fabricated payload — because a fabricated payload is exactly where a byte-swap error hides.

They cover the three ways this fails silently: a byte-swap error (decodes nothing, exits 0), a
policy bypass (a rejected type reaching the output), and a fail-open default (an unrecognised type
decoded anyway). Each was watched failing on deliberately broken code before being trusted.
