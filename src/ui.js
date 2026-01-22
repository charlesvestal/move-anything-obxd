/*
 * OB-Xd UI for Move Anything
 *
 * Uses shared sound generator UI base for consistent preset browsing.
 * Parameter editing via shadow UI hierarchy when in chain context.
 *
 * GPL-3.0 License
 */

/* Shared utilities - absolute path for module location independence */
import { createSoundGeneratorUI } from '/data/UserData/move-anything/shared/sound_generator_ui.mjs';

/* Create the UI with OB-Xd-specific customizations */
const ui = createSoundGeneratorUI({
    moduleName: 'OB-Xd',

    onInit: (state) => {
        /* Any OB-Xd specific initialization */
    },

    onTick: (state) => {
        /* Any OB-Xd specific per-tick updates */
    },

    onPresetChange: (preset) => {
        /* OB-Xd may need voice reset on preset change */
        host_module_set_param('all_notes_off', '1');
    },

    showPolyphony: true,
    showOctave: true,
});

/* Export required callbacks */
globalThis.init = ui.init;
globalThis.tick = ui.tick;
globalThis.onMidiMessageInternal = ui.onMidiMessageInternal;
globalThis.onMidiMessageExternal = ui.onMidiMessageExternal;
