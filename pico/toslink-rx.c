/*
 * toslink-rx -- S/PDIF receiver and reporter for the Pico board.
 *
 * Reads S/PDIF off the ORJ-8 board wired per hardware/README.md, "Board A" (GP15, physical pin 20),
 * and reports once a second on USB CDC. Press 's' to stream the audio as raw PCM instead, 'c' to
 * dump the IEC 60958 side channels.
 *
 * **This is a transport, not a sound card.** What it carries is the S/PDIF payload, which may be
 * stereo PCM or a compressed surround bitstream, and only software can tell which. So nothing here
 * interprets it -- presenting it as ordinary audio input would invite the host to resample or
 * convert it, and doing that to a bitstream turns it into noise while everything reports success.
 *
 * ## WHY A REPORTER BEFORE A USB TRANSPORT ENDPOINT
 *
 * The payload currently reaches software over USB CDC, with the glue in pico/listen.py. A UAC2
 * endpoint would let an ordinary capture API read it instead -- a convenience for transport, not a
 * transformation into an audio device. Writing that first means bringing up a PIO receiver, a ring
 * buffer, a TinyUSB endpoint and an isochronous clocking policy at once, with nothing to say which
 * of them is wrong when the host captures silence.
 *
 * A reporter answers the questions that shape that design, using nothing but a host, the Pico and
 * an optical cable:
 *
 *   1. **Is the board wired correctly?** LOCK is the test, and nothing else is needed.
 *   2. **What is the source's real sample rate, and does it MOVE?** USB Audio Class 2 negotiates the
 *      rate host-side, so a source that changes rate needs the stream closed and reopened. If a
 *      source never leaves 48 kHz that problem is theoretical -- and this counts rate changes, so
 *      the answer is measured rather than assumed.
 *   3. **Does the payload survive as an IEC 61937 bitstream?** Counted here, before any USB exists,
 *      so a later "no bursts on the host" points at the USB path rather than at the framing.
 *
 * ## THE ONE RULE THAT LIVES IN TWO PLACES, AND THE THREE THAT DO NOT
 *
 * Anything that also decodes S/PDIF -- the decoder in this repo, for instance -- has to agree with
 * this file about exactly one thing, and it is worth being precise about which:
 *
 *   - **Preamble alignment is NOT duplicated.** pico_spdif_rx aligns its own FIFO to the sync-B
 *     block boundary (`block_aligned` / `block_align_count` in spdif_rx.c, which discards words
 *     until it hits it), so once STABLE an even FIFO index is channel A and an odd one channel B.
 *   - **IEC 61937 deframing, codec decode and rate conversion are NOT here** and never will be.
 *     This firmware carries bytes; interpreting them is the host's job, and that is exactly what
 *     makes every payload format work.
 *   - What IS shared is one line: the 16-bit payload's position inside the subframe.
 *
 * The FIFO word carries the 24-bit audio field at bits 4..27 -- pico_spdif_rx's own I2S sample masks
 * it `0x0ffffff0`. IEC 61937 puts the burst payload in the top 16 of those 24. Composing the two:
 *
 *     payload16 = (word >> 12) & 0xFFFF
 *
 * That is the whole shared rule, and PAYLOAD16_SHIFT below is the only place it appears.
 *
 * ## INSTRUMENTS, WHICH SHIP WITH IT RATHER THAN AFTER THE FIRST BUG
 *
 * A component that carries audio ships with the counters that would catch it failing, not with
 * counters added after the first bug. The failure worth designing against is an instrument that
 * measures its own LOOP and not its own INPUT: a queue that empties tells you nothing about why.
 *
 *   - Consumed and produced are both counted, and reported as rates over a STATED window.
 *   - min/max, never a bare instantaneous value -- `rate 48000` and `rate 48000 (47993..47995)` are
 *     different facts and only the second is a fact about the system.
 *   - The FIFO's own depth is the input-timing signal: how full it gets says whether the consumer
 *     keeps up, and is the number a USB ring would have to be sized from.
 *   - What it could NOT do is counted: near-overflows, parity errors, lost locks.
 *
 * And the instrument stays off the sample path. The library's callbacks fire from alarm/IRQ context,
 * so they only touch counters; every printf happens on the main loop. Counters on the sample path, formatting and I/O on
 * the main loop -- a report emitted from inside a real-time path becomes the fault it measures.
 *
 * ## WHY THERE IS AN LED STAGE CODE
 *
 * A firmware that hangs during init cannot report over the stdio it never finished setting up, so
 * stdio is the one instrument that class of fault disables. On a host it looks like a device that
 * enumerates and then stops: the descriptor is readable, but the USB node has zero interface
 * children because configuration never completed, and no serial device appears.
 *
 * Hence the onboard LED, blinked from a repeating timer rather than the main loop, so it keeps
 * reporting the last stage reached even if the main loop is stuck:
 *
 *     1 blink   booted, stdio initialised
 *     2 blinks  first report emitted -- USB is genuinely working
 *     3 blinks  spdif_rx_start() returned
 *     4 blinks  S/PDIF has been STABLE at least once
 *     frozen    hung at the stage it last showed
 *
 * Two things are arranged so that class of fault cannot recur. There is no `set_sys_clock_khz()`:
 * RP2040 already boots at 125 MHz, exactly SPDIF_RX_SYS_CLK_FREQ, so the call could only
 * reconfigure the PLL to the value it already held on the way into USB init -- it is asserted
 * instead. And `spdif_rx_start()` runs AFTER the first report goes out, so a hang inside it is
 * distinguishable from a hang before it: 2 blinks with a live serial line means the receiver, and
 * no blinks past 1 means it never got that far.
 *
 * Build and flash: pico/build.sh --flash   (needs the Pico held in BOOTSEL while plugging in;
 * `picotool load` is used rather than a file copy -- see build.sh)
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"
#include "hardware/structs/sio.h"
#include "pico/stdio_usb.h"
#include "pico/stdio/driver.h"
#include "spdif_rx.h"

/* Wiring, from hardware/README.md, "Board A". GP15 is pico_spdif_rx's own default
 * data pin, chosen so the reference library runs unmodified -- one variable at a time. */
