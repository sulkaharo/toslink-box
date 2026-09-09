# The board

Two ways to get S/PDIF into a Raspberry Pi. **Build one.** Both are the same three-pin optical
receiver and the same two capacitors; they differ only in what carries the bits onward.

## Which one

| | **A — Pico over USB** | **B — Pi 5 direct** |
|---|---|---|
| Path | ORJ-8 → Pico `GP15` → RP2040 PIO → USB | ORJ-8 → Pi `GPIO12` → RP1 PIO |
| Wires to the Pi | one USB lead | three, to the 40-pin header |
| Needs the header free | no | **yes** |
| Kernel requirements | none | cyclic DMA needs a **2026-09-03 or later** rpi kernel, and `piolib` built from source |
| Pi CPU cost | negligible | **50 MS/s oversampling, ~6.25 MB/s of DMA** |
| Host it works with | **anything with a USB port** — Linux, macOS, Windows, any Pi, a NAS | **Raspberry Pi 5 only** (needs the RP1 chip's PIO) |
| Firmware to flash | yes | no |
| Payload reaches software as | a USB serial stream (`pico/listen.py`) | a pipe |

**A is the one to build unless you have a reason not to.** The Pico does the S/PDIF work itself and
talks USB, so the host can be any machine with a USB port — there is nothing platform-specific about
it, nothing wired to the host, and nothing that can conflict with whatever else is plugged in. The
payload reaches software over a USB serial stream, which needs the glue in `pico/listen.py`.

**B** works end to end today with no firmware step, but only on a Pi 5, with a free header and a
recent kernel, and it spends real CPU there.

---

# The receiver, common to both

| Ref | Part | Note |
|---|---|---|
| J1 | Cliff **ORJ-8** / `FCR684208R` | 2.7–5.5 V, 16 Mbps, TTL out, `tr/tf ≤ 25 ns`. Buy two: long lead time, and reversed pins destroy it |
| C1 | 100 nF ceramic (`104`) | Supply bypass. Within **7 mm** of the lead frame |
| C2 | 30 pF (`300`), or 33 pF (`330`) | Vout-to-GND. **Required by the datasheet** |
| — | 100 Ω resistor ¼ W | Not a board part — a bring-up tool, see check 2 |
| — | Perfboard, 2.54 mm | 8 × 6 holes for board B, ~24 × 10 for board A |

**No buffer, no series resistor, no pull-up.** At a 3V3 supply the ORJ-8's output is already 3V3
logic (`VOH ≥ 2.4 V`) with edges inside 25 ns against a 163 ns minimum cell, so it drives a 3V3 GPIO
directly. A 74HC14 Schmitt buffer is sometimes added for hysteresis; it is optional insurance, and
if you fit one use two inverters in series so the polarity returns — biphase-mark coding is
polarity-insensitive anyway. Tie any unused inverter inputs to GND.

## Pin 1 is Vout, not Vcc

`Pin Function: 1 = Vout, 2 = GND, 3 = Vcc`, from Cliff drawing `FCR684208R` rev 2. **Feeding pin 1
with 3V3 can destroy the part** — it drives current through the output stage's ESD structure with no
supply to clamp against.

The rule that does not depend on how you mount it:

> **Hold the part with the optical port facing you and the pins pointing down. They read
> 3 · 2 · 1 left to right — so Vcc is on your left.**

## Mirroring the jack moves the capacitors too

GND is the middle pin, so reversing the orientation swaps the roles of the two **outer** pins and
leaves the middle alone. Everything attached to them changes together — **both wires and both
capacitors.** Move only the wires and C1's 100 nF ends up across the signal, which is **0.26 Ω at
the 6.144 MHz cell rate** and destroys every AC-3 frame.

## C1 and C2 are not interchangeable

| | at 6.144 MHz |
|---|---|
| **30 pF** (C2, across the signal) | ≈ 860 Ω — a light load |
| **100 nF** (C1, across the supply) | ≈ 0.26 Ω — a short |

Same shape of part, 3000× different behaviour. If C2 is unavailable, fit C1 and leave C2's spot
empty — a correct build, and the retrofit is two pads on the solder side.

## Mounting

Both capacitors go on the **solder side**, directly across J1's pads: ~2.5 mm from the lead frame
and clear of the jack body. **J1 pin 2 carries three joints** — the GND wire, C1 from one side, C2
from the other. Solder the wire first and take it off the pad at an angle, leaving copper on both
sides.

Lead length matters: leads are ~1 nH/mm, in series with the capacitor. C1 with short legs is
≈0.26 Ω at 6 MHz; 10 mm of total lead adds 10 nH and more than doubles that. Trim and bend flat.
C2, an 860 Ω element, is indifferent.

The jack drops onto ordinary perfboard: its locating holes sit 2.60 mm and 5.10 mm from the pin
line, within a tenth of a millimetre of one and two grid pitches. Ream those four to ⌀1.1 and
⌀1.7 mm.

---

# Board A — Pico over USB

Adds a Raspberry Pi Pico (RP2040), socketed, and a **data-capable** micro-USB lead. The lead is the
only supply and the only data path; nothing connects to the Pi.

## Pad map — port off the RIGHT edge, component side, from above

| pad, top → bottom | jack pin | wire to | cap to the MIDDLE pad |
|---|---|---|---|
| **top** | **P1 = Vout** | Pico **pin 20** (`GP15`) | **C2, 30 pF** |
| middle | P2 = GND | Pico **pin 18** (GND) | — |
| **bottom** | **P3 = Vcc** | Pico **pin 36** (`3V3(OUT)`) | **C1, 100 nF** |

```
  USB lead                                                            optical
  ◄────────┐                                                     ┌──────────►
           │   ┌── pins 40 … 21 ──────────────────────────────┐   │
           │   │   ·   ·   ·   ·   ⊕   ·   ·   ·   ·   ·   ·  │   │
           └───┤             pin 36                           │  ┌┴────────┐
               │        RASPBERRY PI PICO   (socketed)        │  │   J1    │
               │   ·   ·   ·   ·   ·   ·   ·   ·   ·   ⊖   ⊗  │  │  ORJ-8  │
               └── pins 1 … 20 ───────────────────────────────┘  └─────────┘
                                            pin 18 ─┘   └─ pin 20 (GP15)
                                            ⊕ +3V3   ⊖ GND   ⊗ signal
```

USB out one end, optical the other, so the cables never fight. The Pico sits in two 20-way female
strips **7 hole-pitches apart** — 17.78 mm, its own row spacing.

`GP15` is `pico_spdif_rx`'s default data pin, so the firmware runs unmodified. Keep the signal wire
under ~100 mm; the 3V3 run can be as long as the board needs, because C1 is the local reservoir.

---

# Board B — Pi 5 direct

The whole board is the jack, two capacitors and three wires. **Six holes of perfboard.**

## Pad map — port off the LEFT edge, component side, from above

The **mirror** of board A, because the port faces the other way:

| pad, top → bottom | jack pin | wire to | cap to the MIDDLE pad |
|---|---|---|---|
| **top** | **P3 = Vcc** | Pi header **pin 1** (3V3) | **C1, 100 nF** |
| middle | P2 = GND | Pi header **pin 34** (GND) | — |
| **bottom** | **P1 = Vout** | Pi header **pin 32** (`GPIO12`) | **C2, 30 pF** |

```
   optical                     1   2   3   4   5   6
  ◄────────┐              1    ·   ·   ·   ·   ·   ·
           │  ┌────────┐  2    ·   ⊕   o   O   ·   ·   ⊕ → Pi pin 1   (3V3)
           └──┤   J1   │  3    ·   ⊖   ·   ·   ·   ·   ⊖ → Pi pin 34  (GND)
              │ ORJ-8  │  4    ·   ⊗   o   O   ·   ·   ⊗ → Pi pin 32  (GPIO12)
              └────────┘  5    ·   ·   ·   ·   ·   ·
                                    o ream ⌀1.1   O ream ⌀1.7
```

**Twist the GPIO12 wire with the pin-34 ground return** and keep the pair under 100 mm. Pins 32 and
34 are adjacent on the header, which is what makes that easy.

## Connection list

| # | Net | From | To | Note |
|---|---|---|---|---|
| 1 | +3V3 | J1 pin 3 (Vcc) | Pi header **pin 1** | Pin 17 is the same rail |
| 2 | GND | J1 pin 2 (GND) | Pi header **pin 34** | Twist with #3 |
| 3 | **signal** | J1 pin 1 (Vout) | Pi header **pin 32** (`GPIO12`) | Twist with #2, under 100 mm |
| 4 | 3V3 ↔ GND | C1 100 nF | J1 pin 3 — pin 2 | Solder side. A shunt, not in series |
| 5 | signal ↔ GND | C2 30 pF | J1 pin 1 — pin 2 | Solder side |

## Coexisting with other boards

This board takes **three pins and nothing else**: 3V3, GND, and `GPIO12` for the signal. It claims
no bus, loads no driver and needs no device-tree overlay of its own, so the only question is whether
something else already wants those pins.

**Check, rather than assume:**

```sh
pinctrl get 0-27          # GPIO12 must read `GPIO12 = none`
```

Anything already driving `GPIO12` has to be unloaded first — a PWM output, a fan controller, or an
S/PDIF *transmitter* module, all of which land there by default on some setups because GPIO12 is
PWM0.

**Why GPIO12 is a safe pin to pick.** Add-on audio boards cluster in three places, and GPIO12 is
outside all of them:

| what a board typically uses | pins |
|---|---|
| I2S | GPIO18–21 — and GPIO22–27 as well on multi-lane cards |
| I2C control (codec registers, volume) | GPIO2, GPIO3 |
| Board ID EEPROM | GPIO0, GPIO1 |

So an I2S DAC or ADC — including eight-channel cards that use every data lane — does not touch
`GPIO12`. Confirm it on yours with the command above rather than trusting the table.

**If the header is physically occupied**, a stacking or pass-through header, or an extender, gets you
to pins 1, 32 and 34. That is a purchasing decision rather than a soldering one, and **board A avoids
the question entirely** — it uses no Pi pins at all.

---

# Bring-up

Four checks, either board. Each either passes or says which half is wrong. A multimeter covers the
first three.

1. **Supply, jack not fitted.** 3V3 to GND: `>1 MΩ`. Then powered, 3.30 V ±0.1 at the jack's pads.
2. **Jack alone, through the 100 Ω resistor.** Draw should be ~2.5 mA typical, 10 mA worst case —
   under 1 V across the resistor. **Anything near 3.3 V across it means the pins are reversed.
   Stop.** This resistor is the cheap insurance against the one mistake that destroys the part.
3. **The link is live, with a DC voltmeter.** Connect a source and set it to output over optical.
   Biphase-mark coding is DC-balanced by construction, so a working carrier holds `Vout` at **half
   the supply, about 1.65 V**, on an ordinary DMM. No light and it rests at a rail. One reading
   clears the source, the cable, the jack and the supply, with no scope.
4. **Edges, if a scope is available.** At 48 kHz the cell rate is 6.144 MHz and the shortest pulse
   is **163 ns**; the longest, inside a preamble, is 488 ns. The jack is specified at
   `tr/tf ≤ 25 ns` — about 15% of a cell.

On board A the firmware replaces checks 3 and 4: it reports lock and the measured rate, and a raw
pin probe distinguishes *floating*, *held low* and *carrier present*. The probe includes a pull-up
self-test, because "no edges, 0% duty" is also exactly what a probe reading the wrong pin reports.

# Numbers worth not re-deriving

* **48 kHz S/PDIF:** a frame is 2 subframes × 32 bits and biphase-mark doubles it → **6.144 MHz
  cells**, **163 ns** minimum pulse, 488 ns longest. Runs are only ever 1, 2 or 3 cells; the
  standard reserves 3-cell runs for preambles, which is what makes them unambiguous.
* **RP2040 PIO at 125 MHz** = ~32 clocks per cell. One PIO block, four state machines, **32
  instruction slots for the whole chip.**
* **AC-3:** a frame is 1536 samples and the burst period is `1536 << 2` = 6144 bytes, so at 48 kHz
  bursts arrive at exactly **31.25/s**. At 640 kbps the burst occupies 2560 of those bytes and has
  fully arrived ~13 ms into the period.
* **IEC 60958 side channels**, per subframe: slot 28 is V (validity), **29 is U (user data — 192
  bits per block per channel, no standard meaning)**, 30 is C (channel status), 31 is P (parity).
  A vendor control protocol, if there is one, lives in U.
