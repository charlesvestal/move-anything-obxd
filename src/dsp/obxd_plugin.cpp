/*
 * OB-Xd Synth DSP Plugin for Move Anything
 *
 * Virtual analog synthesizer based on the Oberheim OB-X.
 * GPL-3.0 License - see LICENSE file.
 *
 * Based on OB-Xd by Filatov Vadim (reales)
 * https://github.com/reales/OB-Xd
 *
 * V2 API only - instance-based for multi-instance support
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

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

/* Plugin API v2 - instance-based */
#define MOVE_PLUGIN_API_VERSION_2 2

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*move_plugin_init_v2_fn)(const host_api_v1_t *host);
#define MOVE_PLUGIN_INIT_V2_SYMBOL "move_plugin_init_v2"
}

/* OB-Xd Engine */
#include "Engine/SynthEngine.h"

/* Constants */
#define MAX_VOICES 6  /* Balanced for ARM CPU */
#define MAX_PRESETS 128
#define MAX_PARAMS 100

/* Host API reference */
static const host_api_v1_t *g_host = NULL;

/* Preset storage structure */
struct Preset {
    char name[32];
    float params[MAX_PARAMS];
    int param_count;
};

/* Parameter names for UI display */
static const char* g_param_names[3][8] = {
    /* Bank 0: Filter */
    {"cutoff", "resonance", "filter_env", "key_track", "attack", "decay", "sustain", "release"},
    /* Bank 1: Oscillators */
    {"osc1_wave", "osc2_wave", "osc_mix", "noise", "pw", "osc2_det", "osc1_pitch", "osc2_pitch"},
    /* Bank 2: Modulation */
    {"lfo_rate", "lfo_wave", "lfo_cutoff", "lfo_pitch", "lfo_pw", "vibrato", "unison", "portamento"}
};

/* Parameter definitions for shadow UI - maps names to indices */
#include "param_helper.h"
static const param_def_t g_shadow_params[] = {
    /* Filter params (bank 0) */
    {"cutoff",      "Cutoff",      PARAM_TYPE_FLOAT, 0,  0.0f, 1.0f},
    {"resonance",   "Resonance",   PARAM_TYPE_FLOAT, 1,  0.0f, 1.0f},
    {"filter_env",  "Filter Env",  PARAM_TYPE_FLOAT, 2,  0.0f, 1.0f},
    {"attack",      "Attack",      PARAM_TYPE_FLOAT, 4,  0.0f, 1.0f},
    {"decay",       "Decay",       PARAM_TYPE_FLOAT, 5,  0.0f, 1.0f},
    {"sustain",     "Sustain",     PARAM_TYPE_FLOAT, 6,  0.0f, 1.0f},
    {"release",     "Release",     PARAM_TYPE_FLOAT, 7,  0.0f, 1.0f},
    /* Osc params (bank 1) */
    {"osc_mix",     "Osc Mix",     PARAM_TYPE_FLOAT, 10, 0.0f, 1.0f},
    {"osc2_detune", "Detune",      PARAM_TYPE_FLOAT, 13, 0.0f, 1.0f},
    {"noise",       "Noise",       PARAM_TYPE_FLOAT, 11, 0.0f, 1.0f},
    /* Mod params (bank 2) */
    {"lfo_rate",    "LFO Rate",    PARAM_TYPE_FLOAT, 16, 0.0f, 1.0f},
    {"vibrato",     "Vibrato",     PARAM_TYPE_FLOAT, 21, 0.0f, 1.0f},
    {"portamento",  "Portamento",  PARAM_TYPE_FLOAT, 23, 0.0f, 1.0f},
    {"unison",      "Unison",      PARAM_TYPE_FLOAT, 22, 0.0f, 1.0f},
};

/* =====================================================================
 * Shared utility functions
 * ===================================================================== */

/* Logging helper */
static void plugin_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[obxd] %s", msg);
        g_host->log(buf);
    }
}

/* Parse float from attribute value */
static float parse_attr_float(const char *start) {
    return atof(start);
}

