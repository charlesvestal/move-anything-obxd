/*
 * OB-Xd Synth DSP Plugin for Move Anything
 *
 * Virtual analog synthesizer based on the Oberheim OB-X.
 * GPL-3.0 License - see LICENSE file.
 *
 * Based on OB-Xd by Filatov Vadim (reales)
 * https://github.com/reales/OB-Xd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Include plugin API */
extern "C" {
/* Copy plugin_api_v1.h definitions inline to avoid path issues */
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION 1
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128
#define MOVE_MIDI_SOURCE_INTERNAL 0
#define MOVE_MIDI_SOURCE_EXTERNAL 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *json_defaults);
    void (*on_unload)(void);
    void (*on_midi)(const uint8_t *msg, int len, int source);
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
    void (*render_block)(int16_t *out_interleaved_lr, int frames);
} plugin_api_v1_t;
}

/* OB-Xd Engine */
#include "Engine/SynthEngine.h"

/* Constants */
#define MAX_VOICES 4  /* Reduced for ARM CPU */

/* Plugin state */
static const host_api_v1_t *g_host = NULL;
static SynthEngine g_synth;
static plugin_api_v1_t g_plugin_api;

static int g_current_preset = 0;
static int g_preset_count = 0;
static int g_param_bank = 0;  /* 0=Filter, 1=Osc, 2=Mod */
static int g_octave_transpose = 0;
static float g_tempo_bpm = 120.0f;
static char g_preset_name[64] = "Init";

/* Parameter values (0.0 - 1.0 normalized) */
static float g_params[24] = {0};  /* 3 banks x 8 params */

/* Parameter names for banks */
static const char* g_param_names[3][8] = {
    /* Bank 0: Filter */
    {"cutoff", "resonance", "filter_env", "key_track", "attack", "decay", "sustain", "release"},
    /* Bank 1: Oscillators */
    {"osc1_wave", "osc1_pw", "osc2_wave", "osc2_pw", "osc2_detune", "osc_mix", "osc2_pitch", "noise"},
    /* Bank 2: Modulation */
    {"lfo_rate", "lfo_wave", "lfo_cutoff", "lfo_pitch", "lfo_pw", "vibrato", "unison", "portamento"}
};

/* Helper: log via host */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[obxd] %s", msg);
        g_host->log(buf);
    }
}

/* Apply parameter to synth engine */
static void apply_param(int bank, int idx, float value) {
    int param_idx = bank * 8 + idx;
    g_params[param_idx] = value;

    switch (bank) {
        case 0: /* Filter bank */
            switch (idx) {
                case 0: g_synth.processCutoff(value); break;
                case 1: g_synth.processResonance(value); break;
                case 2: g_synth.processFilterEnvelopeAmt(value); break;
                case 3: g_synth.processFilterKeyFollow(value); break;
                case 4: g_synth.processLoudnessEnvelopeAttack(value); break;
                case 5: g_synth.processLoudnessEnvelopeDecay(value); break;
                case 6: g_synth.processLoudnessEnvelopeSustain(value); break;
                case 7: g_synth.processLoudnessEnvelopeRelease(value); break;
            }
            break;

        case 1: /* Oscillator bank */
            switch (idx) {
                case 0: /* osc1 wave - saw/pulse toggle */
                    g_synth.processOsc1Saw(value > 0.5f ? 1.0f : 0.0f);
                    g_synth.processOsc1Pulse(value <= 0.5f ? 1.0f : 0.0f);
                    break;
                case 1: g_synth.processPulseWidth(value); break;
                case 2: /* osc2 wave */
                    g_synth.processOsc2Saw(value > 0.5f ? 1.0f : 0.0f);
                    g_synth.processOsc2Pulse(value <= 0.5f ? 1.0f : 0.0f);
                    break;
                case 3: g_synth.processPulseWidth(value); break; /* TODO: separate PW2 */
                case 4: g_synth.processOsc2Det(value); break;
                case 5:
                    g_synth.processOsc1Mix(1.0f - value);
                    g_synth.processOsc2Mix(value);
                    break;
                case 6: g_synth.processOsc2Pitch(value); break;
                case 7: g_synth.processNoiseMix(value); break;
            }
            break;

        case 2: /* Modulation bank */
            switch (idx) {
                case 0: g_synth.processLfoFrequency(value); break;
                case 1: /* LFO wave - cycle through shapes */
                    g_synth.processLfoSine(value < 0.33f ? 1.0f : 0.0f);
                    g_synth.processLfoSquare(value >= 0.33f && value < 0.66f ? 1.0f : 0.0f);
                    g_synth.processLfoSH(value >= 0.66f ? 1.0f : 0.0f);
                    break;
                case 2: g_synth.processLfoFilter(value > 0.5f ? 1.0f : 0.0f); break;
                case 3:
                    g_synth.processLfoOsc1(value > 0.5f ? 1.0f : 0.0f);
                    g_synth.processLfoOsc2(value > 0.5f ? 1.0f : 0.0f);
                    break;
                case 4:
                    g_synth.processLfoPw1(value > 0.5f ? 1.0f : 0.0f);
                    g_synth.processLfoPw2(value > 0.5f ? 1.0f : 0.0f);
                    break;
                case 5: g_synth.procModWheelFrequency(value); break;
                case 6: g_synth.processUnison(value); break;
                case 7: g_synth.processPortamento(value); break;
            }
            break;
    }
}