#define SPDIF_DATA_PIN   15

/* The 16-bit IEC 61937 payload's position in a FIFO word. See the header. */
#define PAYLOAD16_SHIFT  12

#define REPORT_MS        1000u

/* IEC 61937 burst preamble. Pa then Pb in CONSECUTIVE subframes -- which may straddle the
 * left/right boundary, so the probe compares against the previous word whatever channel it came
 * from. spdif-decode.c's probe carries the same comment for the same reason. */
#define IEC61937_PA     0xF872u
#define IEC61937_PB     0x4E1Fu

/* ---- which encoding is the TV actually sending? ----
 *
 * The word AFTER Pb is Pc, and its low five bits are the IEC 61937 data type -- the only place in
 * the stream that names the payload. Pa/Pb alone say "some bitstream"; Pc says which one.
 *
 * This is here because "support all encoding types" is a requirement about the PI, not about this
 * chip. The Pico's contract is to be a bit-exact conduit: every encoding works precisely BECAUSE
 * nothing here interprets the payload, and any format handling added to this firmware would be a
 * way to break the formats it does not know about. What the Pico can usefully contribute is
 * evidence -- a measured list of what this TV emits, so the Pi's format support is designed
 * against reality instead of a spec's worth of possibilities.
 *
 * Both the current type and a bitmask of everything seen since boot are reported, because the
 * interesting behaviour is the SWITCHING: a TV moves between PCM and bitstream freely -- menu
 * clicks versus content -- so a list of what has been seen across a session is worth more than
 * whatever happens to be playing this second.
 */
static const char *iec61937_type_name(uint8_t t)
{
    switch (t) {
        case 0:  return "null";
        case 1:  return "AC-3";
        case 3:  return "pause";
        case 4:  return "MPEG-1 L1";
        case 5:  return "MPEG-1 L2/3";
        case 6:  return "MPEG-2 ext";
        case 7:  return "MPEG-2 AAC";
        case 8:  return "MPEG-2 L1 LSF";
        case 9:  return "MPEG-2 L2 LSF";
        case 10: return "MPEG-2 L3 LSF";
        case 11: return "DTS-I";
        case 12: return "DTS-II";
        case 13: return "DTS-III";
        case 14: return "ATRAC";
        case 15: return "ATRAC 2/3";
        case 16: return "E-AC-3";
        case 17: return "DTS-IV/HD";
        case 19: return "WMA Pro";
        case 21: return "MAT/TrueHD";
        default: return "unknown";
    }
}

/* ---- the S/PDIF side channels: where a vendor control protocol would live ----
 *
 * Some TVs and soundbars can control volume over the optical link. Whatever protocol that uses is
 * undocumented here, and it may not be in the S/PDIF stream at all -- in which case this instrument
 * returns a clean negative cheaply, which is worth having too.
 *
 * What IS certain is where a vendor would put it, because IEC 60958 gives a subframe exactly four
 * non-audio bits and two of them form addressable channels:
 *
 *     slots 0-3   preamble          (B/M/W -- the framing)
 *     slots 4-27  audio             the 24-bit sample, or the bitstream payload
 *     slot 28     V, validity
 *     slot 29     U, USER DATA      192 bits per block per channel, no standard meaning at all
 *     slot 30     C, channel status 192 bits per block, standardised but with reserved fields
 *     slot 31     P, parity
 *
 * **U is the obvious carrier**: a general-purpose channel the standard deliberately leaves empty,
 * 192 bits per block per channel, and nothing in this project has ever read it. C is the other
 * candidate -- the library collects it already and byte 0 bit 1 is the non-audio flag we report --
 * but most of C is specified, so a vendor has less room there.
 *
 * So this accumulates both, per block, and prints a hex dump only WHEN THEY CHANGE. Static side
 * channels stay silent; press volume on the remote and a diff appears with a timestamp. That is the
 * whole method: no guess about the encoding, just make the bytes visible and let the remote
 * generate the evidence.
 *
 * Two honest limits. The block position is tracked by counting words, so a dropped word desyncs
 * the U accumulator until the next report -- fine for discovery, not a decoder. And the dump is
 * queued for the report tick rather than printed inline, because printf inside the drain loop is an
 * instrument that becomes the fault.
 */
#define SIDE_BYTES 24          /* 192 bits per block per channel */

static uint8_t  ua_bits[SIDE_BYTES], ub_bits[SIDE_BYTES];   /* accumulating, channel A and B */
static uint8_t  ua_last[SIDE_BYTES], ub_last[SIDE_BYTES];   /* last completed block */
static uint8_t  cbits_last[SIDE_BYTES];
static uint32_t block_pos;                                  /* word index within the 384 block */
static bool     side_changed, side_seen;
static uint32_t side_changes;