/* Extract attribute value between quotes */
static const char* find_attr(const char *xml, const char *attr_name, char *buf, int buf_len) {
    char search[64];
    snprintf(search, sizeof(search), "%s=\"", attr_name);
    const char *pos = strstr(xml, search);
    if (!pos) return NULL;

    pos += strlen(search);
    const char *end = strchr(pos, '"');
    if (!end) return NULL;

    int len = end - pos;
    if (len >= buf_len) len = buf_len - 1;
    strncpy(buf, pos, len);
    buf[len] = '\0';
    return end + 1;
}

/* =====================================================================
 * Plugin API v2 - Instance-based API
 * ===================================================================== */

typedef struct {
    char module_dir[256];
    SynthEngine *synth;
    int current_preset;
    int preset_count;
    int param_bank;
    int octave_transpose;
    float tempo_bpm;
    char preset_name[64];
    float params[24];
    Preset presets[MAX_PRESETS];
    float output_gain;
} obxd_instance_t;

/* Forward declarations */
static void v2_init_default_patch(obxd_instance_t *inst);
static void v2_apply_preset(obxd_instance_t *inst, int preset_idx);
static void v2_apply_param(obxd_instance_t *inst, int bank, int idx, float value);
static int v2_load_bank(obxd_instance_t *inst, const char *bank_path);

/* v2 helper: Initialize default patch */
static void v2_init_default_patch(obxd_instance_t *inst) {
    SynthEngine *synth = inst->synth;

    synth->processVolume(1.0f);
    synth->setVoiceCount(MAX_VOICES / 8.0f);

    synth->processOsc1Saw(1.0f);
    synth->processOsc1Pulse(0.0f);
    synth->processOsc2Saw(1.0f);
    synth->processOsc2Pulse(0.0f);
    synth->processOsc1Mix(0.5f);
    synth->processOsc2Mix(0.5f);
    synth->processOsc2Det(0.1f);

    synth->processCutoff(0.7f);
    synth->processResonance(0.2f);
    synth->processFourPole(1.0f);
    synth->processFilterEnvelopeAmt(0.3f);

    synth->processLoudnessEnvelopeAttack(0.01f);
    synth->processLoudnessEnvelopeDecay(0.3f);
    synth->processLoudnessEnvelopeSustain(0.7f);
    synth->processLoudnessEnvelopeRelease(0.2f);

    synth->processFilterEnvelopeAttack(0.01f);
    synth->processFilterEnvelopeDecay(0.3f);
    synth->processFilterEnvelopeSustain(0.3f);
    synth->processFilterEnvelopeRelease(0.2f);

    inst->params[0] = 0.7f;
    inst->params[1] = 0.2f;
    inst->params[2] = 0.3f;
    inst->params[4] = 0.01f;
    inst->params[5] = 0.3f;
    inst->params[6] = 0.7f;
    inst->params[7] = 0.2f;

    snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");
}

