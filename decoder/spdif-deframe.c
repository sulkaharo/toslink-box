/*
 * spdif-deframe -- deframe IEC 61937 and decode the bitstream in-process, without waiting.
 *
 * Reads the S/PDIF subframe payload on stdin as S16_LE stereo -- which is what ANY front end
 * gives you: `arecord` on a USB receiver, a PIO capture, or a file. Writes interleaved S16_LE PCM
 * on stdout. A drop-in replacement for `ffmpeg -f spdif -i -` that releases a frame as soon as it is
 * complete rather than at the end of its burst period.
 *
 * ## WHY IT EXISTS: ffmpeg's demuxer waits for padding it does not need
 *
 * `ffmpeg -f spdif -i -` finds Pa/Pb, reads `Pd` bytes of payload, and then does
 *
 *     avio_skip(pb, offset - pkt->size - BURST_HEADER_SIZE);
 *
 * BEFORE returning the packet -- it skips the padding out to the next burst boundary first, and on
 * a pipe that skip blocks until those bytes arrive.
 *
 * S/PDIF is a constant-rate carrier and the burst period is fixed, so this is exact arithmetic
 * rather than a measurement. An AC-3 frame is 1536 samples -- 32.0 ms at 48 kHz -- in a period of
 * `1536 << 2` = 6144 bytes, and the payload occupies `bitrate_kbps * 4` of them:
 *
 *     640 kbps   2560 B   complete at 13.3 ms   ffmpeg releases at 32.0 ms
 *     448 kbps   1792 B   complete at  9.3 ms
 *     384 kbps   1536 B   complete at  8.0 ms
 *     256 kbps   1024 B   complete at  5.3 ms
 *
 * So a frame goes to the decoder in 5-13 ms instead of 32, and the LOWER bit rates gain most
 * because the payload is a smaller slice of a fixed period. The decoder has no lookahead: one
 * packet in, 1536 samples out.
 *
 * That figure is one stage of a chain and is not an end-to-end measurement -- what fraction of a
 * given pipeline's latency it represents depends on the capture buffers, ALSA and the output path.
 *
 * There is no option for it -- the skip is unconditional -- and `ffmpeg -f ac3` does not help
 * either, because the raw demuxer reads fixed 1024-byte chunks and the last chunk of every frame
 * waits on bytes of the next one. So the deframing has to be ours.
 *
 * ## WHAT IS STILL FFMPEG'S: the decode
 *
 * libavcodec does the actual decoding, and that is deliberate. dialnorm, DRC, channel order and
 * the rest are precisely what a hand-rolled decoder gets wrong for months. We take over only the
 * framing, which is the part that was costing latency it did not need to.
 *
 * ## WHICH TYPES, AND THE SAFETY RULE
 *
 * `spdif-policy.h` is generated from `policy.py`, the single source for that. Anything not
 * SPDIF_DECODE is DROPPED -- never emitted as audio, because a raw burst interpreted as linear PCM
 * is full-scale white noise at whatever volume the listener set for dialogue. An unrecognised type
 * is a rejection, not a fallback.
 *
 * Dropped bursts produce no output, so the caller sees a gap rather than silence. That is the
 * honest behaviour for a tool that does not know your timing model; if you need continuous output,
 * the caller is the right place to fill it.
 *
 * ## TWO THINGS THAT ARE EASY TO GET WRONG
 *
 * **The payload is byte-swapped.** IEC 61937 carries the bitstream big-endian within each 16-bit
 * subframe word, so AC-3's 0x0B77 syncword appears on the wire as 0x770B. Get this wrong and you
 * have a decoder that finds no syncword and says nothing useful about why.
 *
 * **`Pd` is a BIT count -- except for E-AC-3, where it is bytes.** Treating it as bytes throughout
 * reads eight times too much and then resynchronises for the rest of the film.
 *
 * Build: make        (or: cc -O2 -o spdif-deframe spdif-deframe.c $(pkg-config --cflags --libs libavcodec libavutil))
 */
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spdif-policy.h"

