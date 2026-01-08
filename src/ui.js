/*
 * OB-Xd UI for Move Anything
 *
 * GPL-3.0 License
 */

import {
    MidiCC, MidiNoteOn, MidiNoteOff,
    MoveLeft, MoveRight, MoveUp, MoveDown,
    MoveMainKnob, MoveShift
} from '../../shared/constants.mjs';

/* State */
let paramBank = 0;  /* 0=Filter, 1=Osc, 2=Mod */
let octaveTranspose = 0;
let currentPreset = 0;
let shiftHeld = false;

/* Bank definitions */
const bankNames = ["Filter", "Oscillators", "Modulation"];
const bankParams = [
    /* Bank 0: Filter */
    ["cutoff", "resonance", "filter_env", "key_track", "attack", "decay", "sustain", "release"],
    /* Bank 1: Oscillators */
    ["osc1_wave", "osc1_pw", "osc2_wave", "osc2_pw", "osc2_detune", "osc_mix", "osc2_pitch", "noise"],
    /* Bank 2: Modulation */
    ["lfo_rate", "lfo_wave", "lfo_cutoff", "lfo_pitch", "lfo_pw", "vibrato", "unison", "portamento"]
];

/* Short display names (max 3 chars) */
const paramDisplayNames = [
    ["Cut", "Res", "Env", "Key", "Atk", "Dec", "Sus", "Rel"],
    ["Wv1", "PW1", "Wv2", "PW2", "Det", "Mix", "Pit", "Noi"],
    ["Rat", "Wve", "Flt", "Pit", "PW", "Vib", "Uni", "Por"]
];

/* Get parameter value from DSP (returns 0-100) */
function getParamValue(bank, idx) {
    let paramName = bankParams[bank][idx];
    let val = parseFloat(host_module_get_param(paramName) || "0");
    return Math.round(val * 100);
}

/* Knob CC mappings (knobs 1-8 = CC 71-78) */
const KNOB_CC_START = 71;

/* Helper: format value for display */
function formatValue(val) {
    let s = Math.round(val).toString();
    while (s.length < 3) s = ' ' + s;
    return s;
}

/* Update display */
function updateDisplay() {
    clear_screen();

    /* Line 1: Module name and bank */
    print(2, 2, "OB-Xd", 1);
    print(50, 2, "[" + bankNames[paramBank] + "]", 1);

    /* Line 2: Preset number and name */
    let presetStr = currentPreset.toString().padStart(3, '0');
    let presetName = host_module_get_param("preset_name") || "Init";
    print(2, 12, presetStr + " " + presetName.substring(0, 16), 1);

    /* Line 3: Params 1-4 names */
    let names = paramDisplayNames[paramBank];
    for (let i = 0; i < 4; i++) {
        print(2 + i * 32, 24, names[i], 1);
    }

    /* Line 4: Params 1-4 values */
    for (let i = 0; i < 4; i++) {
        print(2 + i * 32, 34, formatValue(getParamValue(paramBank, i)), 1);
    }

    /* Line 5: Params 5-8 names */
    for (let i = 4; i < 8; i++) {
        print(2 + (i - 4) * 32, 44, names[i], 1);
    }

    /* Line 6: Params 5-8 values */
    for (let i = 4; i < 8; i++) {
        print(2 + (i - 4) * 32, 54, formatValue(getParamValue(paramBank, i)), 1);
    }
}

/* Handle knob change (relative encoder) */
function handleKnob(knobIndex, value) {
    /* knobIndex: 0-7, value: 1 = CW, 127 = CCW */
    let delta = 0;
    if (value === 1) {
        delta = 2;  /* Clockwise - increase */
    } else if (value === 127) {
        delta = -2; /* Counter-clockwise - decrease */
    } else if (value < 64) {
        delta = value;  /* Fast CW */
    } else {
        delta = value - 128;  /* Fast CCW */
    }

    /* Get current value from DSP and apply delta */
    let currentVal = getParamValue(paramBank, knobIndex);
    currentVal += delta;
    if (currentVal < 0) currentVal = 0;
    if (currentVal > 100) currentVal = 100;

    /* Send normalized to DSP */
    let paramName = bankParams[paramBank][knobIndex];
    let normalized = currentVal / 100.0;
    host_module_set_param(paramName, normalized.toFixed(3));

    updateDisplay();
}

/* Switch bank */
function switchBank(delta) {
    paramBank += delta;
    if (paramBank < 0) paramBank = 2;
    if (paramBank > 2) paramBank = 0;

    host_module_set_param("param_bank", paramBank.toString());
    updateDisplay();
}

/* Change octave */
function changeOctave(delta) {
    octaveTranspose += delta;
    if (octaveTranspose < -4) octaveTranspose = -4;
    if (octaveTranspose > 4) octaveTranspose = 4;

    host_module_set_param("octave_transpose", octaveTranspose.toString());
    updateDisplay();
}

/* Change preset */
function changePreset(delta) {
    let presetCount = parseInt(host_module_get_param("preset_count") || "1");
    currentPreset += delta;
    if (currentPreset < 0) currentPreset = presetCount - 1;
    if (currentPreset >= presetCount) currentPreset = 0;

    host_module_set_param("preset", currentPreset.toString());
    updateDisplay();
}

/* Init */
globalThis.init = function() {
    console.log("OB-Xd UI initializing");
    updateDisplay();
};

/* Tick */
globalThis.tick = function() {
    updateDisplay();
};

/* Handle Move hardware MIDI */
globalThis.onMidiMessageInternal = function(data) {
    let status = data[0] & 0xF0;
    let d1 = data[1];
    let d2 = data[2];

    if (status === 0xB0) {  /* CC */
        /* Shift button */
        if (d1 === MoveShift) {
            shiftHeld = (d2 > 0);
            return;
        }

        /* Left/Right arrows - switch bank */
        if (d1 === MoveLeft && d2 > 0) {
            switchBank(-1);
            return;
        }
        if (d1 === MoveRight && d2 > 0) {
            switchBank(1);
            return;
        }

        /* Up/Down arrows - octave */
        if (d1 === MoveUp && d2 > 0) {
            changeOctave(1);
            return;
        }
        if (d1 === MoveDown && d2 > 0) {
            changeOctave(-1);
            return;
        }

        /* Jog wheel - preset browsing (relative encoder: 1-63=CW, 64-127=CCW) */
        if (d1 === MoveMainKnob) {
            if (d2 >= 1 && d2 <= 63) {
                changePreset(1);
            } else if (d2 >= 64 && d2 <= 127) {
                changePreset(-1);
            }
            return;
        }

        /* Knobs 1-8 (CC 71-78) */
        if (d1 >= KNOB_CC_START && d1 < KNOB_CC_START + 8) {
            let knobIndex = d1 - KNOB_CC_START;
            handleKnob(knobIndex, d2);
            return;
        }
    }
};

/* Handle external MIDI - pass through to DSP */
globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI is handled directly by DSP */
};