/* v2 helper: Apply preset */
static void v2_apply_preset(obxd_instance_t *inst, int preset_idx) {
    if (preset_idx < 0 || preset_idx >= inst->preset_count) return;

    Preset *p = &inst->presets[preset_idx];
    SynthEngine *synth = inst->synth;
    snprintf(inst->preset_name, sizeof(inst->preset_name), "%s", p->name);

    if (p->param_count > 2) synth->processVolume(p->params[2]);
    if (p->param_count > 4) synth->processTune(p->params[4]);
    if (p->param_count > 5) synth->processOctave(p->params[5]);
    if (p->param_count > 13) synth->processPortamento(p->params[13]);
    if (p->param_count > 14) synth->processUnison(p->params[14]);
    if (p->param_count > 15) synth->processDetune(p->params[15]);
    if (p->param_count > 16) synth->processOsc2Det(p->params[16]);
    if (p->param_count > 17) synth->processLfoFrequency(p->params[17]);
    if (p->param_count > 18) synth->processLfoSine(p->params[18]);
    if (p->param_count > 19) synth->processLfoSquare(p->params[19]);
    if (p->param_count > 20) synth->processLfoSH(p->params[20]);
    if (p->param_count > 21) synth->processLfoAmt1(p->params[21]);
    if (p->param_count > 22) synth->processLfoAmt2(p->params[22]);
    if (p->param_count > 23) synth->processLfoOsc1(p->params[23]);
    if (p->param_count > 24) synth->processLfoOsc2(p->params[24]);
    if (p->param_count > 25) synth->processLfoFilter(p->params[25]);
    if (p->param_count > 26) synth->processLfoPw1(p->params[26]);
    if (p->param_count > 27) synth->processLfoPw2(p->params[27]);
    if (p->param_count > 28) synth->processOsc2HardSync(p->params[28]);
    if (p->param_count > 29) synth->processOsc2Xmod(p->params[29]);
    if (p->param_count > 30) synth->processOsc1Pitch(p->params[30]);
    if (p->param_count > 31) synth->processOsc2Pitch(p->params[31]);
    if (p->param_count > 32) synth->processPitchQuantization(p->params[32]);
    if (p->param_count > 33) synth->processOsc1Saw(p->params[33]);
    if (p->param_count > 34) synth->processOsc1Pulse(p->params[34]);
    if (p->param_count > 35) synth->processOsc2Saw(p->params[35]);
    if (p->param_count > 36) synth->processOsc2Pulse(p->params[36]);
    if (p->param_count > 37) synth->processPulseWidth(p->params[37]);
    if (p->param_count > 38) synth->processBrightness(p->params[38]);
    if (p->param_count > 39) synth->processEnvelopeToPitch(p->params[39]);
    if (p->param_count > 40) synth->processOsc1Mix(p->params[40]);
    if (p->param_count > 41) synth->processOsc2Mix(p->params[41]);
    if (p->param_count > 42) synth->processNoiseMix(p->params[42]);
    if (p->param_count > 43) synth->processFilterKeyFollow(p->params[43]);
    if (p->param_count > 44) synth->processCutoff(p->params[44]);
    if (p->param_count > 45) synth->processResonance(p->params[45]);
    if (p->param_count > 46) synth->processMultimode(p->params[46]);
    if (p->param_count > 48) synth->processBandpassSw(p->params[48]);
    if (p->param_count > 49) synth->processFourPole(p->params[49]);
    if (p->param_count > 50) synth->processFilterEnvelopeAmt(p->params[50]);
    if (p->param_count > 51) synth->processLoudnessEnvelopeAttack(p->params[51]);
    if (p->param_count > 52) synth->processLoudnessEnvelopeDecay(p->params[52]);
    if (p->param_count > 53) synth->processLoudnessEnvelopeSustain(p->params[53]);
    if (p->param_count > 54) synth->processLoudnessEnvelopeRelease(p->params[54]);
    if (p->param_count > 55) synth->processFilterEnvelopeAttack(p->params[55]);
    if (p->param_count > 56) synth->processFilterEnvelopeDecay(p->params[56]);
    if (p->param_count > 57) synth->processFilterEnvelopeSustain(p->params[57]);
    if (p->param_count > 58) synth->processFilterEnvelopeRelease(p->params[58]);
    if (p->param_count > 59) synth->processEnvelopeDetune(p->params[59]);
    if (p->param_count > 60) synth->processFilterDetune(p->params[60]);
    if (p->param_count > 61) synth->processPortamentoDetune(p->params[61]);

    if (p->param_count > 44) inst->params[0] = p->params[44];
    if (p->param_count > 45) inst->params[1] = p->params[45];
    if (p->param_count > 50) inst->params[2] = p->params[50];
    if (p->param_count > 43) inst->params[3] = p->params[43];
    if (p->param_count > 51) inst->params[4] = p->params[51];
    if (p->param_count > 52) inst->params[5] = p->params[52];
    if (p->param_count > 53) inst->params[6] = p->params[53];
    if (p->param_count > 54) inst->params[7] = p->params[54];
}

