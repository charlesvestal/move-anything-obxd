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

/* === Preset storage === */
#define MAX_PRESETS 128
#define MAX_PARAMS 100

struct Preset {
    char name[32];
    float params[MAX_PARAMS];
    int param_count;
};

static Preset g_presets[MAX_PRESETS];
static char g_module_dir[256] = "";

/* Forward declaration */
static void plugin_log(const char *msg);

/* Debug log to file */
static void debug_log(const char *msg) {
    FILE *f = fopen("/tmp/obxd-debug.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
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

/* Apply a loaded preset to the synth engine */
static void apply_preset(int preset_idx) {
    if (preset_idx < 0 || preset_idx >= g_preset_count) return;

    Preset *p = &g_presets[preset_idx];
    snprintf(g_preset_name, sizeof(g_preset_name), "%s", p->name);

    /* Apply all parameters from the preset */
    /* Val_X maps directly to ObxdParameters enum indices */
    if (p->param_count > 2) g_synth.processVolume(p->params[2]);           /* VOLUME = 2 */
    if (p->param_count > 4) g_synth.processTune(p->params[4]);             /* TUNE = 4 */
    if (p->param_count > 5) g_synth.processOctave(p->params[5]);           /* OCTAVE = 5 */
    if (p->param_count > 13) g_synth.processPortamento(p->params[13]);     /* PORTAMENTO = 13 */
    if (p->param_count > 14) g_synth.processUnison(p->params[14]);         /* UNISON = 14 */
    if (p->param_count > 15) g_synth.processDetune(p->params[15]);         /* UDET = 15 */
    if (p->param_count > 16) g_synth.processOsc2Det(p->params[16]);        /* OSC2_DET = 16 */
    if (p->param_count > 17) g_synth.processLfoFrequency(p->params[17]);   /* LFOFREQ = 17 */
    if (p->param_count > 18) g_synth.processLfoSine(p->params[18]);        /* LFOSINWAVE = 18 */
    if (p->param_count > 19) g_synth.processLfoSquare(p->params[19]);      /* LFOSQUAREWAVE = 19 */
    if (p->param_count > 20) g_synth.processLfoSH(p->params[20]);          /* LFOSHWAVE = 20 */
    if (p->param_count > 21) g_synth.processLfoAmt1(p->params[21]);        /* LFO1AMT = 21 */
    if (p->param_count > 22) g_synth.processLfoAmt2(p->params[22]);        /* LFO2AMT = 22 */
    if (p->param_count > 23) g_synth.processLfoOsc1(p->params[23]);        /* LFOOSC1 = 23 */
    if (p->param_count > 24) g_synth.processLfoOsc2(p->params[24]);        /* LFOOSC2 = 24 */
    if (p->param_count > 25) g_synth.processLfoFilter(p->params[25]);      /* LFOFILTER = 25 */
    if (p->param_count > 26) g_synth.processLfoPw1(p->params[26]);         /* LFOPW1 = 26 */
    if (p->param_count > 27) g_synth.processLfoPw2(p->params[27]);         /* LFOPW2 = 27 */
    if (p->param_count > 28) g_synth.processOsc2HardSync(p->params[28]);   /* OSC2HS = 28 */
    if (p->param_count > 29) g_synth.processOsc2Xmod(p->params[29]);       /* XMOD = 29 */
    if (p->param_count > 30) g_synth.processOsc1Pitch(p->params[30]);      /* OSC1P = 30 */
    if (p->param_count > 31) g_synth.processOsc2Pitch(p->params[31]);      /* OSC2P = 31 */
    if (p->param_count > 32) g_synth.processPitchQuantization(p->params[32]); /* OSCQuantize = 32 */
    if (p->param_count > 33) g_synth.processOsc1Saw(p->params[33]);        /* OSC1Saw = 33 */
    if (p->param_count > 34) g_synth.processOsc1Pulse(p->params[34]);      /* OSC1Pul = 34 */
    if (p->param_count > 35) g_synth.processOsc2Saw(p->params[35]);        /* OSC2Saw = 35 */
    if (p->param_count > 36) g_synth.processOsc2Pulse(p->params[36]);      /* OSC2Pul = 36 */
    if (p->param_count > 37) g_synth.processPulseWidth(p->params[37]);     /* PW = 37 */
    if (p->param_count > 38) g_synth.processBrightness(p->params[38]);     /* BRIGHTNESS = 38 */
    if (p->param_count > 39) g_synth.processEnvelopeToPitch(p->params[39]); /* ENVPITCH = 39 */
    if (p->param_count > 40) g_synth.processOsc1Mix(p->params[40]);        /* OSC1MIX = 40 */
    if (p->param_count > 41) g_synth.processOsc2Mix(p->params[41]);        /* OSC2MIX = 41 */
    if (p->param_count > 42) g_synth.processNoiseMix(p->params[42]);       /* NOISEMIX = 42 */
    if (p->param_count > 43) g_synth.processFilterKeyFollow(p->params[43]); /* FLT_KF = 43 */
    if (p->param_count > 44) g_synth.processCutoff(p->params[44]);         /* CUTOFF = 44 */
    if (p->param_count > 45) g_synth.processResonance(p->params[45]);      /* RESONANCE = 45 */
    if (p->param_count > 46) g_synth.processMultimode(p->params[46]);      /* MULTIMODE = 46 */
    if (p->param_count > 48) g_synth.processBandpassSw(p->params[48]);     /* BANDPASS = 48 */
    if (p->param_count > 49) g_synth.processFourPole(p->params[49]);       /* FOURPOLE = 49 */
    if (p->param_count > 50) g_synth.processFilterEnvelopeAmt(p->params[50]); /* ENVELOPE_AMT = 50 */
    if (p->param_count > 51) g_synth.processLoudnessEnvelopeAttack(p->params[51]);  /* LATK = 51 */
    if (p->param_count > 52) g_synth.processLoudnessEnvelopeDecay(p->params[52]);   /* LDEC = 52 */
    if (p->param_count > 53) g_synth.processLoudnessEnvelopeSustain(p->params[53]); /* LSUS = 53 */
    if (p->param_count > 54) g_synth.processLoudnessEnvelopeRelease(p->params[54]); /* LREL = 54 */
    if (p->param_count > 55) g_synth.processFilterEnvelopeAttack(p->params[55]);    /* FATK = 55 */
    if (p->param_count > 56) g_synth.processFilterEnvelopeDecay(p->params[56]);     /* FDEC = 56 */
    if (p->param_count > 57) g_synth.processFilterEnvelopeSustain(p->params[57]);   /* FSUS = 57 */
    if (p->param_count > 58) g_synth.processFilterEnvelopeRelease(p->params[58]);   /* FREL = 58 */
    if (p->param_count > 59) g_synth.processEnvelopeDetune(p->params[59]);          /* ENVDER = 59 */
    if (p->param_count > 60) g_synth.processFilterDetune(p->params[60]);            /* FILTERDER = 60 */
    if (p->param_count > 61) g_synth.processPortamentoDetune(p->params[61]);        /* PORTADER = 61 */

    /* Update local param cache for display */
    if (p->param_count > 44) g_params[0] = p->params[44];  /* cutoff */
    if (p->param_count > 45) g_params[1] = p->params[45];  /* resonance */
    if (p->param_count > 50) g_params[2] = p->params[50];  /* filter_env */
    if (p->param_count > 43) g_params[3] = p->params[43];  /* key_track */
    if (p->param_count > 51) g_params[4] = p->params[51];  /* attack */
    if (p->param_count > 52) g_params[5] = p->params[52];  /* decay */
    if (p->param_count > 53) g_params[6] = p->params[53];  /* sustain */
    if (p->param_count > 54) g_params[7] = p->params[54];  /* release */
}

/* Parse FXB bank file and load presets */
static int load_bank(const char *bank_path) {
    char dbg[512];
    snprintf(dbg, sizeof(dbg), "load_bank: trying to open %s", bank_path);
    debug_log(dbg);

    FILE *f = fopen(bank_path, "rb");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open bank: %s", bank_path);
        plugin_log(msg);
        debug_log(msg);
        return -1;
    }
    debug_log("load_bank: file opened successfully");

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char dbg2[128];
    snprintf(dbg2, sizeof(dbg2), "load_bank: file size = %ld", size);
    debug_log(dbg2);

    /* Read entire file */
    char *data = (char*)malloc(size + 1);
    if (!data) {
        fclose(f);
        debug_log("load_bank: malloc failed");
        return -1;
    }
    size_t read_bytes = fread(data, 1, size, f);
    data[size] = '\0';
    fclose(f);

    snprintf(dbg2, sizeof(dbg2), "load_bank: read %zu bytes", read_bytes);
    debug_log(dbg2);

    /* Find XML start - scan manually because binary header may contain NULLs */
    char *xml = NULL;
    for (long i = 0; i < size - 5; i++) {
        if (data[i] == '<' && data[i+1] == '?' && data[i+2] == 'x' &&
            data[i+3] == 'm' && data[i+4] == 'l') {
            xml = &data[i];
            break;
        }
    }
    if (!xml) {
        free(data);
        debug_log("load_bank: No XML found in bank file");
        plugin_log("No XML found in bank file");
        return -1;
    }
    debug_log("load_bank: found XML start");

    /* Parse each <program> element */
    g_preset_count = 0;
    char *program = xml;
    char buf[256];

    debug_log("load_bank: starting program parse loop");

    while ((program = strstr(program, "<program ")) != NULL && g_preset_count < MAX_PRESETS) {
        if (g_preset_count == 0) {
            debug_log("load_bank: found first <program>");
        }
        Preset *p = &g_presets[g_preset_count];
        memset(p, 0, sizeof(Preset));

        /* Get program name */
        if (find_attr(program, "programName", buf, sizeof(buf))) {
            strncpy(p->name, buf, sizeof(p->name) - 1);
        } else {
            snprintf(p->name, sizeof(p->name), "Preset %d", g_preset_count);
        }

        /* Parse Val_X parameters */
        for (int i = 0; i < MAX_PARAMS; i++) {
            char attr_name[16];
            snprintf(attr_name, sizeof(attr_name), "Val_%d", i);
            if (find_attr(program, attr_name, buf, sizeof(buf))) {
                p->params[i] = parse_attr_float(buf);
                p->param_count = i + 1;
            }
        }

        g_preset_count++;
        program++; /* Move past this <program to find next */
    }

    free(data);

    char msg[128];
    snprintf(msg, sizeof(msg), "Loaded %d presets from bank", g_preset_count);
    plugin_log(msg);
    debug_log(msg);

    return g_preset_count;
}

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

/* Output gain */
static float g_output_gain = 1.0f;

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

    /* Store module directory */
    strncpy(g_module_dir, module_dir, sizeof(g_module_dir) - 1);

    /* Initialize synth engine */
    g_synth.setSampleRate((float)MOVE_SAMPLE_RATE);

    /* Set default tempo */
    g_synth.setPlayHead(g_tempo_bpm, 0.0f);

    /* Initialize with default patch */
    init_default_patch();

    /* Try to load preset bank */
    char bank_path[512];
    snprintf(bank_path, sizeof(bank_path), "%s/presets/factory.fxb", module_dir);
    if (load_bank(bank_path) > 0) {
        /* Load first preset */
        g_current_preset = 0;
        apply_preset(0);
    }

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
        /* Send all notes off before changing octave to prevent stuck notes */
        g_synth.allNotesOff();
        g_octave_transpose = (int)fval;
        if (g_octave_transpose < -4) g_octave_transpose = -4;
        if (g_octave_transpose > 4) g_octave_transpose = 4;
    } else if (strcmp(key, "tempo") == 0) {
        g_tempo_bpm = fval;
        g_synth.setPlayHead(g_tempo_bpm, 0.0f);
    } else if (strcmp(key, "preset") == 0) {
        int new_preset = (int)fval;
        if (new_preset >= 0 && new_preset < g_preset_count) {
            /* Send all notes off before changing preset to prevent artifacts */
            g_synth.allNotesOff();
            g_current_preset = new_preset;
            apply_preset(g_current_preset);
        }
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
