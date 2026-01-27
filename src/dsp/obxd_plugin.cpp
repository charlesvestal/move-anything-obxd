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

/* Parameter definitions for shadow UI - maps names to engine indices from ParamsEnum.h */
#include "param_helper.h"
#include "Engine/ParamsEnum.h"

static const param_def_t g_shadow_params[] = {
    /* Global - continuous */
    {"volume",        "Volume",        PARAM_TYPE_FLOAT, VOLUME,        0.0f, 1.0f},
    {"tune",          "Tune",          PARAM_TYPE_FLOAT, TUNE,          0.0f, 1.0f},
    {"portamento",    "Portamento",    PARAM_TYPE_FLOAT, PORTAMENTO,    0.0f, 1.0f},
    {"unison_det",    "Uni Detune",    PARAM_TYPE_FLOAT, UDET,          0.0f, 1.0f},
    /* Global - stepped/toggle */
    {"octave",        "Octave",        PARAM_TYPE_INT,   OCTAVE,        0.0f, 1.0f},  /* 5 steps: -2 to +2 */
    {"voice_count",   "Voices",        PARAM_TYPE_INT,   VOICE_COUNT,   0.0f, 1.0f},  /* 1-8 voices */
    {"legato",        "Legato",        PARAM_TYPE_INT,   LEGATOMODE,    0.0f, 1.0f},  /* 4 modes: 0-3 */
    {"unison",        "Unison",        PARAM_TYPE_INT,   UNISON,        0.0f, 1.0f},  /* toggle */

    /* Oscillator 1 - continuous */
    {"osc1_pitch",    "Osc1 Pitch",    PARAM_TYPE_FLOAT, OSC1P,         0.0f, 1.0f},
    {"osc1_mix",      "Osc1 Mix",      PARAM_TYPE_FLOAT, OSC1MIX,       0.0f, 1.0f},
    /* Oscillator 1 - toggle */
    {"osc1_saw",      "Osc1 Saw",      PARAM_TYPE_INT,   OSC1Saw,       0.0f, 1.0f},  /* toggle */
    {"osc1_pulse",    "Osc1 Pulse",    PARAM_TYPE_INT,   OSC1Pul,       0.0f, 1.0f},  /* toggle */

    /* Oscillator 2 - continuous */
    {"osc2_pitch",    "Osc2 Pitch",    PARAM_TYPE_FLOAT, OSC2P,         0.0f, 1.0f},
    {"osc2_mix",      "Osc2 Mix",      PARAM_TYPE_FLOAT, OSC2MIX,       0.0f, 1.0f},
    {"osc2_detune",   "Osc2 Detune",   PARAM_TYPE_FLOAT, OSC2_DET,      0.0f, 1.0f},
    /* Oscillator 2 - toggle */
    {"osc2_saw",      "Osc2 Saw",      PARAM_TYPE_INT,   OSC2Saw,       0.0f, 1.0f},  /* toggle */
    {"osc2_pulse",    "Osc2 Pulse",    PARAM_TYPE_INT,   OSC2Pul,       0.0f, 1.0f},  /* toggle */
    {"osc2_sync",     "Osc2 Sync",     PARAM_TYPE_INT,   OSC2HS,        0.0f, 1.0f},  /* toggle */

    /* Oscillator Common - continuous */
    {"pw",            "Pulse Width",   PARAM_TYPE_FLOAT, PW,            0.0f, 1.0f},
    {"pw_env",        "PW Env Amt",    PARAM_TYPE_FLOAT, PW_ENV,        0.0f, 1.0f},
    {"pw_ofs",        "PW Osc2 Ofs",   PARAM_TYPE_FLOAT, PW_OSC2_OFS,   0.0f, 1.0f},
    {"noise",         "Noise",         PARAM_TYPE_FLOAT, NOISEMIX,      0.0f, 1.0f},
    {"xmod",          "X-Mod",         PARAM_TYPE_FLOAT, XMOD,          0.0f, 1.0f},
    {"brightness",    "Brightness",    PARAM_TYPE_FLOAT, BRIGHTNESS,    0.0f, 1.0f},
    /* Oscillator Common - toggle */
    {"pw_env_both",   "PW Env Both",   PARAM_TYPE_INT,   PW_ENV_BOTH,   0.0f, 1.0f},  /* toggle */

    /* Filter - continuous */
    {"cutoff",        "Cutoff",        PARAM_TYPE_FLOAT, CUTOFF,        0.0f, 1.0f},
    {"resonance",     "Resonance",     PARAM_TYPE_FLOAT, RESONANCE,     0.0f, 1.0f},
    {"filter_env",    "Filter Env",    PARAM_TYPE_FLOAT, ENVELOPE_AMT,  0.0f, 1.0f},
    {"key_follow",    "Key Follow",    PARAM_TYPE_FLOAT, FLT_KF,        0.0f, 1.0f},
    {"multimode",     "Multimode",     PARAM_TYPE_FLOAT, MULTIMODE,     0.0f, 1.0f},
    /* Filter - toggle */
    {"bandpass",      "Bandpass",      PARAM_TYPE_INT,   BANDPASS,      0.0f, 1.0f},  /* toggle */
    {"fourpole",      "4-Pole",        PARAM_TYPE_INT,   FOURPOLE,      0.0f, 1.0f},  /* toggle */
    {"self_osc",      "Self Osc",      PARAM_TYPE_INT,   SELF_OSC_PUSH, 0.0f, 1.0f},  /* toggle */
    {"fenv_inv",      "F.Env Invert",  PARAM_TYPE_INT,   FENV_INVERT,   0.0f, 1.0f},  /* toggle */

    /* Filter Envelope - continuous */
    {"f_attack",      "F Attack",      PARAM_TYPE_FLOAT, FATK,          0.0f, 1.0f},
    {"f_decay",       "F Decay",       PARAM_TYPE_FLOAT, FDEC,          0.0f, 1.0f},
    {"f_sustain",     "F Sustain",     PARAM_TYPE_FLOAT, FSUS,          0.0f, 1.0f},
    {"f_release",     "F Release",     PARAM_TYPE_FLOAT, FREL,          0.0f, 1.0f},
    {"vel_filter",    "Vel>Filter",    PARAM_TYPE_FLOAT, VFLTENV,       0.0f, 1.0f},

    /* Amp Envelope - continuous */
    {"attack",        "Attack",        PARAM_TYPE_FLOAT, LATK,          0.0f, 1.0f},
    {"decay",         "Decay",         PARAM_TYPE_FLOAT, LDEC,          0.0f, 1.0f},
    {"sustain",       "Sustain",       PARAM_TYPE_FLOAT, LSUS,          0.0f, 1.0f},
    {"release",       "Release",       PARAM_TYPE_FLOAT, LREL,          0.0f, 1.0f},
    {"vel_amp",       "Vel>Amp",       PARAM_TYPE_FLOAT, VAMPENV,       0.0f, 1.0f},

    /* LFO - continuous */
    {"lfo_rate",      "LFO Rate",      PARAM_TYPE_FLOAT, LFOFREQ,       0.0f, 1.0f},
    {"lfo_amt1",      "LFO Amt 1",     PARAM_TYPE_FLOAT, LFO1AMT,       0.0f, 1.0f},
    {"lfo_amt2",      "LFO Amt 2",     PARAM_TYPE_FLOAT, LFO2AMT,       0.0f, 1.0f},
    /* LFO - toggle */
    {"lfo_sin",       "LFO Sine",      PARAM_TYPE_INT,   LFOSINWAVE,    0.0f, 1.0f},  /* toggle */
    {"lfo_square",    "LFO Square",    PARAM_TYPE_INT,   LFOSQUAREWAVE, 0.0f, 1.0f},  /* toggle */
    {"lfo_sh",        "LFO S&H",       PARAM_TYPE_INT,   LFOSHWAVE,     0.0f, 1.0f},  /* toggle */
    {"lfo_sync",      "LFO Sync",      PARAM_TYPE_INT,   LFO_SYNC,      0.0f, 1.0f},  /* toggle */

    /* LFO Destinations - toggle */
    {"lfo_osc1",      "LFO>Osc1",      PARAM_TYPE_INT,   LFOOSC1,       0.0f, 1.0f},  /* toggle */
    {"lfo_osc2",      "LFO>Osc2",      PARAM_TYPE_INT,   LFOOSC2,       0.0f, 1.0f},  /* toggle */
    {"lfo_filter",    "LFO>Filter",    PARAM_TYPE_INT,   LFOFILTER,     0.0f, 1.0f},  /* toggle */
    {"lfo_pw1",       "LFO>PW1",       PARAM_TYPE_INT,   LFOPW1,        0.0f, 1.0f},  /* toggle */
    {"lfo_pw2",       "LFO>PW2",       PARAM_TYPE_INT,   LFOPW2,        0.0f, 1.0f},  /* toggle */

    /* Pitch Mod - continuous */
    {"env_pitch",     "Env>Pitch",     PARAM_TYPE_FLOAT, ENVPITCH,      0.0f, 1.0f},
    {"vibrato",       "Vibrato",       PARAM_TYPE_FLOAT, BENDLFORATE,   0.0f, 1.0f},
    /* Pitch Mod - toggle */
    {"env_pitch_both","Env Pitch Both",PARAM_TYPE_INT,   ENV_PITCH_BOTH,0.0f, 1.0f},  /* toggle */
    {"bend_range",    "Bend Range",    PARAM_TYPE_INT,   BENDRANGE,     0.0f, 1.0f},  /* 2 or 12 semitones */
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
    float params[PARAM_COUNT];  /* Engine param storage - indexed by ParamsEnum */
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

    /* Clear all params */
    memset(inst->params, 0, sizeof(inst->params));

    /* Global */
    synth->processVolume(1.0f);
    inst->params[VOLUME] = 1.0f;
    synth->setVoiceCount(MAX_VOICES / 8.0f);
    inst->params[VOICE_COUNT] = MAX_VOICES / 8.0f;

    /* Oscillators */
    synth->processOsc1Saw(1.0f);
    inst->params[OSC1Saw] = 1.0f;
    synth->processOsc1Pulse(0.0f);
    inst->params[OSC1Pul] = 0.0f;
    synth->processOsc2Saw(1.0f);
    inst->params[OSC2Saw] = 1.0f;
    synth->processOsc2Pulse(0.0f);
    inst->params[OSC2Pul] = 0.0f;
    synth->processOsc1Mix(0.5f);
    inst->params[OSC1MIX] = 0.5f;
    synth->processOsc2Mix(0.5f);
    inst->params[OSC2MIX] = 0.5f;
    synth->processOsc2Det(0.1f);
    inst->params[OSC2_DET] = 0.1f;

    /* Filter */
    synth->processCutoff(0.7f);
    inst->params[CUTOFF] = 0.7f;
    synth->processResonance(0.2f);
    inst->params[RESONANCE] = 0.2f;
    synth->processFourPole(1.0f);
    inst->params[FOURPOLE] = 1.0f;
    synth->processFilterEnvelopeAmt(0.3f);
    inst->params[ENVELOPE_AMT] = 0.3f;

    /* Amp Envelope */
    synth->processLoudnessEnvelopeAttack(0.01f);
    inst->params[LATK] = 0.01f;
    synth->processLoudnessEnvelopeDecay(0.3f);
    inst->params[LDEC] = 0.3f;
    synth->processLoudnessEnvelopeSustain(0.7f);
    inst->params[LSUS] = 0.7f;
    synth->processLoudnessEnvelopeRelease(0.2f);
    inst->params[LREL] = 0.2f;

    /* Filter Envelope */
    synth->processFilterEnvelopeAttack(0.01f);
    inst->params[FATK] = 0.01f;
    synth->processFilterEnvelopeDecay(0.3f);
    inst->params[FDEC] = 0.3f;
    synth->processFilterEnvelopeSustain(0.3f);
    inst->params[FSUS] = 0.3f;
    synth->processFilterEnvelopeRelease(0.2f);
    inst->params[FREL] = 0.2f;

    snprintf(inst->preset_name, sizeof(inst->preset_name), "Init");
}