/* Output gain (boost the quiet OB-Xd output) */
static float g_output_gain = 4.0f;

/* Initialize default patch */
static void init_default_patch() {
    /* Set up a basic saw + filter patch */
    g_synth.processVolume(1.0f);  /* Max internal volume */
    g_synth.setVoiceCount(MAX_VOICES / 8.0f);  /* Normalized 0-1 for voice count */

    /* Oscillators */
    g_synth.processOsc1Saw(1.0f);
    g_synth.processOsc1Pulse(0.0f);
    g_synth.processOsc2Saw(1.0f);
    g_synth.processOsc2Pulse(0.0f);
    g_synth.processOsc1Mix(0.5f);
    g_synth.processOsc2Mix(0.5f);
    g_synth.processOsc2Det(0.1f);

    /* Filter */
    g_synth.processCutoff(0.7f);
    g_synth.processResonance(0.2f);
    g_synth.processFourPole(1.0f);
    g_synth.processFilterEnvelopeAmt(0.3f);

    /* Envelopes */
    g_synth.processLoudnessEnvelopeAttack(0.01f);
    g_synth.processLoudnessEnvelopeDecay(0.3f);
    g_synth.processLoudnessEnvelopeSustain(0.7f);
    g_synth.processLoudnessEnvelopeRelease(0.2f);

    g_synth.processFilterEnvelopeAttack(0.01f);
    g_synth.processFilterEnvelopeDecay(0.3f);
    g_synth.processFilterEnvelopeSustain(0.3f);
    g_synth.processFilterEnvelopeRelease(0.2f);

    /* Store normalized values */
    g_params[0] = 0.7f;  /* cutoff */
    g_params[1] = 0.2f;  /* resonance */
    g_params[2] = 0.3f;  /* filter_env */
    g_params[4] = 0.01f; /* attack */
    g_params[5] = 0.3f;  /* decay */
    g_params[6] = 0.7f;  /* sustain */
    g_params[7] = 0.2f;  /* release */

    snprintf(g_preset_name, sizeof(g_preset_name), "Init");
}

/* === Plugin API callbacks === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    char msg[256];
    snprintf(msg, sizeof(msg), "OB-Xd plugin loading from: %s", module_dir);
    plugin_log(msg);

    /* Initialize synth engine */
    g_synth.setSampleRate((float)MOVE_SAMPLE_RATE);

    /* Set default tempo */
    g_synth.setPlayHead(g_tempo_bpm, 0.0f);

    /* Initialize with default patch */
    init_default_patch();

    plugin_log("OB-Xd plugin loaded");
    return 0;
}

