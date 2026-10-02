# aLabs LV2

A collection of LV2 plugins for the [MOD Dwarf](https://mod.audio/dwarf/) and other MOD devices, written by me out of necessity or just for fun. Each plugin lives in its own folder, with its own README, build instructions and modgui.

## Plugins

| Plugin | Description |
|---|---|
| [ExpSense](expsense/) | Reads a passive expression pedal through the Dwarf's audio I/O, with no extra hardware, and turns it into volume, CV and MIDI CC. |
| [Rootline](rootline/) | A bass player that follows your guitar: tracks the lowest note and plays it an octave down (synth and OC-2 style octaver), with melody/chord detection, low-note removal from the guitar, unison mode and a separate bass output. |
| [Euclides](euclides/) | A Euclidean MIDI sequencer: 5 independent patterns (kick, snare, closed/open hat, zap), each with its own steps/pulses/rotation, host- or internal-clocked. Pairs with BlipMachine. |
| [BlipMachine](blipmachine/) | A 5-voice synthesized percussion module (kick, snare, closed/open hat, zap), triggered by MIDI note — the sound counterpart to Euclides, but usable with any MIDI source. |

## License

See [LICENSE](LICENSE).