static void side_accumulate(uint32_t word)
{
    uint32_t u = (word >> 29) & 1u;            /* slot 29 -- see the table above */
    uint32_t idx = block_pos >> 1;             /* subframe pair = one frame */
    if (idx < SIDE_BYTES * 8u) {
        uint8_t *dst = (block_pos & 1u) ? ub_bits : ua_bits;
        if (u) dst[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
    }
    if (++block_pos >= SPDIF_BLOCK_SIZE) {     /* block complete: compare and latch */
        block_pos = 0;
        if (side_seen && (memcmp(ua_bits, ua_last, SIDE_BYTES) ||
                          memcmp(ub_bits, ub_last, SIDE_BYTES))) {
            side_changed = true;
            side_changes++;
        }
        memcpy(ua_last, ua_bits, SIDE_BYTES);
        memcpy(ub_last, ub_bits, SIDE_BYTES);
        memset(ua_bits, 0, SIDE_BYTES);
        memset(ub_bits, 0, SIDE_BYTES);
        side_seen = true;
    }
}

/* ---- LG Sound Sync: volume and mute, carried in the channel-status block ----
 *
 * Some LG televisions broadcast their volume and mute state down the optical cable so a connected
 * device can follow the TV's remote. It rides in the IEC 60958 CHANNEL STATUS bits -- not the user
 * data bits, which is where a general-purpose side channel would more naturally live.
 *
 * The wire format is a fact about the TV, and this decoder is written from the format rather than
 * from anyone's implementation. It was cross-checked against five published samples before a line
 * of it was written; the table below is those samples, and they all reproduce.
 *
 * ## LAYOUT
 *
 * Channel status is 192 bits, one per subframe, presented here as 24 bytes LSB-first per
 * IEC 60958-3. Two fields:
 *
 *   SIGNATURE, five nibbles reading 0xF048A, and its presence IS the detection:
 *       (cs[16] & 0x0F) == 0x0F,  cs[17] == 0x04,  cs[18] == 0x8A
 *
 *   PAYLOAD, one byte spanning two:
 *       vol_byte = ((cs[15] & 0x0F) << 4) | ((cs[16] & 0xF0) >> 4)
 *       volume   = vol_byte & 0x7F        0..100
 *       muted    = vol_byte & 0x80        volume is PRESERVED while muted, so unmuting restores it
 *
 * 2017-era sets use the same nibble layout with the byte indices mirrored about the block centre:
 * 16<->7, 17<->6, 18<->5, 15<->8. Both are checked.
 *
 * ## SAMPLES THIS DECODER REPRODUCES
 *
 *     cs[15] cs[16]   vol_byte   volume  mute
 *      0x10   0x0F      0x00        0      0
 *      0x13   0x2F      0x32       50      0
 *      0x05   0x0F      0x50       80      0
 *      0x0D   0x0F      0xD0       80      1
 *      0x16   0x4F      0x64      100      0
 *
 * ## WHAT IT IS NOT
 *
 * There is **no checksum and no back-channel** -- the TV cannot tell whether anything received it,
 * and there is no pairing handshake. Presence is purely "did the signature appear". So robustness
 * has to come from hysteresis rather than from validation: three consecutive sightings to declare
 * it present, ten consecutive misses to declare it gone, which absorbs a single flipped bit.
 *
 * A full channel-status block takes 192 frames to assemble -- 4 ms at 48 kHz -- and there is no
 * update event, so this simply reads the current state on the report tick.
 */
#define LG_PRESENT_THRESHOLD  3
#define LG_ABSENT_THRESHOLD  10

static uint8_t  lg_present, lg_volume = 0xFF, lg_muted;
static uint8_t  lg_hits, lg_misses;
static bool     lg_mirrored;

/* Returns true and fills vol/mute if the signature is present in either layout. */
static bool lg_decode(const uint8_t *cs, uint8_t *vol, uint8_t *mute, bool *mirrored)
{
    bool a = ((cs[16] & 0x0Fu) == 0x0Fu) && cs[17] == 0x04u && cs[18] == 0x8Au;
    bool b = ((cs[7]  & 0x0Fu) == 0x0Fu) && cs[6]  == 0x04u && cs[5]  == 0x8Au;
    if (!a && !b) return false;
    uint8_t hi = a ? cs[15] : cs[8];
    uint8_t lo = a ? cs[16] : cs[7];
    uint8_t vb = (uint8_t)(((hi & 0x0Fu) << 4) | ((lo & 0xF0u) >> 4));
    *vol = (uint8_t)(vb & 0x7Fu);
    *mute = (uint8_t)((vb & 0x80u) ? 1u : 0u);
    *mirrored = !a;
    return true;
}

static void lg_update(const uint8_t *cs)
{
    uint8_t vol, mute; bool mirrored;
    if (lg_decode(cs, &vol, &mute, &mirrored)) {
        lg_misses = 0;
        if (lg_hits < LG_PRESENT_THRESHOLD) lg_hits++;
        if (lg_hits >= LG_PRESENT_THRESHOLD) {
            lg_present = 1; lg_volume = vol; lg_muted = mute; lg_mirrored = mirrored;
        }
    } else {
        lg_hits = 0;
        if (lg_misses < LG_ABSENT_THRESHOLD) lg_misses++;
        if (lg_misses >= LG_ABSENT_THRESHOLD) lg_present = 0;
    }
}

static void side_report(bool force)
{
    uint8_t c[SIDE_BYTES];
    spdif_rx_get_c_bits(c, SIDE_BYTES, 0);
    lg_update(c);
    bool c_changed = memcmp(c, cbits_last, SIDE_BYTES) != 0;
    if (!force && !side_changed && !c_changed) return;
    memcpy(cbits_last, c, SIDE_BYTES);
    side_changed = false;

    printf("  SIDE (change %lu)\n           U-A:", (unsigned long)side_changes);
    for (int i = 0; i < SIDE_BYTES; i++) printf(" %02x", ua_last[i]);
    printf("\n           U-B:");
    for (int i = 0; i < SIDE_BYTES; i++) printf(" %02x", ub_last[i]);
    printf("\n           C  :");
    for (int i = 0; i < SIDE_BYTES; i++) printf(" %02x", c[i]);
    if (lg_present)
        printf("\n           LG Sound Sync: volume %u%s%s",
               lg_volume, lg_muted ? " MUTED" : "", lg_mirrored ? "  (mirrored layout)" : "");
    printf("\n");
}

/* ---- raw audio streaming over the CDC port: the listening test ----
 *
 * Every counter in this file can be clean while the audio is wrong, so the chain does not count as
 * working until somebody has heard it. This is the cheapest
 * possible way to get ears on it: press 's' and the port stops emitting text and starts emitting
 * raw interleaved S16_LE stereo, which pico/listen.py pipes into a player.
 *
 * It is a real tool, not only a bring-up aid -- for a PCM source it is simply how you listen. What
 * it is NOT is a timing reference: a serial byte pipe has no clocking contract, so the source's
 * crystal and the player's assumed rate drift apart with nothing correcting it. A proper USB
 * transport endpoint is the one that would carry the payload with real isochronous timing. This is a byte pipe over a serial
 * port with no clocking contract at all -- the source's crystal and whatever the player assumes will
 * drift apart, and nothing here corrects it. Fine for listening, useless as a measurement of
 * anything timing-related.
 *
 * Bandwidth: 96000 subframes/s x 2 bytes = 187.5 kB/s, comfortably inside USB full speed.
 *
 * The packing is the one rule this file shares with spdif-decode.c, and no more than that: the
 * 16-bit payload is (word >> 12) & 0xFFFF, and because pico_spdif_rx block-aligns its FIFO to
 * sync-B, consecutive words are already channel A then channel B. So the stream is literally the
 * payloads in FIFO order, and it is interleaved L,R,L,R by construction rather than by any code
 * here deciding what is left.
 *
 * A magic marker is emitted on entry so the host can discard the text still in flight rather than
 * playing it as a burst of noise.
 */
#define STREAM_MAGIC_BYTE 0xA5
#define STREAM_MAGIC_LEN  16
/* 128 bytes = 32 frames = 0.67 ms. Latency, not throughput, sets this: at 192 kB/s it means about
 * 1500 writes a second into a 64-byte CDC FIFO that is polled every 1 ms anyway, so there is
 * nothing to gain from batching harder and 2 ms of delay to lose by it. */
#define STREAM_BUF_BYTES  128

static bool     streaming;
static uint8_t  stream_buf[STREAM_BUF_BYTES];
static uint32_t stream_fill;
static uint32_t stream_overflows;   /* FIFO near-overflows WHILE streaming: USB not keeping up */
static uint32_t stream_fifo_max;    /* deepest the FIFO got while streaming, in words */

static void stream_flush(void)
{
    if (stream_fill) {
        fwrite(stream_buf, 1, stream_fill, stdout);
        stream_fill = 0;
    }
}

static void stream_put16(uint16_t v)
{
    stream_buf[stream_fill++] = (uint8_t)(v & 0xFFu);          /* little endian */
    stream_buf[stream_fill++] = (uint8_t)((v >> 8) & 0xFFu);
    if (stream_fill >= STREAM_BUF_BYTES) stream_flush();
}

static void stream_enter(void)
{
    /* Binary down a stdio that was translating newlines would corrupt every 0x0A byte. */
    stdio_set_translate_crlf(&stdio_usb, false);
    printf("\n# STREAM S16_LE 2ch - press any key to stop\n");
    fflush(stdout);
    for (int i = 0; i < STREAM_MAGIC_LEN; i++) putchar_raw(STREAM_MAGIC_BYTE);
    fflush(stdout);
    streaming = true;
    stream_fill = 0;
    stream_overflows = 0;
    stream_fifo_max = 0;
}

static void stream_exit(void)
{
    stream_flush();
    fflush(stdout);
    stdio_set_translate_crlf(&stdio_usb, true);
    streaming = false;
    /* The FIFO depth is the one latency term the host cannot see. In words; two words per frame,
     * so 96 words is 1 ms. The library's DMA granularity is SPDIF_BLOCK_SIZE = 384 words = 4 ms,
     * which is the floor for this design however small every other buffer gets. */
    printf("\n# stream stopped. fifo max %lu words (%lu.%lu ms), near-overflows %lu\n",
           (unsigned long)stream_fifo_max,
           (unsigned long)(stream_fifo_max / 96u), (unsigned long)((stream_fifo_max % 96u) * 10u / 96u),
           (unsigned long)stream_overflows);
}

/* ---- state the callbacks may touch: counters only, no I/O ---- */
static volatile uint32_t cb_locks;
static volatile uint32_t cb_unlocks;
static volatile uint32_t cb_last_freq;

/* ---- state only the main loop touches ---- */
static uint64_t subframes;
static uint64_t bursts;
static uint32_t near_overflows;
static uint16_t probe_prev;
static bool     expect_pc;          /* the word after Pb carries the data type */
static uint8_t  last_dtype = 0xFF;
static uint16_t last_pc;
static uint32_t seen_types_mask;    /* bit per IEC 61937 data type observed since boot */
static uint32_t rate_changes;
static uint32_t last_nominal;
static uint32_t peak_mag;
static uint32_t fifo_min = UINT32_MAX, fifo_max;
static float    rate_min = 1e9f, rate_max;
static uint32_t parity_base;
static uint32_t last_locks, last_unlocks;
static uint64_t last_subframes, last_bursts;

static void on_stable(spdif_rx_samp_freq_t f) { cb_last_freq = (uint32_t)f; cb_locks++; }
static void on_lost_stable(void)              { cb_unlocks++; }

static const char *state_name(spdif_rx_state_t s)
{
    switch (s) {
        case SPDIF_RX_STATE_NO_SIGNAL:      return "NO_SIGNAL";
        case SPDIF_RX_STATE_WAITING_STABLE: return "WAITING";
        case SPDIF_RX_STATE_STABLE:         return "STABLE";
        default:                            return "?";
    }
}

/* Drain the FIFO, counting. It must be drained even though the samples are discarded: left to fill
 * it would overflow and the library's block alignment would be chasing its own tail, which would
 * look exactly like a wiring fault. */
static void drain_fifo(void)
{
    for (;;) {
        uint32_t depth = spdif_rx_get_fifo_count();
        if (depth < fifo_min) fifo_min = depth;
        if (depth > fifo_max) fifo_max = depth;
        if (depth >= SPDIF_RX_FIFO_SIZE - SPDIF_BLOCK_SIZE) {
            near_overflows++;
            if (streaming) stream_overflows++;
        }
        if (streaming && depth > stream_fifo_max) stream_fifo_max = depth;
        if (depth == 0) return;

        uint32_t *buf = NULL;
        uint32_t got = spdif_rx_read_fifo(&buf, depth);
        if (got == 0 || buf == NULL) return;   /* ring wrapped with nothing contiguous */

        for (uint32_t i = 0; i < got; i++) {
            uint16_t s = (uint16_t)((buf[i] >> PAYLOAD16_SHIFT) & 0xFFFFu);
            if (expect_pc) {
                last_pc = s;
                last_dtype = (uint8_t)(s & 0x1Fu);
                seen_types_mask |= (1u << last_dtype);
                expect_pc = false;
            }
            if (probe_prev == IEC61937_PA && s == IEC61937_PB) { bursts++; expect_pc = true; }
            probe_prev = s;
            int32_t sv = (int16_t)s;
            uint32_t mag = (uint32_t)(sv < 0 ? -sv : sv);
            if (mag > peak_mag) peak_mag = mag;
            if (streaming) stream_put16(s);
            else side_accumulate(buf[i]);   /* side channels only when not streaming audio */
        }
        subframes += got;
    }
}

/* ---- raw GP15 probe: what is the PIN doing, independent of the decoder? ----
 *
 * NO_SIGNAL with locks 0 has at least four causes that look identical from the decoder's side: the
 * fibre is out, the TV is asleep, the board is not wired to the Pico, or the jack's pins are
 * reversed. The decoder cannot tell them apart because it only ever sees "no valid preambles".
 *
 * Sampling the pad directly does tell them apart. GP15 belongs to PIO, but gpio_get() reads the pad
 * input regardless of who owns the function, so this costs nothing and disturbs nothing:
 *
 *     edges 0,  duty 0%     pin stuck LOW  -- no receiver output reaching the pin at all:
 *                                             the signal wire, or Vout not connected
 *     edges 0,  duty 100%   pin stuck HIGH -- the receiver is powered and idling with no light:
 *                                             the fibre, or the TV is not transmitting
 *     edges ~6000/ms, ~50%  a carrier IS present and the DECODER is the problem
 *
 * The third case is the one worth having: it moves the fault from the bench to the firmware, which
 * is a completely different afternoon. 50% duty is not a coincidence -- biphase-mark coding is
 * DC-balanced by construction, which is the same property the 1.65 V multimeter check in
 * hardware/README.md, bring-up check 3, relies on.
 *
 * Cost and placement: one 1 ms sampling burst, on the main loop, once a second, and ONLY while
 * unlocked -- so it can never perturb a working stream. No allocation, no syscall, no filesystem.
 */
#define PROBE_US 1000u

static uint32_t probe_edges;
static uint32_t probe_high;
static uint32_t probe_samples_taken;
static uint32_t probe_pullup_duty;   /* the instrument's own self-test, see below */

static void probe_window(uint32_t *edges_out, uint32_t *high_out, uint32_t *n_out)
{
    absolute_time_t until = make_timeout_time_us(PROBE_US);
    uint32_t edges = 0, high = 0, n = 0;
    bool last = gpio_get(SPDIF_DATA_PIN);
    while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
        for (int i = 0; i < 64; i++) {
            bool now = gpio_get(SPDIF_DATA_PIN);
            if (now != last) { edges++; last = now; }
            if (now) high++;
            n++;
        }
    }
    *edges_out = edges; *high_out = high; *n_out = n;
}