static void plugin_on_unload(void) {
    plugin_log("OB-Xd plugin unloading");
    g_synth.allSoundOff();
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    if (len < 2) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    /* Filter capacitive touch from Move knobs */
    int is_note = (status == 0x90 || status == 0x80);
    if (is_note && data1 < 10 && source == MOVE_MIDI_SOURCE_INTERNAL) {
        return;
    }

    /* Apply octave transpose */
    int note = data1;
    if (is_note) {
        note += g_octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90: /* Note On */
            if (data2 > 0) {
                g_synth.procNoteOn(note, data2 / 127.0f);
            } else {
                g_synth.procNoteOff(note);
            }
            break;

        case 0x80: /* Note Off */
            g_synth.procNoteOff(note);
            break;

        case 0xB0: /* Control Change */
            switch (data1) {
                case 1:  /* Mod wheel */
                    g_synth.procModWheel(data2 / 127.0f);
                    break;
                case 64: /* Sustain pedal */
                    if (data2 >= 64) {
                        g_synth.sustainOn();
                    } else {
                        g_synth.sustainOff();
                    }
                    break;
                case 123: /* All notes off */
                    g_synth.allNotesOff();
                    break;
            }
            break;

        case 0xE0: /* Pitch Bend */
            {
                int bend = ((int)data2 << 7) | data1;
                float normalized = (bend - 8192) / 8192.0f;
                g_synth.procPitchWheel(normalized * 0.5f + 0.5f);
            }
            break;
    }
}

static void plugin_set_param(const char *key, const char *val) {
    float fval = atof(val);

    if (strcmp(key, "param_bank") == 0) {
        g_param_bank = (int)fval;
        if (g_param_bank < 0) g_param_bank = 0;
        if (g_param_bank > 2) g_param_bank = 2;
    } else if (strcmp(key, "octave_transpose") == 0) {
        g_octave_transpose = (int)fval;
        if (g_octave_transpose < -4) g_octave_transpose = -4;
        if (g_octave_transpose > 4) g_octave_transpose = 4;
    } else if (strcmp(key, "tempo") == 0) {
        g_tempo_bpm = fval;
        g_synth.setPlayHead(g_tempo_bpm, 0.0f);
    } else if (strcmp(key, "preset") == 0) {
        g_current_preset = (int)fval;
        /* TODO: Load preset from .fxb file */
    } else if (strcmp(key, "all_notes_off") == 0) {
        g_synth.allNotesOff();
    } else if (strcmp(key, "panic") == 0) {
        g_synth.allSoundOff();
    } else {
        /* Check parameter names */
        for (int bank = 0; bank < 3; bank++) {
            for (int idx = 0; idx < 8; idx++) {
                if (strcmp(key, g_param_names[bank][idx]) == 0) {
                    apply_param(bank, idx, fval);
                    return;
                }
            }
        }
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "preset_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "%s", g_preset_name);
    } else if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", g_current_preset);
    } else if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", g_preset_count);
    } else if (strcmp(key, "param_bank") == 0) {
        return snprintf(buf, buf_len, "%d", g_param_bank);
    } else if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", g_octave_transpose);
    } else if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "%d", MAX_VOICES);
    } else {
        /* Check parameter names */
        for (int bank = 0; bank < 3; bank++) {
            for (int idx = 0; idx < 8; idx++) {
                if (strcmp(key, g_param_names[bank][idx]) == 0) {
                    return snprintf(buf, buf_len, "%.3f", g_params[bank * 8 + idx]);
                }
            }
        }
    }

    return -1;
}

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    for (int i = 0; i < frames; i++) {
        float left = 0.0f, right = 0.0f;

        /* Process one sample */
        g_synth.processSample(&left, &right);

        /* Apply output gain boost */
        left *= g_output_gain;
        right *= g_output_gain;

        /* Convert float to int16 with clipping */
        int32_t l = (int32_t)(left * 32767.0f);
        int32_t r = (int32_t)(right * 32767.0f);

        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;

        out_interleaved_lr[i * 2] = (int16_t)l;
        out_interleaved_lr[i * 2 + 1] = (int16_t)r;
    }
}

/* === Plugin entry point === */

extern "C" plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    g_host = host;

    if (host->api_version != MOVE_PLUGIN_API_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "API version mismatch: host=%d, plugin=%d",
                 host->api_version, MOVE_PLUGIN_API_VERSION);
        if (host->log) host->log(msg);
        return NULL;
    }

    memset(&g_plugin_api, 0, sizeof(g_plugin_api));
    g_plugin_api.api_version = MOVE_PLUGIN_API_VERSION;
    g_plugin_api.on_load = plugin_on_load;
    g_plugin_api.on_unload = plugin_on_unload;
    g_plugin_api.on_midi = plugin_on_midi;
    g_plugin_api.set_param = plugin_set_param;
    g_plugin_api.get_param = plugin_get_param;
    g_plugin_api.render_block = plugin_render_block;

    return &g_plugin_api;
}