/* v2 helper: Apply parameter */
static void v2_apply_param(obxd_instance_t *inst, int bank, int idx, float value) {
    int param_idx = bank * 8 + idx;
    inst->params[param_idx] = value;
    SynthEngine *synth = inst->synth;

    switch (bank) {
        case 0:
            switch (idx) {
                case 0: synth->processCutoff(value); break;
                case 1: synth->processResonance(value); break;
                case 2: synth->processFilterEnvelopeAmt(value); break;
                case 3: synth->processFilterKeyFollow(value); break;
                case 4: synth->processLoudnessEnvelopeAttack(value); break;
                case 5: synth->processLoudnessEnvelopeDecay(value); break;
                case 6: synth->processLoudnessEnvelopeSustain(value); break;
                case 7: synth->processLoudnessEnvelopeRelease(value); break;
            }
            break;
        case 1:
            switch (idx) {
                case 0: synth->processOsc1Saw(value > 0.5f ? 1.0f : 0.0f);
                        synth->processOsc1Pulse(value > 0.5f ? 0.0f : 1.0f); break;
                case 1: synth->processOsc2Saw(value > 0.5f ? 1.0f : 0.0f);
                        synth->processOsc2Pulse(value > 0.5f ? 0.0f : 1.0f); break;
                case 2: synth->processOsc1Mix(value);
                        synth->processOsc2Mix(1.0f - value); break;
                case 3: synth->processNoiseMix(value); break;
                case 4: synth->processPulseWidth(value); break;
                case 5: synth->processOsc2Det(value); break;
                case 6: synth->processOsc1Pitch(value); break;
                case 7: synth->processOsc2Pitch(value); break;
            }
            break;
        case 2:
            switch (idx) {
                case 0: synth->processLfoFrequency(value); break;
                case 1: synth->processLfoSine(value > 0.5f ? 1.0f : 0.0f);
                        synth->processLfoSquare(value > 0.5f ? 0.0f : 1.0f); break;
                case 2: synth->processLfoFilter(value); break;
                case 3: synth->processLfoOsc1(value); synth->processLfoOsc2(value); break;
                case 4: synth->processLfoPw1(value); synth->processLfoPw2(value); break;
                case 5: synth->processLfoAmt1(value); break;  /* vibrato mapped to LFO amount */
                case 6: synth->processUnison(value); break;
                case 7: synth->processPortamento(value); break;
            }
            break;
    }
}

/* v2 helper: Load bank from FXB file */
static int v2_load_bank(obxd_instance_t *inst, const char *bank_path) {
    FILE *f = fopen(bank_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *data = (char*)malloc(size + 1);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);

    char *xml = NULL;
    for (long i = 0; i < size - 5; i++) {
        if (data[i] == '<' && data[i+1] == '?' && data[i+2] == 'x' &&
            data[i+3] == 'm' && data[i+4] == 'l') {
            xml = &data[i];
            break;
        }
    }
    if (!xml) { free(data); return -1; }

    inst->preset_count = 0;
    char *program = xml;
    char buf[256];

    while ((program = strstr(program, "<program ")) != NULL && inst->preset_count < MAX_PRESETS) {
        Preset *p = &inst->presets[inst->preset_count];
        memset(p, 0, sizeof(Preset));

        if (find_attr(program, "programName", buf, sizeof(buf))) {
            strncpy(p->name, buf, sizeof(p->name) - 1);
        } else {
            snprintf(p->name, sizeof(p->name), "Preset %d", inst->preset_count);
        }

        for (int i = 0; i < MAX_PARAMS; i++) {
            char attr_name[16];
            snprintf(attr_name, sizeof(attr_name), "Val_%d", i);
            if (find_attr(program, attr_name, buf, sizeof(buf))) {
                p->params[i] = parse_attr_float(buf);
                p->param_count = i + 1;
            }
        }

        inst->preset_count++;
        program++;
    }

    free(data);

    char msg[128];
    snprintf(msg, sizeof(msg), "Loaded %d presets from bank", inst->preset_count);
    plugin_log(msg);

    return inst->preset_count;
}

/* v2 API: Create instance */
static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    obxd_instance_t *inst = (obxd_instance_t*)calloc(1, sizeof(obxd_instance_t));
    if (!inst) return NULL;

    strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    inst->output_gain = 0.5f;
    inst->tempo_bpm = 120.0f;
    snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");

    inst->synth = new SynthEngine();
    if (!inst->synth) {
        free(inst);
        return NULL;
    }

    inst->synth->setSampleRate((float)MOVE_SAMPLE_RATE);
    inst->synth->setPlayHead(inst->tempo_bpm, 0.0f);

    v2_init_default_patch(inst);

    char bank_path[512];
    snprintf(bank_path, sizeof(bank_path), "%s/presets/factory.fxb", module_dir);
    if (v2_load_bank(inst, bank_path) > 0) {
        inst->current_preset = 0;
        v2_apply_preset(inst, 0);
    }

    plugin_log("OB-Xd v2: Instance created");
    return inst;
}