/* MEASURED sample rate, rather than the 25-30 MS/s first guessed: ~6800 samples per 1 ms window,
 * i.e. about 6.8 MS/s. That is barely above the 6.144 M transitions/s a 48 kHz S/PDIF carrier
 * produces, so a live carrier ALIASES here and `edges` is not a quantitative reading of it. It is
 * still decisive for the question actually asked -- zero edges with 0% or 100% duty over ~6800
 * samples cannot be a carrier of any kind.
 *
 * ## AND THE INSTRUMENT SELF-TESTS, because "edges 0, duty 0%" is also what a broken probe says
 *
 * Suspect the instrument before the system. A probe reading the wrong pin, or one where gpio_get() returns nothing useful because PIO
 * owns the pad, reports exactly the same thing as a dead wire.
 *
 * So the probe briefly enables the pad's pull-UP and measures again. The pull-up is ~50k, which any
 * real driver overwhelms, so:
 *
 *     pull-up duty ~100%   the probe and the pad read fine, and the pin is UNDRIVEN -- floating,
 *                          i.e. not connected to anything that drives it
 *     pull-up duty ~0%     something is actively holding the pin LOW: a wire IS present and the
 *                          receiver is not powered, or there is a short to ground
 *                          (or the probe is broken -- but then it would not have moved at all)
 *
 * That is the difference between "you have not wired it yet" and "you have wired it and the
 * receiver is dead", which are two very different next actions. Only runs while unlocked, and the
 * pad's pull state is saved and restored so a working stream is never touched.
 */