/* v2 helper: Apply preset - FXB file params match ParamsEnum indices */
static void v2_apply_preset(obxd_instance_t *inst, int preset_idx) {
    if (preset_idx < 0 || preset_idx >= inst->preset_count) return;

    Preset *p = &inst->presets[preset_idx];
    SynthEngine *synth = inst->synth;
    snprintf(inst->preset_name, sizeof(inst->preset_name), "%s", p->name);

    /* Copy all preset params to instance params (indices match ParamsEnum) */
    for (int i = 0; i < p->param_count && i < PARAM_COUNT; i++) {
        inst->params[i] = p->params[i];
    }

    /* Apply all parameters to engine */
    if (p->param_count > VOLUME) synth->processVolume(p->params[VOLUME]);
    if (p->param_count > TUNE) synth->processTune(p->params[TUNE]);
    if (p->param_count > OCTAVE) synth->processOctave(p->params[OCTAVE]);
    if (p->param_count > VOICE_COUNT) synth->setVoiceCount(p->params[VOICE_COUNT]);
    if (p->param_count > LEGATOMODE) synth->processLegatoMode(p->params[LEGATOMODE]);
    if (p->param_count > PORTAMENTO) synth->processPortamento(p->params[PORTAMENTO]);
    if (p->param_count > UNISON) synth->processUnison(p->params[UNISON]);
    if (p->param_count > UDET) synth->processDetune(p->params[UDET]);
    if (p->param_count > OSC2_DET) synth->processOsc2Det(p->params[OSC2_DET]);

    /* LFO */
    if (p->param_count > LFOFREQ) synth->processLfoFrequency(p->params[LFOFREQ]);
    if (p->param_count > LFOSINWAVE) synth->processLfoSine(p->params[LFOSINWAVE]);
    if (p->param_count > LFOSQUAREWAVE) synth->processLfoSquare(p->params[LFOSQUAREWAVE]);
    if (p->param_count > LFOSHWAVE) synth->processLfoSH(p->params[LFOSHWAVE]);
    if (p->param_count > LFO1AMT) synth->processLfoAmt1(p->params[LFO1AMT]);
    if (p->param_count > LFO2AMT) synth->processLfoAmt2(p->params[LFO2AMT]);
    if (p->param_count > LFOOSC1) synth->processLfoOsc1(p->params[LFOOSC1]);
    if (p->param_count > LFOOSC2) synth->processLfoOsc2(p->params[LFOOSC2]);
    if (p->param_count > LFOFILTER) synth->processLfoFilter(p->params[LFOFILTER]);
    if (p->param_count > LFOPW1) synth->processLfoPw1(p->params[LFOPW1]);
    if (p->param_count > LFOPW2) synth->processLfoPw2(p->params[LFOPW2]);
    if (p->param_count > LFO_SYNC) synth->procLfoSync(p->params[LFO_SYNC]);

    /* Oscillators */
    if (p->param_count > OSC2HS) synth->processOsc2HardSync(p->params[OSC2HS]);
    if (p->param_count > XMOD) synth->processOsc2Xmod(p->params[XMOD]);
    if (p->param_count > OSC1P) synth->processOsc1Pitch(p->params[OSC1P]);
    if (p->param_count > OSC2P) synth->processOsc2Pitch(p->params[OSC2P]);
    if (p->param_count > OSCQuantize) synth->processPitchQuantization(p->params[OSCQuantize]);
    if (p->param_count > OSC1Saw) synth->processOsc1Saw(p->params[OSC1Saw]);
    if (p->param_count > OSC1Pul) synth->processOsc1Pulse(p->params[OSC1Pul]);
    if (p->param_count > OSC2Saw) synth->processOsc2Saw(p->params[OSC2Saw]);
    if (p->param_count > OSC2Pul) synth->processOsc2Pulse(p->params[OSC2Pul]);
    if (p->param_count > PW) synth->processPulseWidth(p->params[PW]);
    if (p->param_count > PW_ENV) synth->processPwEnv(p->params[PW_ENV]);
    if (p->param_count > PW_ENV_BOTH) synth->processPwEnvBoth(p->params[PW_ENV_BOTH]);
    if (p->param_count > PW_OSC2_OFS) synth->processPwOfs(p->params[PW_OSC2_OFS]);
    if (p->param_count > BRIGHTNESS) synth->processBrightness(p->params[BRIGHTNESS]);
    if (p->param_count > ENVPITCH) synth->processEnvelopeToPitch(p->params[ENVPITCH]);
    if (p->param_count > ENV_PITCH_BOTH) synth->processPitchModBoth(p->params[ENV_PITCH_BOTH]);
    if (p->param_count > OSC1MIX) synth->processOsc1Mix(p->params[OSC1MIX]);
    if (p->param_count > OSC2MIX) synth->processOsc2Mix(p->params[OSC2MIX]);
    if (p->param_count > NOISEMIX) synth->processNoiseMix(p->params[NOISEMIX]);

    /* Filter */
    if (p->param_count > FLT_KF) synth->processFilterKeyFollow(p->params[FLT_KF]);
    if (p->param_count > CUTOFF) synth->processCutoff(p->params[CUTOFF]);
    if (p->param_count > RESONANCE) synth->processResonance(p->params[RESONANCE]);
    if (p->param_count > MULTIMODE) synth->processMultimode(p->params[MULTIMODE]);
    if (p->param_count > BANDPASS) synth->processBandpassSw(p->params[BANDPASS]);
    if (p->param_count > FOURPOLE) synth->processFourPole(p->params[FOURPOLE]);
    if (p->param_count > SELF_OSC_PUSH) synth->processSelfOscPush(p->params[SELF_OSC_PUSH]);
    if (p->param_count > FENV_INVERT) synth->processInvertFenv(p->params[FENV_INVERT]);
    if (p->param_count > ENVELOPE_AMT) synth->processFilterEnvelopeAmt(p->params[ENVELOPE_AMT]);

    /* Amp Envelope */
    if (p->param_count > LATK) synth->processLoudnessEnvelopeAttack(p->params[LATK]);
    if (p->param_count > LDEC) synth->processLoudnessEnvelopeDecay(p->params[LDEC]);
    if (p->param_count > LSUS) synth->processLoudnessEnvelopeSustain(p->params[LSUS]);
    if (p->param_count > LREL) synth->processLoudnessEnvelopeRelease(p->params[LREL]);
    if (p->param_count > VAMPENV) synth->procAmpVelocityAmount(p->params[VAMPENV]);

    /* Filter Envelope */
    if (p->param_count > FATK) synth->processFilterEnvelopeAttack(p->params[FATK]);
    if (p->param_count > FDEC) synth->processFilterEnvelopeDecay(p->params[FDEC]);
    if (p->param_count > FSUS) synth->processFilterEnvelopeSustain(p->params[FSUS]);
    if (p->param_count > FREL) synth->processFilterEnvelopeRelease(p->params[FREL]);
    if (p->param_count > VFLTENV) synth->procFltVelocityAmount(p->params[VFLTENV]);

    /* Detune params */
    if (p->param_count > ENVDER) synth->processEnvelopeDetune(p->params[ENVDER]);
    if (p->param_count > FILTERDER) synth->processFilterDetune(p->params[FILTERDER]);
    if (p->param_count > PORTADER) synth->processPortamentoDetune(p->params[PORTADER]);

    /* Pitch bend */
    if (p->param_count > BENDRANGE) synth->procPitchWheelAmount(p->params[BENDRANGE]);
    if (p->param_count > BENDLFORATE) synth->procModWheelFrequency(p->params[BENDLFORATE]);
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

/* v2 helper: Apply param directly to engine using ParamsEnum index */
static void v2_apply_param_direct(obxd_instance_t *inst, int param_idx, float value) {
    if (param_idx < 0 || param_idx >= PARAM_COUNT) return;
    SynthEngine *synth = inst->synth;
    if (!synth) return;

    /* Store value for state serialization */
    inst->params[param_idx] = value;

    switch (param_idx) {
        /* Global */
        case VOLUME:        synth->processVolume(value); break;
        case TUNE:          synth->processTune(value); break;
        case OCTAVE:        synth->processOctave(value); break;
        case VOICE_COUNT:   synth->setVoiceCount(value); break;
        case LEGATOMODE:    synth->processLegatoMode(value); break;
        case PORTAMENTO:    synth->processPortamento(value); break;
        case UNISON:        synth->processUnison(value); break;
        case UDET:          synth->processDetune(value); break;

        /* Oscillator 1 */
        case OSC1Saw:       synth->processOsc1Saw(value); break;
        case OSC1Pul:       synth->processOsc1Pulse(value); break;
        case OSC1P:         synth->processOsc1Pitch(value); break;
        case OSC1MIX:       synth->processOsc1Mix(value); break;

        /* Oscillator 2 */
        case OSC2Saw:       synth->processOsc2Saw(value); break;
        case OSC2Pul:       synth->processOsc2Pulse(value); break;
        case OSC2P:         synth->processOsc2Pitch(value); break;
        case OSC2MIX:       synth->processOsc2Mix(value); break;
        case OSC2_DET:      synth->processOsc2Det(value); break;
        case OSC2HS:        synth->processOsc2HardSync(value); break;

        /* Oscillator Common */
        case PW:            synth->processPulseWidth(value); break;
        case PW_ENV:        synth->processPwEnv(value); break;
        case PW_ENV_BOTH:   synth->processPwEnvBoth(value); break;
        case PW_OSC2_OFS:   synth->processPwOfs(value); break;
        case NOISEMIX:      synth->processNoiseMix(value); break;
        case XMOD:          synth->processOsc2Xmod(value); break;
        case BRIGHTNESS:    synth->processBrightness(value); break;

        /* Filter */
        case CUTOFF:        synth->processCutoff(value); break;
        case RESONANCE:     synth->processResonance(value); break;
        case ENVELOPE_AMT:  synth->processFilterEnvelopeAmt(value); break;
        case FLT_KF:        synth->processFilterKeyFollow(value); break;
        case MULTIMODE:     synth->processMultimode(value); break;
        case BANDPASS:      synth->processBandpassSw(value); break;
        case FOURPOLE:      synth->processFourPole(value); break;
        case SELF_OSC_PUSH: synth->processSelfOscPush(value); break;
        case FENV_INVERT:   synth->processInvertFenv(value); break;

        /* Filter Envelope */
        case FATK:          synth->processFilterEnvelopeAttack(value); break;
        case FDEC:          synth->processFilterEnvelopeDecay(value); break;
        case FSUS:          synth->processFilterEnvelopeSustain(value); break;
        case FREL:          synth->processFilterEnvelopeRelease(value); break;
        case VFLTENV:       synth->procFltVelocityAmount(value); break;

        /* Amp Envelope */
        case LATK:          synth->processLoudnessEnvelopeAttack(value); break;
        case LDEC:          synth->processLoudnessEnvelopeDecay(value); break;
        case LSUS:          synth->processLoudnessEnvelopeSustain(value); break;
        case LREL:          synth->processLoudnessEnvelopeRelease(value); break;
        case VAMPENV:       synth->procAmpVelocityAmount(value); break;

        /* LFO */
        case LFOFREQ:       synth->processLfoFrequency(value); break;
        case LFOSINWAVE:    synth->processLfoSine(value); break;
        case LFOSQUAREWAVE: synth->processLfoSquare(value); break;
        case LFOSHWAVE:     synth->processLfoSH(value); break;
        case LFO_SYNC:      synth->procLfoSync(value); break;
        case LFO1AMT:       synth->processLfoAmt1(value); break;
        case LFO2AMT:       synth->processLfoAmt2(value); break;

        /* LFO Destinations */
        case LFOOSC1:       synth->processLfoOsc1(value); break;
        case LFOOSC2:       synth->processLfoOsc2(value); break;
        case LFOFILTER:     synth->processLfoFilter(value); break;
        case LFOPW1:        synth->processLfoPw1(value); break;
        case LFOPW2:        synth->processLfoPw2(value); break;

        /* Pitch Mod */
        case ENVPITCH:      synth->processEnvelopeToPitch(value); break;
        case ENV_PITCH_BOTH:synth->processPitchModBoth(value); break;
        case BENDRANGE:     synth->procPitchWheelAmount(value); break;
        case BENDLFORATE:   synth->procModWheelFrequency(value); break;

        default: break;
    }
}

/* v2 API: Set parameter */
/* Helper to extract a JSON number value by key */
static int json_get_number(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ' ') pos++;
    *out = (float)atof(pos);
    return 0;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    obxd_instance_t *inst = (obxd_instance_t*)instance;
    if (!inst) return;

    /* State restore from patch save */
    if (strcmp(key, "state") == 0) {
        float fval;

        /* Restore preset first */
        if (json_get_number(val, "preset", &fval) == 0) {
            int idx = (int)fval;
            if (idx >= 0 && idx < inst->preset_count) {
                inst->current_preset = idx;
                v2_apply_preset(inst, idx);
            }
        }

        /* Restore octave transpose */
        if (json_get_number(val, "octave_transpose", &fval) == 0) {
            inst->octave_transpose = (int)fval;
            if (inst->octave_transpose < -3) inst->octave_transpose = -3;
            if (inst->octave_transpose > 3) inst->octave_transpose = 3;
        }

        /* Restore all shadow params */
        for (int i = 0; i < (int)PARAM_DEF_COUNT(g_shadow_params); i++) {
            if (json_get_number(val, g_shadow_params[i].key, &fval) == 0) {
                if (fval < g_shadow_params[i].min_val) fval = g_shadow_params[i].min_val;
                if (fval > g_shadow_params[i].max_val) fval = g_shadow_params[i].max_val;
                v2_apply_param_direct(inst, g_shadow_params[i].index, fval);
            }
        }
        return;
    }

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
                v2_apply_param_direct(inst, g_shadow_params[i].index, fval);
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
    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "OB-Xd");
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
                    "\"children\":\"main\","
                    "\"knobs\":[\"cutoff\",\"resonance\",\"filter_env\",\"attack\",\"decay\",\"sustain\",\"release\",\"octave_transpose\"],"
                    "\"params\":[]"
                "},"
                "\"main\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"cutoff\",\"resonance\",\"filter_env\",\"attack\",\"decay\",\"sustain\",\"release\",\"octave_transpose\"],"
                    "\"params\":["
                        "{\"level\":\"global\",\"label\":\"Global\"},"
                        "{\"level\":\"osc1\",\"label\":\"Oscillator 1\"},"
                        "{\"level\":\"osc2\",\"label\":\"Oscillator 2\"},"
                        "{\"level\":\"osc_common\",\"label\":\"Osc Common\"},"
                        "{\"level\":\"filter\",\"label\":\"Filter\"},"
                        "{\"level\":\"filt_env\",\"label\":\"Filter Env\"},"
                        "{\"level\":\"amp_env\",\"label\":\"Amp Env\"},"
                        "{\"level\":\"lfo\",\"label\":\"LFO\"},"
                        "{\"level\":\"lfo_dest\",\"label\":\"LFO Dest\"},"
                        "{\"level\":\"pitch_mod\",\"label\":\"Pitch Mod\"}"
                    "]"
                "},"
                "\"global\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"volume\",\"tune\",\"octave\",\"portamento\",\"unison\",\"unison_det\",\"legato\",\"octave_transpose\"],"
                    "\"params\":[\"volume\",\"tune\",\"octave\",\"portamento\",\"unison\",\"unison_det\",\"legato\",\"octave_transpose\"]"
                "},"
                "\"osc1\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"osc1_saw\",\"osc1_pulse\",\"osc1_pitch\",\"osc1_mix\"],"
                    "\"params\":[\"osc1_saw\",\"osc1_pulse\",\"osc1_pitch\",\"osc1_mix\"]"
                "},"
                "\"osc2\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"osc2_saw\",\"osc2_pulse\",\"osc2_pitch\",\"osc2_mix\",\"osc2_detune\",\"osc2_sync\"],"
                    "\"params\":[\"osc2_saw\",\"osc2_pulse\",\"osc2_pitch\",\"osc2_mix\",\"osc2_detune\",\"osc2_sync\"]"
                "},"
                "\"osc_common\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"pw\",\"pw_env\",\"noise\",\"xmod\",\"brightness\"],"
                    "\"params\":[\"pw\",\"pw_env\",\"pw_env_both\",\"pw_ofs\",\"noise\",\"xmod\",\"brightness\"]"
                "},"
                "\"filter\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"cutoff\",\"resonance\",\"filter_env\",\"key_follow\",\"multimode\",\"fourpole\"],"
                    "\"params\":[\"cutoff\",\"resonance\",\"filter_env\",\"key_follow\",\"multimode\",\"bandpass\",\"fourpole\",\"self_osc\",\"fenv_inv\"]"
                "},"
                "\"filt_env\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"f_attack\",\"f_decay\",\"f_sustain\",\"f_release\",\"vel_filter\"],"
                    "\"params\":[\"f_attack\",\"f_decay\",\"f_sustain\",\"f_release\",\"vel_filter\"]"
                "},"
                "\"amp_env\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"attack\",\"decay\",\"sustain\",\"release\",\"vel_amp\"],"
                    "\"params\":[\"attack\",\"decay\",\"sustain\",\"release\",\"vel_amp\"]"
                "},"
                "\"lfo\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"lfo_rate\",\"lfo_sin\",\"lfo_square\",\"lfo_sh\",\"lfo_amt1\",\"lfo_amt2\"],"
                    "\"params\":[\"lfo_rate\",\"lfo_sin\",\"lfo_square\",\"lfo_sh\",\"lfo_sync\",\"lfo_amt1\",\"lfo_amt2\"]"
                "},"
                "\"lfo_dest\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"lfo_osc1\",\"lfo_osc2\",\"lfo_filter\",\"lfo_pw1\",\"lfo_pw2\"],"
                    "\"params\":[\"lfo_osc1\",\"lfo_osc2\",\"lfo_filter\",\"lfo_pw1\",\"lfo_pw2\"]"
                "},"
                "\"pitch_mod\":{"
                    "\"children\":null,"
                    "\"knobs\":[\"env_pitch\",\"bend_range\",\"vibrato\"],"
                    "\"params\":[\"env_pitch\",\"env_pitch_both\",\"bend_range\",\"vibrato\"]"
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

    /* State serialization for patch save/load */
    if (strcmp(key, "state") == 0) {
        int offset = 0;
        offset += snprintf(buf + offset, buf_len - offset,
            "{\"preset\":%d,\"octave_transpose\":%d",
            inst->current_preset, inst->octave_transpose);

        /* Add all shadow params */
        for (int i = 0; i < (int)PARAM_DEF_COUNT(g_shadow_params); i++) {
            float val = inst->params[g_shadow_params[i].index];
            offset += snprintf(buf + offset, buf_len - offset,
                ",\"%s\":%.4f", g_shadow_params[i].key, val);
        }

        offset += snprintf(buf + offset, buf_len - offset, "}");
        return offset;
    }

    /* Chain params metadata for shadow parameter editor - dynamically generated */
    if (strcmp(key, "chain_params") == 0) {
        /* Build JSON with preset/octave_transpose first, then all shadow params */
        int offset = 0;
        offset += snprintf(buf + offset, buf_len - offset,
            "[{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\",\"min\":0,\"max\":9999},"
            "{\"key\":\"octave_transpose\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-3,\"max\":3}");

        /* Add all shadow params */
        for (int i = 0; i < (int)PARAM_DEF_COUNT(g_shadow_params) && offset < buf_len - 100; i++) {
            offset += snprintf(buf + offset, buf_len - offset,
                ",{\"key\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"min\":%g,\"max\":%g}",
                g_shadow_params[i].key,
                g_shadow_params[i].name[0] ? g_shadow_params[i].name : g_shadow_params[i].key,
                g_shadow_params[i].type == PARAM_TYPE_INT ? "int" : "float",
                g_shadow_params[i].min_val,
                g_shadow_params[i].max_val);
        }
        offset += snprintf(buf + offset, buf_len - offset, "]");
        return offset;
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