/* v2 API: Destroy instance */
static void v2_destroy_instance(void *instance) {
    obxd_instance_t *inst = (obxd_instance_t*)instance;
    if (!inst) return;

    if (inst->synth) {
        delete inst->synth;
    }
    free(inst);
    plugin_log("OB-Xd v2: Instance destroyed");
}

/* v2 API: MIDI handler */
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    obxd_instance_t *inst = (obxd_instance_t*)instance;
    if (!inst || !inst->synth || len < 2) return;

    (void)source;

    uint8_t status = msg[0] & 0xF0;
    uint8_t data1 = msg[1];
    uint8_t data2 = (len > 2) ? msg[2] : 0;

    int note = data1;
    if (status == 0x90 || status == 0x80) {
        note += inst->octave_transpose * 12;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    switch (status) {
        case 0x90:
            if (data2 > 0) {
                inst->synth->procNoteOn(note, data2 / 127.0f);
            } else {
                inst->synth->procNoteOff(note);
            }
            break;
        case 0x80:
            inst->synth->procNoteOff(note);
            break;
        case 0xB0:
            switch (data1) {
                case 1: inst->synth->procModWheel(data2 / 127.0f); break;
                case 64:
                    if (data2 >= 64) inst->synth->sustainOn();
                    else inst->synth->sustainOff();
                    break;
            }
            break;
        case 0xE0: {
            int bend = ((data2 << 7) | data1) - 8192;
            inst->synth->procPitchWheel(bend / 8192.0f);
            break;
        }
    }
}

/* v2 helper: Apply param by flat index (converts to bank/idx internally) */
static void v2_apply_param_flat(obxd_instance_t *inst, int flat_idx, float value) {
    if (flat_idx < 0 || flat_idx >= MAX_PARAMS) return;
    int bank = flat_idx / 8;
    int idx = flat_idx % 8;
    v2_apply_param(inst, bank, idx, value);
}

/* v2 API: Set parameter */
static void v2_set_param(void *instance, const char *key, const char *val) {
    obxd_instance_t *inst = (obxd_instance_t*)instance;
    if (!inst) return;

    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->preset_count) {
            inst->current_preset = idx;
            v2_apply_preset(inst, idx);
        }
    }
    else if (strcmp(key, "octave_transpose") == 0) {
        inst->octave_transpose = atoi(val);
        if (inst->octave_transpose < -3) inst->octave_transpose = -3;
        if (inst->octave_transpose > 3) inst->octave_transpose = 3;
    }
    else if (strcmp(key, "param_bank") == 0) {
        inst->param_bank = atoi(val);
        if (inst->param_bank < 0) inst->param_bank = 0;
        if (inst->param_bank > 2) inst->param_bank = 2;
    }
    else if (strncmp(key, "param_", 6) == 0) {
        int idx = atoi(key + 6);
        if (idx >= 0 && idx < 8) {
            float fval = atof(val);
            v2_apply_param(inst, inst->param_bank, idx, fval);
        }
    }
    else {
        /* Named parameter access via helper (for shadow UI) */
        float fval = (float)atof(val);
        /* Find the param and apply it */
        for (int i = 0; i < (int)PARAM_DEF_COUNT(g_shadow_params); i++) {
            if (strcmp(key, g_shadow_params[i].key) == 0) {
                /* Clamp value */
                if (fval < g_shadow_params[i].min_val) fval = g_shadow_params[i].min_val;
                if (fval > g_shadow_params[i].max_val) fval = g_shadow_params[i].max_val;
                v2_apply_param_flat(inst, g_shadow_params[i].index, fval);
                return;
            }
        }
    }
}

