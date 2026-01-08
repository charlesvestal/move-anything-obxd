# OB-Xd for Move Anything

Virtual analog synthesizer module based on [OB-Xd](https://github.com/reales/OB-Xd) by Filatov Vadim (reales).

Emulates the classic Oberheim OB-X with polyphonic voices, analog-modeled filters, and rich modulation.

## Features

- 4-voice polyphonic (optimized for Move's ARM CPU)
- Saw and pulse oscillators with BLEP anti-aliasing
- 4-pole resonant filter with envelope modulation
- LFO with sine, square, and S&H waveforms
- Full ADSR envelopes for amplitude and filter
- Works standalone or as a sound generator in Signal Chain patches

## Install

Requires ARM64 cross-compiler or Docker.

```bash
git clone https://github.com/charlesvestal/move-anything-obxd
cd move-anything-obxd
./scripts/build.sh
./scripts/install.sh
```

This also installs chain presets for using OB-Xd with arpeggiators and effects.

## Controls

| Control | Function |
|---------|----------|
| Left/Right | Switch parameter bank (Filter/Osc/Mod) |
| Up/Down | Octave transpose |
| Jog wheel | Browse presets |
| Knobs 1-8 | Adjust current bank's parameters |

### Parameter Banks

**Filter:** Cutoff, Resonance, Env Amount, Key Track, Attack, Decay, Sustain, Release

**Oscillators:** Osc1 Wave, Osc1 PW, Osc2 Wave, Osc2 PW, Detune, Mix, Osc2 Pitch, Noise

**Modulation:** LFO Rate, LFO Wave, LFO→Filter, LFO→Pitch, LFO→PW, Vibrato, Unison, Portamento

## License

GPL-3.0 - See [LICENSE](LICENSE)

Based on OB-Xd by Filatov Vadim, which is also GPL licensed.

## Requirements

- [Move Anything](https://github.com/charlesvestal/move-anything) must be installed on your Move