/* TWO settle times, not one, and the reason is a mistake this very board made.
 *
 * The pad pull-up is 50-80k. Against the node capacitance that gives a charge time constant, and
 * the settle has to beat it or a CAPACITOR reads exactly like a short:
 *
 *     30 pF  (C2, correct)      tau ~ 1.6 us    50 us settle is ample
 *     100 nF (C1, if it lands
 *             on the signal by
 *             a mirrored jack)  tau ~ 5.5 ms    50 us settle reads 0% -- a FALSE "held low"
 *
 * The first version of this probe settled for 50 us only, which would have confidently reported
 * "something IS connected and pulling it down" for a 100 nF to ground. So it now measures at BOTH
 * 100 us and 25 ms, and the pair of readings separates the three cases:
 *
 *     short high, long high    the pin FLOATS -- nothing drives it
 *     short LOW,  long high    a CAPACITIVE load, big enough to be C1 (100 nF) on the wrong node
 *     short low,  long low     a genuine conductive path: a bridge, or an unpowered chip's pin
 *
 * That middle case is the one worth having. A mirrored ORJ-8 swaps the roles of the two outer pins,
 * so correcting only the WIRES leaves the 100 nF across the signal -- 0.26 ohms at the 6.144 MHz
 * cell rate -- and this is the reading that names it instead of blaming a short.
 */
static uint32_t probe_pullup_duty_slow;