/* v2 API: Get parameter */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    obxd_instance_t *inst = (obxd_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", inst->current_preset);
    }
    if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", inst->preset_count);
    }
    if (strcmp(key, "preset_name") == 0) {
        return snprintf(buf, buf_len, "%s", inst->preset_name);
    }
    if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", inst->octave_transpose);
    }
    if (strcmp(key, "param_bank") == 0) {
        return snprintf(buf, buf_len, "%d", inst->param_bank);
    }
    if (strncmp(key, "param_name_", 11) == 0) {
        int idx = atoi(key + 11);
        if (idx >= 0 && idx < 8 && inst->param_bank >= 0 && inst->param_bank < 3) {
            return snprintf(buf, buf_len, "%s", g_param_names[inst->param_bank][idx]);
        }
    }
    if (strncmp(key, "param_", 6) == 0) {
        int idx = atoi(key + 6);
        if (idx >= 0 && idx < 8) {
            int param_idx = inst->param_bank * 8 + idx;
            return snprintf(buf, buf_len, "%.3f", inst->params[param_idx]);
        }
    }

    /* Named parameter access via helper (for shadow UI) */
    int result = param_helper_get(g_shadow_params, PARAM_DEF_COUNT(g_shadow_params),
                                  inst->params, key, buf, buf_len);
    if (result >= 0) return result;

    /* UI hierarchy for shadow parameter editor */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"list_param\":\"preset\","
                    "\"count_param\":\"preset_count\","
                    "\"name_param\":\"preset_name\","
                    "\"children\":\"filter\","
                    "\"knobs\":[],"
                    "\"params\":[]"
                "},"
                "\"filter\":{"
                    "\"children\":\"osc\","
                    "\"knobs\":[\"cutoff\",\"resonance\",\"filter_env\",\"attack\",\"decay\",\"sustain\",\"release\",\"octave_transpose\"],"
                    "\"params\":[\"cutoff\",\"resonance\",\"filter_env\",\"attack\",\"decay\",\"sustain\",\"release\",\"octave_transpose\"]"
                "},"
                "\"osc\":{"
                    "\"children\":\"mod\","
                    "\"knobs\":[\"osc_mix\",\"osc2_detune\",\"noise\"],"
                    "\"params\":[\"osc_mix\",\"osc2_detune\",\"noise\"]"
                "},"
                "\"mod\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"lfo_rate\",\"vibrato\",\"portamento\",\"unison\"],"
                    "\"params\":[\"lfo_rate\",\"vibrato\",\"portamento\",\"unison\"]"
                "}"
            "}"
        "}";
        int len = strlen(hierarchy);
        if (len < buf_len) {
            strcpy(buf, hierarchy);
            return len;
        }
        return -1;
    }

    /* Chain params metadata for shadow parameter editor */
    if (strcmp(key, "chain_params") == 0) {
        const char *params_json = "["
            "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3},"
            "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"resonance\",\"name\":\"Resonance\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"filter_env\",\"name\":\"Filter Env\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"sustain\",\"name\":\"Sustain\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"release\",\"name\":\"Release\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"osc_mix\",\"name\":\"Osc Mix\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"osc2_detune\",\"name\":\"Detune\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"noise\",\"name\":\"Noise\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"lfo_rate\",\"name\":\"LFO Rate\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"vibrato\",\"name\":\"Vibrato\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"portamento\",\"name\":\"Portamento\",\"type\":\"float\",\"min\":0,\"max\":1},"
            "{\"key\":\"unison\",\"name\":\"Unison\",\"type\":\"float\",\"min\":0,\"max\":1}"
        "]";
        int len = strlen(params_json);
        if (len < buf_len) {
            strcpy(buf, params_json);
            return len;
        }
        return -1;
    }

    return -1;
}

/* v2 API: Render audio */
static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    obxd_instance_t *inst = (obxd_instance_t*)instance;
    if (!inst || !inst->synth) {
        memset(out_interleaved_lr, 0, frames * 4);
        return;
    }

    for (int i = 0; i < frames; i++) {
        float left = 0.0f, right = 0.0f;
        inst->synth->processSample(&left, &right);

        left *= inst->output_gain;
        right *= inst->output_gain;

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

/* OB-Xd doesn't require external assets, so no load errors */
static int v2_get_error(void *instance, char *buf, int buf_len) {
    (void)instance;
    (void)buf;
    (void)buf_len;
    return 0;  /* No error */
}

/* v2 API table */
static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;

    return &g_plugin_api_v2;
}