#define PA 0xF872u
#define PB 0x4E1Fu
#define MAX_BURST 65536
#define MAX_CH 16

static unsigned OUT_CH = 6;
static int verbose = 0;
static int list_only = 0;

/* One decoder, reopened when the payload type changes -- which a TV does mid-programme. */
static const AVCodec *codec;
static AVCodecContext *ctx;
static unsigned open_type = 0xFFFF;

static int open_decoder(const spdif_policy_t *p)
{
    if (ctx) { avcodec_free_context(&ctx); ctx = NULL; }
    codec = avcodec_find_decoder_by_name(p->codec);
    if (!codec) {
        fprintf(stderr, "spdif-deframe: policy wants '%s' for %s, but this libavcodec has no such "
                        "decoder\n", p->codec, p->name);
        return -1;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "spdif-deframe: could not open the %s decoder\n", p->codec);
        return -1;
    }
    open_type = p->data_type;
    if (verbose) fprintf(stderr, "spdif-deframe: decoding %s with '%s'\n", p->name, p->codec);
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--channels") && i + 1 < argc) OUT_CH = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--list")) list_only = 1;
        else {
            fprintf(stderr,
                "spdif-deframe -- IEC 61937 deframing + in-process decode, low latency\n\n"
                "usage: spdif-deframe [--channels N] [--verbose] [--list]\n\n"
                "  stdin   S16_LE stereo S/PDIF subframe payload (arecord, a PIO capture, a file)\n"
                "  stdout  interleaved S16_LE PCM, --channels wide (default 6)\n\n"
                "  --list  print the decode policy and exit\n");
            return 2;
        }
    }
    if (list_only) {
        printf("%-14s %-8s %s\n", "type", "codec", "disposition");
        for (unsigned i = 0; i < SPDIF_POLICY_N; i++) {
            static const char *d[] = {"decode", "silent", "reject", "unknown"};
            printf("%-14s %-8s %s\n", SPDIF_POLICY[i].name,
                   SPDIF_POLICY[i].codec ? SPDIF_POLICY[i].codec : "-",
                   d[SPDIF_POLICY[i].disposition]);
        }
        printf("\nanything not listed: dropped (unknown is a rejection, not a fallback)\n");
        return 0;
    }
    if (OUT_CH < 1 || OUT_CH > MAX_CH) {
        fprintf(stderr, "channels must be 1..%d\n", MAX_CH); return 2;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame) { fprintf(stderr, "out of memory\n"); return 1; }

    static uint8_t burst[MAX_BURST];
    int16_t outbuf[MAX_CH];
    uint16_t prev = 0;
    long bursts = 0, decoded = 0, dropped = 0;
    unsigned warned[32] = {0};
    int warned_width = 0;
    /* What the stream says it IS, reported on a tick and, separately, the moment it changes. A
     * 2.0 track and a 5.1 track look identical from every other angle -- both AC-3, same cable,
     * same rate -- so a routing narrowing that is correct behaviour reads exactly like a fault
     * unless something says the width changed. */
    int last_nch = -1, last_rate = -1;
    time_t last_tick = time(NULL);
    long tick_bursts = 0, tick_frames = 0, tick_dropped = 0;
    int16_t w;

    while (fread(&w, 2, 1, stdin) == 1) {
        uint16_t cur = (uint16_t)w;
        if (!(prev == PA && cur == PB)) { prev = cur; continue; }

        int16_t pc, pd;
        if (fread(&pc, 2, 1, stdin) != 1 || fread(&pd, 2, 1, stdin) != 1) break;
        unsigned dtype = (unsigned)pc & 0x1Fu;
        /* Pd in bits, except E-AC-3 (0x15) where it is bytes. See the header. */
        unsigned bytes = (dtype == 0x15u) ? (unsigned)(uint16_t)pd
                                          : ((unsigned)(uint16_t)pd + 7u) / 8u;
        if (bytes == 0 || bytes > MAX_BURST) { prev = 0; continue; }

        unsigned words = (bytes + 1) / 2, got = 0;
        for (unsigned i = 0; i < words; i++) {
            int16_t pw;
            if (fread(&pw, 2, 1, stdin) != 1) { words = i; break; }
            burst[got++] = (uint8_t)(((uint16_t)pw >> 8) & 0xFFu);   /* big-endian in the word */
            if (got < bytes) burst[got++] = (uint8_t)((uint16_t)pw & 0xFFu);
        }
        prev = 0;
        bursts++;
        tick_bursts++;
        if (got < bytes) break;                       /* input ended mid-burst */

        const spdif_policy_t *p = spdif_lookup(dtype);
        spdif_disposition_t disp = p ? p->disposition : SPDIF_UNKNOWN;

        if (disp != SPDIF_DECODE) {
            /* Dropped, and said once per type rather than once per burst. NULL and PAUSE are
             * silence with a valid clock, not a fault -- a parser that treats them as an error
             * drops the stream every time the TV shows a menu. */
            if (!warned[dtype]) {
                warned[dtype] = 1;
                if (disp == SPDIF_SILENT) {
                    if (verbose) fprintf(stderr, "spdif-deframe: %s bursts (silence, not a fault)\n",
                                         p->name);
                } else {
                    fprintf(stderr, "spdif-deframe: dropping %s (Pc type 0x%02x) -- %s\n",
                            p ? p->name : "unrecognised", dtype,
                            disp == SPDIF_UNKNOWN ? "not in the policy, so not decoded"
                                                  : "policy says do not decode");
                }
            }
            dropped++;
            tick_dropped++;
            continue;
        }

        if (dtype != open_type && open_decoder(p) < 0) return 1;

        /* THE POINT OF THE WHOLE FILE: the packet reaches the decoder here, the moment its last
         * byte arrived, rather than after the padding out to the next burst boundary. */
        pkt->data = burst;
        pkt->size = (int)bytes;
        if (avcodec_send_packet(ctx, pkt) < 0) continue;

        while (avcodec_receive_frame(ctx, frame) == 0) {
            int nch = frame->ch_layout.nb_channels;
            if (nch != last_nch || frame->sample_rate != last_rate) {
                fprintf(stderr, "spdif-deframe: %s %d ch @ %d Hz\n",
                        p->name, nch, frame->sample_rate);
                last_nch = nch; last_rate = frame->sample_rate;
            }
            if ((unsigned)nch > OUT_CH && !warned_width) {
                fprintf(stderr, "spdif-deframe: %d channels truncated to %u -- the output width is "
                                "fixed so a reader is not broken mid-stream\n", nch, OUT_CH);
                warned_width = 1;
            }
            for (int s = 0; s < frame->nb_samples; s++) {
                memset(outbuf, 0, sizeof(int16_t) * OUT_CH);
                for (int c = 0; c < nch && (unsigned)c < OUT_CH; c++) {
                    /* Planar float is what every one of these decoders produces. */
                    if (frame->format == AV_SAMPLE_FMT_FLTP) {
                        float v = ((const float *)frame->data[c])[s];
                        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                        outbuf[c] = (int16_t)lrintf(v * 32767.0f);
                    } else if (frame->format == AV_SAMPLE_FMT_S16P) {
                        outbuf[c] = ((const int16_t *)frame->data[c])[s];
                    }
                }
                fwrite(outbuf, sizeof(int16_t), OUT_CH, stdout);
            }
            decoded++;
            tick_frames++;
        }

        if (verbose) {
            time_t now = time(NULL);
            if (now - last_tick >= 10) {
                fprintf(stderr, "spdif-deframe: %ld bursts %ld frames %ld dropped in %lds\n",
                        tick_bursts, tick_frames, tick_dropped, (long)(now - last_tick));
                last_tick = now; tick_bursts = tick_frames = tick_dropped = 0;
            }
        }
    }

    fflush(stdout);
    if (verbose)
        fprintf(stderr, "spdif-deframe: %ld bursts, %ld frames decoded, %ld dropped\n",
                bursts, decoded, dropped);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (ctx) avcodec_free_context(&ctx);
    return 0;
}
