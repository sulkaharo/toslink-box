#!/usr/bin/env python3
"""Listen to the TOSLINK input through the Pico, in real time.

    ./pico/listen.py                  play through ffplay
    ./pico/listen.py --out cap.wav    record instead of playing
    ./pico/listen.py --rate 44100     if the source is not 48 kHz (the firmware prints `nominal`)

The firmware sits in text mode reporting once a second. Sending 's' switches it to raw interleaved
S16_LE stereo; this sends that, discards the text still in flight by scanning for the magic marker,
and shovels the rest into ffplay.

WHAT THIS IS AND IS NOT. It is the ears test -- every counter in the firmware can read clean while
the audio is wrong, so nothing counts as working until it has been heard. It is NOT a timing
measurement of any kind: a serial byte pipe has no clocking contract, the source's crystal and
ffplay's assumed rate drift apart, and nothing corrects it. Expect the buffer to creep over a long
listen. A real USB audio endpoint is the one with a clocking story.
"""
import argparse, glob, subprocess, sys, termios, time

MAGIC = bytes([0xA5]) * 16


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("no Pico serial port found (looked for /dev/cu.usbmodem* and /dev/ttyACM*)")
    return ports[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--out", default=None, help="write a WAV instead of playing")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = until ^C)")
    ap.add_argument("--sink", choices=("sox", "ff"), default="sox")
    # Latency. Both of these default to values chosen for throughput, not delay, and together they
    # were 64 ms of the ~85 ms end-to-end: sox's --buffer defaults to 8192 bytes (42.7 ms at
    # 192 kB/s) and reading in 4096-byte chunks adds another 21.3 ms. Neither buys anything here --
    # the CDC FIFO is 64 bytes and TinyUSB is polled every 1 ms regardless.
    # Defaults are the MEASURED optimum, not the smallest possible. Backlog in the player, three
    # configurations, 5-8 s each on a 48 kHz source:
    #     --buffer 8192 (sox default) + 4096 chunk    ~64 ms of the ~85 ms end-to-end
    #     --buffer 512  + 256 chunk                   2.2 ms
    #     --buffer 128  + 64 chunk                    1.0 ms   <- best
    #     --buffer 64   + 32 chunk                    3.3 ms   <- WORSE
    # The curve turns because smaller buffers mean more syscalls and more scheduling jitter, so the
    # player has to hold more to avoid underrunning. Smaller is not lower past this point.
    # No single default: the two modes have opposite needs.
    #   --out  writes a file with no real-time deadline, so 128 B is free and latency is moot.
    #   play   must feed CoreAudio on time, and 128 B is 0.67 ms of margin against macOS
    #          scheduling jitter -- which is not a judgement call, it is arithmetic. That is what
    #          the audible noise was: underrun clicks. Recording stayed clean throughout, which is
    #          why the meters never showed it and only ears did.
    ap.add_argument("--buffer", type=int, default=None,
                    help="player buffer in bytes (default 4096 playing, 128 recording)")
    ap.add_argument("--chunk", type=int, default=64, help="serial read size in bytes")
    args = ap.parse_args()

    if args.buffer is None:
        args.buffer = 128 if args.out else 4096
    port = args.port or find_port()
    print(f"port {port} @ {args.rate} Hz", file=sys.stderr)

    fd = open(port, "r+b", buffering=0)
    # Raw mode, or the tty layer eats bytes and translates others.
    attrs = termios.tcgetattr(fd)
    termios.tcsetattr(fd, termios.TCSANOW, attrs[:3] + [attrs[3] & ~(termios.ICANON | termios.ECHO)] + attrs[4:])

    fd.write(b"s")
    fd.flush()

    # Discard the text still in flight. The firmware emits MAGIC once, immediately before audio.
    print("syncing…", file=sys.stderr)
    window, deadline = b"", time.time() + 5
    while MAGIC not in window:
        if time.time() > deadline:
            sys.exit("never saw the stream marker - is the firmware the streaming build?")
        window = (window + fd.read(64))[-4096:]
    audio_tail = window.split(MAGIC, 1)[1]

    # sox rather than ffmpeg/ffplay. Measured 2026-09-09 on this Mac: both ffmpeg binaries are
    # broken by a dangling libx265.216.dylib from a partial Homebrew upgrade and exit before
    # reading a byte, which reaches this script only as a BrokenPipeError. sox has no such
    # dependency and does the same job. `--sink ff` forces the ffmpeg path if it is ever repaired.
    raw = ["--buffer", str(args.buffer),
           "-t", "raw", "-r", str(args.rate), "-e", "signed", "-b", "16", "-c", "2", "-"]
    if args.sink == "ff":
        sink = (["ffmpeg", "-loglevel", "error", "-y", "-f", "s16le", "-ar", str(args.rate),
                 "-ch_layout", "stereo", "-i", "-", args.out] if args.out else
                ["ffplay", "-loglevel", "error", "-nodisp", "-autoexit",
                 "-f", "s16le", "-ar", str(args.rate), "-ch_layout", "stereo", "-i", "-"])
    elif args.out:
        sink = ["sox"] + raw + [args.out]
    else:
        # NOT -q: sox's level meter on stderr is the fastest confirmation that audio is actually
        # flowing, and it hides real errors. Verified 2026-09-09 that recording worked while
        # playback had never once been run -- exactly the gap -q would keep hidden.
        sink = ["play"] + raw

    bps = args.rate * 4
    print(f"{'recording to ' + args.out if args.out else 'playing'} — ^C to stop\n"
          f"latency budget: fifo <=4.0 (spdif_rx DMA block, the floor) + pico 0.7 + usb ~1.3"
          f" + chunk {args.chunk/bps*1000:.1f} + player {args.buffer/bps*1000:.1f} ms"
          f" + whatever CoreAudio adds", file=sys.stderr)
    proc = subprocess.Popen(sink, stdin=subprocess.PIPE)
    # t0 AFTER the player exists, not before. `play` spends about a second opening CoreAudio, and
    # counting that as streaming time understated the rate by 9% -- 43873 frames/s against a real
    # 47977, which read as dropped samples when nothing had been dropped. The instrument was wrong,
    # not the chain.
    total, t0 = 0, time.time()
    backlog_max, backlog_min, next_sample = 0.0, None, time.time() + 0.5
    try:
        proc.stdin.write(audio_tail)
        total += len(audio_tail)
        while True:
            chunk = fd.read(args.chunk)
            if not chunk:
                break
            proc.stdin.write(chunk)
            total += len(chunk)
            # Downstream backlog, measured rather than assumed: everything written above real
            # time is still sitting in the player's buffers, so it IS the latency they add. The
            # one term neither the firmware nor a spec sheet can tell us.
            now = time.time()
            # Skip the first two seconds. Sampling earlier catches the sync and player start-up,
            # when `total` is still near zero while nominal time has advanced -- which is how this
            # metric came to report a 504 ms deficit on a stream that was delivering 192 kB/s
            # correctly. The instrument was measuring itself, twice now.
            if now >= next_sample and now - t0 > 2.0:
                next_sample = now + 0.5
                ahead = total - bps * (now - t0)
                backlog_max = max(backlog_max, ahead)
                # The MINIMUM is the number that matters for playback. It is the player's margin
                # against scheduling jitter, and when it approaches zero the device has nothing to
                # play and you hear a click. Maximum alone measures latency; minimum measures
                # whether it works.
                backlog_min = ahead if backlog_min is None else min(backlog_min, ahead)
            if args.seconds and now - t0 >= args.seconds:
                break
    except KeyboardInterrupt:
        pass
    finally:
        elapsed = max(time.time() - t0, 1e-6)
        # 4 bytes per frame. Well under the nominal rate means the link, not the source, is the limit.
        print(f"\n{total} bytes in {elapsed:.1f}s = {total/elapsed/1000:.0f} kB/s "
              f"({total/4/elapsed:.0f} frames/s, nominal {args.rate})", file=sys.stderr)
        if backlog_max > 0:
            print(f"downstream backlog: peak {backlog_max:.0f} B ({backlog_max/bps*1000:.1f} ms)"
                  f"  margin {backlog_min:.0f} B ({backlog_min/bps*1000:.1f} ms)", file=sys.stderr)
            if backlog_min is not None and backlog_min < bps * 0.005:
                print("  margin under 5 ms: the player may be underrunning, heard as clicks."
                      " Raise --buffer.", file=sys.stderr)
        print("latency and clicks trade directly here. If you hear crackle, raise --buffer;"
              " if it lags, lower it. Ears are the instrument -- sox does not count underruns"
              " and nothing here can see inside CoreAudio.", file=sys.stderr)
        try:
            fd.write(b"x")   # any key leaves stream mode
            fd.flush()
        except OSError:
            pass
        try:
            proc.stdin.close()
            proc.wait(timeout=3)
        except Exception:
            proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