static void probe_pin(void)
{
    probe_window(&probe_edges, &probe_high, &probe_samples_taken);

    bool was_up = gpio_is_pulled_up(SPDIF_DATA_PIN);
    bool was_dn = gpio_is_pulled_down(SPDIF_DATA_PIN);
    uint32_t e2, h2, n2;

    gpio_set_pulls(SPDIF_DATA_PIN, true, false);
    busy_wait_us(100);                      /* beats 30 pF (~1.6 us), not 100 nF (~5.5 ms) */
    probe_window(&e2, &h2, &n2);
    probe_pullup_duty = n2 ? (uint32_t)((uint64_t)h2 * 100u / n2) : 0;

    busy_wait_us(25000);                    /* ~4.5 tau for 100 nF against a 55k pull-up */
    probe_window(&e2, &h2, &n2);
    probe_pullup_duty_slow = n2 ? (uint32_t)((uint64_t)h2 * 100u / n2) : 0;

    gpio_set_pulls(SPDIF_DATA_PIN, was_up, was_dn);
}

/* ---- run-length capture: is this carrier the right SHAPE, not just present? ----
 *
 * The edge counter above samples at ~6.8 MS/s against a carrier whose transitions run at
 * 6.144 M/s, so its count ALIASES and cannot answer the question that matters once a carrier is
 * confirmed present: are the pulses the right WIDTH?
 *
 * That question separates two faults that look identical from the decoder's side, and which need
 * completely different afternoons:
 *
 *   - a correct carrier that the library is misconfigured to read, or
 *   - a DEGRADED carrier -- the right duty cycle, because the receiver still tracks the light, but
 *     slow edges or missing transitions. Which is what a receiver damaged by 3.3 V on its output
 *     pin would plausibly produce, and this board's output pin has had exactly that.
 *
 * S/PDIF's biphase-mark coding makes this crisp. At 48 kHz the cell is 1/6.144 MHz = 162.8 ns, and
 * a run between transitions is only ever ONE cell (a 1 bit's two halves), TWO cells (a 0 bit), or
 * THREE (inside a preamble -- the standard reserves 3-cell runs for exactly that, which is why they
 * are unambiguous). So a healthy stream's run lengths cluster at 163 / 326 / 488 ns and nothing
 * else. spdif-decode.c on the Pi is built on the same three-value property.
 *
 * So: sample the pad as fast as an unrolled loop can, measure the real sample rate rather than
 * assuming it, and report the run-length extremes in nanoseconds. If min is near 163 ns the carrier
 * is right and the fault is ours. If min is several times that, the signal is wrong and no amount
 * of firmware will fix it.
 */
#define CAP_WORDS 512
static uint32_t cap_buf[CAP_WORDS];
static uint32_t cap_rate_ksps;
static uint32_t cap_min_ns, cap_max_ns, cap_mode_ns, cap_runs;

static void capture_runs(void)
{
    uint32_t t0 = timer_hw->timerawl;
    for (int i = 0; i < CAP_WORDS; i++) {
        uint32_t w = 0;
        /* Unrolled: one gpio_in read per bit, no loop overhead between samples. */
#define S(n) w |= ((sio_hw->gpio_in >> SPDIF_DATA_PIN) & 1u) << (n);
        S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7) S(8) S(9) S(10) S(11) S(12) S(13) S(14) S(15) S(16) S(17) S(18) S(19) S(20) S(21) S(22) S(23) S(24) S(25) S(26) S(27) S(28) S(29) S(30) S(31)
#undef S
        cap_buf[i] = w;
    }
    uint32_t us = timer_hw->timerawl - t0;
    if (us == 0) us = 1;

    uint32_t n_samples = CAP_WORDS * 32u;
    cap_rate_ksps = (uint32_t)((uint64_t)n_samples * 1000u / us);   /* ksps */
    uint32_t ps_per_sample = (uint32_t)((uint64_t)us * 1000000u / n_samples);  /* picoseconds */

    /* Run lengths, in samples, then converted. A small histogram is enough to find the mode; the
     * interesting runs are 1-3 cells, so anything past 32 samples is a dropout not a cell. */
    uint32_t hist[33] = {0};
    uint32_t run = 0, mn = 0xFFFFFFFFu, mx = 0, runs = 0;
    int last = -1;
    for (uint32_t i = 0; i < CAP_WORDS; i++) {
        uint32_t w = cap_buf[i];
        for (int b = 0; b < 32; b++) {
            int v = (int)((w >> b) & 1u);
            if (last < 0) { last = v; run = 1; continue; }
            if (v == last) { run++; continue; }
            if (run < mn) mn = run;
            if (run > mx) mx = run;
            if (run < 33) hist[run]++;
            runs++;
            last = v; run = 1;
        }
    }
    uint32_t best = 0, best_n = 0;
    for (uint32_t k = 1; k < 33; k++) if (hist[k] > best_n) { best_n = hist[k]; best = k; }

    cap_runs   = runs;
    cap_min_ns = (mn == 0xFFFFFFFFu) ? 0 : (uint32_t)((uint64_t)mn * ps_per_sample / 1000u);
    cap_max_ns = (uint32_t)((uint64_t)mx * ps_per_sample / 1000u);
    cap_mode_ns = (uint32_t)((uint64_t)best * ps_per_sample / 1000u);
}

/* ---- the LED stage code: the only instrument that survives a USB hang ---- */
static volatile uint8_t g_stage;

static bool led_tick(repeating_timer_t *t)
{
    (void)t;
    /* A 12-slot cycle: `stage` on/off pairs, then dark. Driven from a timer so a stuck main loop
     * still shows the last stage reached. */
    static uint8_t slot;
    uint8_t st = g_stage;
    bool on = (slot < st * 2u) && ((slot & 1u) == 0u);
    gpio_put(PICO_DEFAULT_LED_PIN, on);
    slot = (uint8_t)((slot + 1u) % 12u);
    return true;
}

