/*
 * make-vectors -- build a real IEC 61937 stream, for testing the deframer against something other
 * than its own assumptions.
 *
 * Encodes a known signal to AC-3 with libavcodec, wraps each frame in a burst exactly as the
 * standard specifies, and writes the result as the S16_LE stereo word stream a front end would
 * deliver. Synthetic bursts with a fabricated payload would exercise the framing but not the
 * decode, and a fabricated payload is precisely where a byte-swap error hides -- so the payload
 * here is real AC-3 that a real decoder has to accept.
 *
 * usage: make-vectors [--type N] [--frames N] > stream.raw
 *   --type   IEC 61937 data type to declare in Pc (default 1 = AC-3). Use 0x15 or 0x1E to test
 *            that the deframer DROPS types the policy rejects or has never heard of.
 *
 * Build: make vectors
 */
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AC3_FRAME 1536
#define PERIOD_BYTES (AC3_FRAME * 4)     /* the AC-3 burst period, ffmpeg spdifdec.c */

static void put16(uint16_t v) { fputc(v & 0xFF, stdout); fputc((v >> 8) & 0xFF, stdout); }

/* One burst: Pa Pb Pc Pd, the payload big-endian within each word, zero-padded to the period. */
static void emit_burst(unsigned dtype, const uint8_t *data, unsigned len)
{
    put16(0xF872); put16(0x4E1F);
    put16((uint16_t)(dtype & 0x1F));
    put16((uint16_t)(dtype == 0x15 ? len : len * 8));      /* bytes for E-AC-3, bits otherwise */
    unsigned words = (len + 1) / 2;
    for (unsigned i = 0; i < words; i++) {
        uint8_t hi = data[i * 2];
        uint8_t lo = (i * 2 + 1 < len) ? data[i * 2 + 1] : 0;
        put16((uint16_t)((hi << 8) | lo));                 /* the swap the deframer must undo */
    }
    for (unsigned b = 8 + words * 2; b < PERIOD_BYTES; b += 2) put16(0);
}

int main(int argc, char **argv)
{
    unsigned dtype = 1, want = 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--type") && i + 1 < argc) dtype = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) want = (unsigned)atoi(argv[++i]);
    }

    const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (!enc) { fprintf(stderr, "no AC-3 encoder in this libavcodec\n"); return 1; }
    AVCodecContext *c = avcodec_alloc_context3(enc);
    c->sample_rate = 48000;
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->bit_rate = 640000;
    av_channel_layout_from_mask(&c->ch_layout, AV_CH_LAYOUT_5POINT1);
    if (avcodec_open2(c, enc, NULL) < 0) { fprintf(stderr, "cannot open the AC-3 encoder\n"); return 1; }

    AVFrame *f = av_frame_alloc();
    f->nb_samples = c->frame_size;
    f->format = c->sample_fmt;
    f->sample_rate = c->sample_rate;
    av_channel_layout_copy(&f->ch_layout, &c->ch_layout);
    av_frame_get_buffer(f, 0);
    AVPacket *pkt = av_packet_alloc();

    /* A distinct tone per channel, so a channel-order mistake downstream is visible rather than
     * merely plausible: 220 Hz in FL, 330 in FR, and so on up. */
    unsigned emitted = 0;
    long t = 0;
    while (emitted < want) {
        av_frame_make_writable(f);
        for (int ch = 0; ch < f->ch_layout.nb_channels; ch++) {
            float *p = (float *)f->data[ch];
            double hz = 220.0 * (ch + 1);
            for (int s = 0; s < f->nb_samples; s++)
                p[s] = 0.25f * (float)sin(2.0 * M_PI * hz * (double)(t + s) / 48000.0);
        }
        t += f->nb_samples;
        if (avcodec_send_frame(c, f) < 0) break;
        while (avcodec_receive_packet(c, pkt) == 0 && emitted < want) {
            emit_burst(dtype, pkt->data, (unsigned)pkt->size);
            emitted++;
            av_packet_unref(pkt);
        }
    }
    fflush(stdout);
    fprintf(stderr, "make-vectors: %u bursts, type 0x%02x\n", emitted, dtype);
    av_packet_free(&pkt); av_frame_free(&f); avcodec_free_context(&c);
    return 0;
}
