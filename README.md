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
| Up/Down | Octave transpose (-2 to +2) |
| Jog wheel | Browse presets / navigate menus |
| Knobs 1-8 | Adjust parameters for current category |

In Shadow UI / Signal Chain, parameters are organized into navigable categories.

## Parameters (67 total)

### Global
`volume`, `tune`, `octave`, `voice_count`, `legato`, `portamento`, `unison`, `unison_det`

### Oscillator 1
`osc1_saw`*, `osc1_pulse`*, `osc1_pitch`, `osc1_mix`

### Oscillator 2
`osc2_saw`*, `osc2_pulse`*, `osc2_pitch`, `osc2_mix`, `osc2_detune`, `osc2_halfstp`*

### Osc Common
`pw`, `pw_env`, `pw_env_both`*, `pw_ofs`, `noise`, `xmod`, `brightness`

### Filter
`cutoff`, `resonance`, `filter_env`, `key_follow`, `multimode`, `bandpass`*, `fourpole`*, `self_osc`*, `fenv_inv`*

### Filter Envelope
`f_attack`, `f_decay`, `f_sustain`, `f_release`, `vel_filter`

### Amp Envelope
`attack`, `decay`, `sustain`, `release`, `vel_amp`

### LFO
`lfo_rate`, `lfo_sin`*, `lfo_square`*, `lfo_sh`*, `lfo_sync`*, `lfo_amt1`, `lfo_amt2`

### LFO Destinations
`lfo_osc1`*, `lfo_osc2`*, `lfo_filter`*, `lfo_pw1`*, `lfo_pw2`*

### Pitch Mod
`env_pitch`, `env_pitch_both`*, `bend_range`, `bend_osc2`*, `vibrato`

*\* = toggle (on/off)*

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
