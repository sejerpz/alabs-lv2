# Rootline (aLabs) — LV2 prototype

A bass player that follows your guitar. Rootline tracks the **lowest note** you play (acoustic or electric guitar, standard, drop D, drop C or B tuning) and plays it one octave down with two timbres you can blend:

- **Synth**: a clean oscillator, phase-locked to the string; *Synth tone* adds harmonics, up to a saw-like sound.
- **Octaver**: OC-2 style, the string signal multiplied by a flip-flop at f/2; *Octaver grit* hardens the square wave.

On top of that it can tell melody from chords, remove the low note from the guitar so the bass takes its place, follow a solo in unison, and send the bass to its own output.

Not tested on real instruments yet: all figures below come from synthetic plucked strings (see [Tests](#tests)).

## Connecting

| Input mode | In 1 | In 2 |
|---|---|---|
| Mono | tracking **and** dry | not used |
| Dual | tracking (**magnetic** pickup) | dry (e.g. **piezo**) |

A magnetic pickup is the best tracking source: strong fundamental and no knocks from the guitar body.

Outputs: **Out** (main) and **Bass out** (bass only: synth + octaver, with its own level). With *Main out = Guitar only* and the Dwarf's two physical outputs on separate channels, guitar and bass are fully split, so the bass can get its own compressor, EQ or bass cab.

Place Rootline **before** any distortion or compression.

## Mode, Unison, Context

| | What it does |
|---|---|
| **Classic** | bass on the lowest note (up to *Track up to*) |
| **Smart mono** | the bass fades out (20 ms) during a fast single-note line; it comes back with a chord or after a pause |
| **Dual** | the tracked low note and all its harmonics are removed from the guitar (comb notch with an adaptively refined period): only the bass plays the low note, the other strings stay |
| **Unison** (switch) | overrides Mode: bass always on, no range limit (tracking up to E6), octave −1 / −2 / Auto; Auto folds the note into E2–E3 (82–165 Hz) |
| **Context** (control output) | Silence / Single notes / Melody / Chords / Unison, shown in the MOD UI |

How melody and chords are told apart: for every note (attack or pitch change) the tracked note is removed with a comb filter on its period, and the residual is checked between +35 and +80 ms. A single note leaves almost nothing; a chord does not (a power chord included: the fifth does not share the root's period). Three single notes closer than `1 / Melody sensitivity` seconds = melody; two chords in a row end it.

## Parameters

| Parameter | Range | Notes |
|---|---|---|
| Input mode | Mono / Dual | see [Connecting](#connecting) |
| Low latency | on/off | see below |
| Instrument | Acoustic / Electric | Electric: faster decisions and a more tolerant relative floor (−16 dB instead of −12) |
| Tuning | Standard E / Drop D / Drop C / B | notes below the lowest open string (−1 semitone) are ignored; B lowers the input high-pass to 30 Hz |
| Track up to | C2…A3 | notes above this limit produce no bass |
| Threshold | −70…−20 dB | minimum level for a note |
| Synth level / tone | dB / 0…1 | |
| Octaver level / grit | dB / 0…1 | off by default (−60 dB) |
| Attack / Release | 0.5–50 ms / 20–1000 ms | synth envelope, and octaver release |
| Dry level | dB | |
| Mode | Classic / Smart mono / Dual | see above |
| Melody sensitivity | 3–16 notes/s | single notes faster than this count as melody (default 7) |
| Unison / Unison octave | on/off, −1 / −2 / Auto | |
| Main out | Guitar + bass / Guitar only | |
| Bass out level | dB | |

### Tuning and Instrument

- **Tuning** helps when there is energy below the lowest string: thumps, palm slaps, rumble. With a 65 Hz thump on every note in standard tuning, the share of wrong bass in the first 50 ms drops from 51% to 20% (Low latency on) and from 41% to 1% (off). Without such energy it changes nothing.
- **Electric** on electric material: 2–8 ms faster attack, same accuracy (power chords, palm mute).

### Low latency

Measured offline on synthetic plucked strings C2…A3 at 48 kHz (single notes, acoustic and electric power chords, palm mute, piezo), JACK buffer and I/O excluded:

| | full level (50%), mean | wrong notes |
|---|---|---|
| **On** (default) | ~14 ms | 9/150, 8 of them on piezo power chords with the fifth +6 dB |
| **Off** | ~37 ms | 2/150 |

- **On**: 4th-order band filters and fast decisions.
- **Off**: 6th-order band filters, longer window, 4 ms hold, slower phase lock.

Steady-state intonation error is below 5 cents.

## How it works

The input is decimated to ~6 kHz and fed to a bank of 14 Chebyshev-II lowpass filters (grid C#2…D#4, every 2 semitones). Each band gives an analytic signal, an amplitude and a one-period energy-weighted frequency. The selected note is the lowest band whose dominant frequency lies below its own grid point and agrees with the two bands above (filter ringing from the pick attack does not). The synth phase advances by exactly half the band's phase increment, so the bass is one octave down with no added lag. Unison uses a second core at ~12 kHz with a grid up to B6. See the header of `src/rootline_dsp.hpp` for details.

## Measurements of the modes (`test/test_modes.py`)

**Context / Smart mono** (share of time in the right state, after the first notes):

| Case | Low latency on | off |
|---|---|---|
| scales at 8 and 12 notes/s legato; 8 notes/s with 40 ms overlap | melody 81–100% | 97–100% |
| slow scale, 3 notes/s | single notes 100% | 100% |
| strums at 3.3 and 6.7/s, power chords at 8/s, ringing arpeggio, double stops | chords 96–100% | 86–100% |

The bass mutes after about **3 notes** of a fast scale. It never muted on chords.

**Dual** (attenuation in the guitar, 100–400 ms after the attack): a single low note is cut by about 17–23 dB on the fundamental, 2nd and 3rd harmonic. In a chord the higher notes stay (C#4 over A2: 0/−4 dB), **except those that coincide with harmonics of the root** (octave, fifth above the octave, double octave: −2/−16 dB). In a mono signal, shared frequencies cannot be separated.

**Unison**: intonation within ±6 cents from E3 to E6, bass at 50% level in 5–12 ms.

**CPU** (x86 host, % of one core): Classic 0.3–0.5%, Dual 0.5%, Unison 1.2%. On the Dwarf's Cortex-A35 expect roughly 10×: ~3–5%, ~12% with Unison. This is an estimate, to be checked on the device.

## Build

```
make                                              # native build in build/rootline.lv2
make install                                      # into ~/.lv2
make CXX=aarch64-linux-gnu-g++ OLD_GLIBC=1        # Dwarf build with a generic cross compiler
make CXX=aarch64-linux-gnu-g++ OLD_GLIBC=1 publish [DWARF=192.168.52.1]
```

`publish` hot-installs the bundle through mod-ui (the plugin must not be in the current pedalboard). With mod-plugin-builder, `OLD_GLIBC` is not needed. The binary depends only on libc/libm (no libstdc++); with `OLD_GLIBC=1` it needs glibc 2.17 or later.

No modgui yet: the MOD UI shows the generic control panel.

## Tests

`test/` holds the offline tools used for the measurements:

- `gen_and_eval.py`, `summary.py`: synthetic signals; latency, intonation and purity.
- `test_modes.py`: context / Smart mono, Dual separation, Unison intonation.
- `bench.cpp`: CPU, denormals, robustness on silence, DC and full-scale noise.
- `host.cpp`: lilv host that loads the bundle with the TTL defaults.
- `dlhost.cpp`: dlopen host, used to run the aarch64 build under qemu.

```
make test
```

## Known limits / to verify on the Dwarf

- **Latency cannot go below ~6 ms.** Telling C2 from its octave takes about one period of signal (15 ms), and the band filter needs time to settle.
- **Piezo with power chords and a fifth much louder than the root**: the tracker sometimes picks the wrong note (8 chords out of 24 with Low latency on, 2 with it off). No errors with a magnetic pickup.
- **Body knocks** (mostly piezo/mic): the body resonance (~100 Hz) is above the lowest string, so Tuning does not remove it; under high notes it can give up to ~30 ms of wrong bass.
- **Smart mono**: the first 2–3 notes of a fast scale still have bass; a slow scale cannot be told from a bass riff and keeps the bass.
- **Dual**: the attack of the low note (10–15 ms, before the tracker locks) stays in the guitar.
- **Unison** follows the lowest note, so a low string left ringing under a solo keeps the bass on itself.
- **Track up to boundary**: with Low latency on, a note 1–2 semitones above the limit can give a short hint of bass on the attack.
- The bass is **monophonic** by design: on a chord only the lowest note is followed.
- **Low string left ringing** under a melody (typical on electric): the bass stays on the low string until it is about 16–20 dB below the other notes. A "last played note" priority was implemented and tested, but it was wrong in about a third of the cases (notes a fourth apart, repeated palm mutes), so it is not included.
- Previously named *AcouBass* (URI `urn:andrea:lv2:acoubass`): pedalboards saved with it must be rebuilt.
