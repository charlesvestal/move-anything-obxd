# CLAUDE.md

Instructions for Claude Code when working with this repository.

## Project Overview

OB-Xd module for Move Anything - an Oberheim OB-X synthesizer emulator based on the OB-Xd project.

## Build Commands

```bash
./scripts/build.sh      # Build with Docker (recommended)
./scripts/install.sh    # Deploy to Move
```

## Architecture

```
src/
  dsp/
    obxd_plugin.cpp     # Main plugin wrapper
    Engine/             # OB-Xd synth engine
      SynthEngine.h     # Main synthesis
      Voice.h           # Voice management
      Filter.h          # 4-pole filter
      Oscillator.h      # BLEP oscillators
      Lfo.h             # LFO
  presets/              # Factory presets (.h files)
  ui.js                 # JavaScript UI with parameter banks
  module.json           # Module metadata
  chain_patches/        # Signal Chain presets
```

## Key Implementation Details

### Plugin API

Implements Move Anything plugin_api_v1:
- `on_load`: Initializes synth engine, loads presets
- `on_midi`: Routes to synth engine
- `set_param`: preset, octave_transpose, parameter values (cutoff, resonance, etc.)
- `get_param`: preset_name, octave_transpose, polyphony, parameter values
- `render_block`: Renders synth output

### Parameter Banks

UI organizes parameters into three banks controlled by Left/Right buttons:
- **Filter**: Cutoff, Resonance, Env Amount, Key Track, ADSR
- **Oscillators**: Wave shapes, pulse width, detune, mix
- **Modulation**: LFO rate/wave, mod depths, vibrato, unison, portamento

### Voice Management

4-voice polyphonic (optimized for Move's ARM CPU). Voice allocation with note stealing.

## Signal Chain Integration

Module declares `"chainable": true` and `"component_type": "sound_generator"` in module.json. Chain presets installed to main repo's `modules/chain/patches/` by install script.

## License

GPL-3.0 (inherited from OB-Xd)
