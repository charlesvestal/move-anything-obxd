# OB-Xd for Move Anything

Virtual analog synthesizer module based on [OB-Xd](https://github.com/reales/OB-Xd) by Filatov Vadim (reales).

Emulates the classic Oberheim OB-X with polyphonic voices, analog-modeled filters, and rich modulation.

## Features

- 6-voice polyphonic (balanced for Move's ARM CPU)
- Saw and pulse oscillators with BLEP anti-aliasing
- 4-pole resonant filter with envelope modulation
- LFO with sine, square, and S&H waveforms
- Full ADSR envelopes for amplitude and filter
- Works standalone or as a sound generator in Signal Chain patches

## Prerequisites

- [Move Anything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Install

### Via Module Store (Recommended)

1. Launch Move Anything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Sound Generators** → **OB-Xd**
4. Select **Install**

### Quick Install (pre-built)

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything-obxd/main/obxd-module.tar.gz | \
  ssh ableton@move.local 'tar -xz -C /data/UserData/move-anything/modules/'
```

### Build from Source

Requires Docker (recommended) or ARM64 cross-compiler.

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

## Troubleshooting

**No sound:**
- Check that voices are playing (polyphony count shows in display)
- Try changing preset - some presets may have low volume
- Ensure MIDI is routed correctly if using external controller

**Harsh/clipping sound:**
- Lower the filter resonance - OB-Xd can self-oscillate at high resonance
- Reduce the cutoff frequency
- Lower unison detune if enabled

**CPU usage high:**
- Reduce unison (each unison voice doubles CPU load)
- Use fewer simultaneous notes

## License

GPL-3.0 - See [LICENSE](LICENSE)

Based on OB-Xd by Filatov Vadim, which is also GPL licensed.