int main(void)
{
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    static repeating_timer_t led_timer;
    add_repeating_timer_ms(120, led_tick, NULL, &led_timer);

    /* NO set_sys_clock_khz(). RP2040 boots at 125 MHz, which is exactly SPDIF_RX_SYS_CLK_FREQ, so
     * the call could only reconfigure the PLL to the value it already held -- a needless clock
     * transition on the way to USB init. Asserted instead, because if a future SDK changes the
     * default the PIO timing would be silently wrong rather than loudly absent. */
    stdio_init_all();
    g_stage = 1;

    spdif_rx_config_t config = {
        .data_pin     = SPDIF_DATA_PIN,
        .pio_sm       = 0,
        .dma_channel0 = 0,
        .dma_channel1 = 1,
        .alarm_pool   = alarm_pool_get_default(),
        /* C bits included: bit 1 of byte 0 is the non-audio flag, which is the cheapest hint that
         * the source is sending a bitstream rather than PCM. It LAGS by up to a
         * 192-frame block and some sources set it late, so it is reported as a hint and
         * the sync-word count is the truth. */
        .flags        = SPDIF_RX_FLAGS_ALL,
    };

    absolute_time_t next = make_timeout_time_ms(REPORT_MS);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    bool started = false;
    bool force_side = false;
    bool side_end = false;

    for (;;) {
        /* Start the receiver only after the first report has gone out, so that a hang inside
         * spdif_rx_start() is distinguishable from a hang before USB was ever usable. */
        if (!started && g_stage >= 2) {
            spdif_rx_set_callback_on_stable(on_stable);
            spdif_rx_set_callback_on_lost_stable(on_lost_stable);
            spdif_rx_start(&config);
            started = true;
            g_stage = 3;
            printf("# spdif_rx_start() returned - pin GP%d, sysclk %lu Hz"
                   " (library wants %d), fifo %d words\n",
                   SPDIF_DATA_PIN, (unsigned long)clock_get_hz(clk_sys),
                   SPDIF_RX_SYS_CLK_FREQ, SPDIF_RX_FIFO_SIZE);
        }

        spdif_rx_state_t st = started ? spdif_rx_get_state() : SPDIF_RX_STATE_NO_SIGNAL;

        if (st == SPDIF_RX_STATE_STABLE) {
            g_stage = 4;
            /* Does this source ever leave 48 kHz? UAC2 negotiates the rate HOST-side, so a
             * source that changes rate needs the stream closed and reopened. If this counter
             * stays at zero across real use, that constraint is theoretical. */
            uint32_t nom = (uint32_t)spdif_rx_get_samp_freq();
            if (nom != 0 && last_nominal != 0 && nom != last_nominal) rate_changes++;
            if (nom != 0) last_nominal = nom;
            float f = spdif_rx_get_samp_freq_actual();
            if (f > 1000.0f) {                     /* 0 while the estimator has nothing yet */
                if (f < rate_min) rate_min = f;
                if (f > rate_max) rate_max = f;
            }
            drain_fifo();
        }

        /* One non-blocking key poll per pass drives the stream toggle. */
        int key = getchar_timeout_us(0);
        if (key != PICO_ERROR_TIMEOUT) {
            if (!streaming && (key == 's' || key == 'S')) stream_enter();
            else if (!streaming && (key == 'c' || key == 'C')) force_side = true;
            else if (streaming) stream_exit();
        }

        if (streaming) {
            stream_flush();
            continue;      /* no text at all while streaming: it would corrupt the audio */
        }

        if (absolute_time_diff_us(get_absolute_time(), next) > 0) {
            sleep_ms(1);   /* yield: never busy-spin next to a USB stack */
            continue;
        }
        next = make_timeout_time_ms(REPORT_MS);

        if (g_stage == 1) {
            printf("\n# toslink-rx -- S/PDIF receiver and reporter\n");
            printf("# with a 48 kHz source: STABLE, rate near 48000, sub ~96000/s"
                   " (2 subframes per frame).\n");
            printf("#   A compressed 5.1 bitstream also gives bursts ~31.2/s and parity 0."
                   " Pull the fibre and it MUST go NO_SIGNAL.\n");
            printf("# LED: 1 blink booted - 2 usb ok - 3 receiver started - 4 locked\n\n");
            g_stage = 2;
        }

        /* Everything below runs on the main loop only. The window is stated in the line, because a
         * throttled report is not an event rate. */
        uint32_t now      = to_ms_since_boot(get_absolute_time());
        uint32_t locks    = cb_locks,   unlocks = cb_unlocks;
        uint32_t parity   = spdif_rx_get_parity_err_count();
        uint64_t d_sub    = subframes - last_subframes;
        uint64_t d_burst  = bursts    - last_bursts;

        printf("[%6.1fs] %-9s", (now - t0) / 1000.0, state_name(st));

        if (st == SPDIF_RX_STATE_STABLE) {
            uint8_t cb0 = 0;
            spdif_rx_get_c_bits(&cb0, 1, 0);
            printf("  rate %.1f (%.1f..%.1f)  nominal %lu"
                   "  sub %llu/s  bursts %.1f/s  parity %lu"
                   "  peak %lu  fifo %lu..%lu/%d  nearovf %lu"
                   "  locks %lu unlocks %lu  cbit-nonaudio %d",
                   (double)spdif_rx_get_samp_freq_actual(),
                   (double)(rate_min > 1e8f ? 0.0f : rate_min), (double)rate_max,
                   (unsigned long)cb_last_freq,
                   (unsigned long long)(d_sub * 1000u / REPORT_MS),
                   (double)d_burst * 1000.0 / REPORT_MS,
                   (unsigned long)(parity - parity_base),
                   (unsigned long)peak_mag,
                   (unsigned long)(fifo_min == UINT32_MAX ? 0 : fifo_min),
                   (unsigned long)fifo_max, SPDIF_RX_FIFO_SIZE,
                   (unsigned long)near_overflows,
                   (unsigned long)locks, (unsigned long)unlocks,
                   (cb0 & 0x02) ? 1 : 0);

            /* Format, named rather than inferred. The channel-status non-audio bit lags by up to a
             * 192-frame block and some sources set it late, so the sync word is the
             * truth and the C bit above is only a hint -- which is why both are printed. */
            if (d_burst > 0 && last_dtype != 0xFF)
                printf("  fmt %s (Pc %04x)", iec61937_type_name(last_dtype), (unsigned)last_pc);
            else if (d_burst == 0)
                printf("  fmt PCM");

            if (seen_types_mask) {
                printf("  seen:");
                for (uint8_t t = 0; t < 32; t++)
                    if (seen_types_mask & (1u << t)) printf(" %s", iec61937_type_name(t));
            }
            if (rate_changes) printf("  rate-changes %lu", (unsigned long)rate_changes);
            if (lg_present) printf("  lg-vol %u%s", lg_volume, lg_muted ? "/mute" : "");
            /* NO `continue` here. An earlier version returned to the top of the loop after the
             * side-channel dump, which skipped the per-window counter reset below -- so `sub`,
             * `bursts`, `peak` and the rate min/max all accumulated instead of describing the
             * window, and the line reported 1906560 subframes a second climbing by exactly 96000.
             * The deltas were right; the reset never ran. `side_end` defers the newline instead. */
            side_end = true;
        } else {
            probe_pin();
            uint32_t pct = probe_samples_taken
                         ? (uint32_t)((uint64_t)probe_high * 100u / probe_samples_taken) : 0;
            printf("  stage %u  locks %lu unlocks %lu  GP%d: edges %lu/ms  duty %lu%%  (%lu samples)",
                   g_stage, (unsigned long)locks, (unsigned long)unlocks, SPDIF_DATA_PIN,
                   (unsigned long)probe_edges, (unsigned long)pct,
                   (unsigned long)probe_samples_taken);
            /* One diagnosis per line, chosen by the measurement rather than printed on every
             * line regardless -- a hint that is always shown carries no information. */
            printf("  [selftest: pull-up %lu%% @100us, %lu%% @25ms]",
                   (unsigned long)probe_pullup_duty, (unsigned long)probe_pullup_duty_slow);
            if (probe_edges == 0 && pct < 5
                && probe_pullup_duty < 10 && probe_pullup_duty_slow > 90)
                printf("\n           -> GP%d has a CAPACITIVE load, not a short: it is low after"
                       " 100us but high after 25ms, which is a ~100 nF to ground."
                       " That is C1 on the signal node -- the signature of a mirrored jack whose"
                       " WIRES were corrected but whose CAPS were not. Swap C1 and C2.",
                       SPDIF_DATA_PIN);
            else if (probe_edges == 0 && pct < 5 && probe_pullup_duty > 90)
                printf("\n           -> GP%d is UNDRIVEN (floats high under a pull-up, so the probe"
                       " is sound). Nothing is connected to it that drives it: check the Vout wire"
                       " from J1 pin 1 to Pico pin 20, and that the board is wired to pins"
                       " 18/20/36 at all", SPDIF_DATA_PIN);
            else if (probe_edges == 0 && pct < 5 && probe_pullup_duty_slow < 10)
                printf("\n           -> GP%d is HELD LOW against a pull-up even after 25 ms, so it"
                       " is a conductive path and not a capacitor. Either the ORJ-8 has no supply"
                       " (measure 3.3 V at J1 pin 3) or J1 pin 1 and pin 3 are swapped, which puts"
                       " GP%d on the chip's Vcc input", SPDIF_DATA_PIN, SPDIF_DATA_PIN);
            else if (probe_edges == 0 && pct < 5)
                printf("\n           -> pin low, and the pull-up self-test is inconclusive"
                       " (%lu%%). Suspect the probe before the board", (unsigned long)probe_pullup_duty);
            else if (probe_edges == 0 && pct > 95)
                printf("\n           -> pin stuck HIGH: the receiver is powered and idling"
                       " with no light. Check the fibre and that the TV is actually"
                       " outputting over optical");
            else if (probe_edges > 1000) {
                capture_runs();
                printf("\n           -> carrier present. Run lengths at %lu.%lu MS/s:"
                       " min %lu ns, mode %lu ns, max %lu ns over %lu runs",
                       (unsigned long)(cap_rate_ksps / 1000u),
                       (unsigned long)((cap_rate_ksps % 1000u) / 100u),
                       (unsigned long)cap_min_ns, (unsigned long)cap_mode_ns,
                       (unsigned long)cap_max_ns, (unsigned long)cap_runs);
                /* 48 kHz S/PDIF: one cell is 162.8 ns and runs are only ever 1, 2 or 3 cells. */
                if (cap_min_ns >= 120 && cap_min_ns <= 230)
                    printf("\n              min is ~1 cell (163 ns expected): the CARRIER IS"
                           " CORRECT and the fault is ours -- decoder config, not the board");
                else if (cap_min_ns > 230)
                    printf("\n              min %lu ns is FAR above the 163 ns cell: the signal is"
                           " too slow or transitions are missing. Suspect the ORJ-8, which has had"
                           " 3.3 V on its output pin -- fit a spare", (unsigned long)cap_min_ns);
                else
                    printf("\n              min %lu ns is BELOW a cell: noise or ringing on the"
                           " line, not clean biphase", (unsigned long)cap_min_ns);
            }
            else
                printf("\n           -> some activity but not a carrier: %lu edges/ms is far"
                       " below the ~6000 a 48 kHz S/PDIF cell rate gives",
                       (unsigned long)probe_edges);
        }

        if (locks != last_locks || unlocks != last_unlocks)
            printf("   *transition*");
        printf("\n");
        if (side_end) { side_report(force_side); force_side = false; side_end = false; }

        /* Reset the per-window extremes, keep the cumulative totals. */
        last_subframes = subframes; last_bursts = bursts;
        last_locks = locks; last_unlocks = unlocks;
        parity_base = parity;
        peak_mag = 0; near_overflows = 0;
        fifo_min = UINT32_MAX; fifo_max = 0;
        rate_min = 1e9f; rate_max = 0.0f;
    }
}
