# spdif-deframe

IEC 61937 deframing plus in-process decode. Reads the S/PDIF subframe payload as S16_LE stereo on
stdin and writes interleaved PCM on stdout — a drop-in for `ffmpeg -f spdif -i -` that releases each
frame **as soon as it is complete rather than at the end of its burst period**, and with an explicit
table of which payload types it will decode.

```sh
make                                    # build
make check                              # policy drift check + end-to-end tests
./spdif-deframe --list                  # what this build decodes, and what it drops

arecord -D hw:2,0 -f S16_LE -c2 -r48000 | ./spdif-deframe --channels 6 | your-sink
```

**`hw:`, never `plughw:`.** ALSA's plug layer will happily rate-convert and reformat, and doing that
to a bitstream turns every burst into noise while every component reports success.

## Why it is faster, and by how much

`ffmpeg -f spdif -i -` finds Pa/Pb, reads `Pd` bytes of payload, and then calls

```c
avio_skip(pb, offset - pkt->size - BURST_HEADER_SIZE);
```

**before** returning the packet — it skips the padding out to the next burst boundary first, and on a
pipe that skip blocks until those bytes arrive. So a frame is released at the end of its burst
period, not when it is complete.

S/PDIF is a constant-rate carrier and the burst period is fixed, so the arithmetic is exact. An AC-3
frame is 1536 samples — **32.0 ms** at 48 kHz — and its burst period is `1536 << 2` = 6144 bytes.
The payload occupies `bitrate_kbps × 4` of those bytes, so it has fully arrived that fraction of the
way through:

| AC-3 bit rate | payload | complete at | ffmpeg releases at | difference |
|---|---|---|---|---|
| 640 kbps | 2560 B | **13.3 ms** | 32.0 ms | 18.7 ms |
| 448 kbps | 1792 B | **9.3 ms** | 32.0 ms | 22.7 ms |
| 384 kbps | 1536 B | **8.0 ms** | 32.0 ms | 24.0 ms |
| 256 kbps | 1024 B | **5.3 ms** | 32.0 ms | 26.7 ms |

So this stage releases a frame in **5–13 ms instead of 32** — between 58% and 83% less latency in
the deframing step. Counter-intuitively the *lower* bit rates gain most, because the payload is a
smaller slice of a fixed-size period.

**Two things that figure is not.** It is **derived from the demuxer's behaviour, not measured end to
end** — the table above is arithmetic, and nothing here has been instrumented against a stopwatch.
And it is **one stage of a chain**: what fraction of your total input-to-output latency it represents
depends entirely on your capture buffers, ALSA, the decoder and your output path, none of which this
program touches. If end-to-end latency is what you care about, measure the whole chain.

There is no ffmpeg option for it — the skip is unconditional — and `ffmpeg -f ac3` does not help
either: the raw demuxer reads fixed 1024-byte chunks, so the last chunk of every frame waits on
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
