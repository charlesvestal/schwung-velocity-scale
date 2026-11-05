/*
 * Velocity Scale MIDI FX
 *
 * Shapes incoming note velocities in three stages:
 *   1. Input Rescale - maps a custom physical input range (input_min..input_max) to 0..1.
 *   2. MPC Curve     - a tunable power curve applied to the normalized input.
 *   3. Min/Max       - linear scale of the curve output into a configurable output range.
 * Velocity 0 (note-off) is always passed through unchanged.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

/* JSON helpers for state parsing */
static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

/* Monitor readouts; 0 = no note seen since load/reset. */
typedef struct { int last, lo, hi; } vel_stats_t;

typedef struct {
    int has_notes;
    vel_stats_t mon_in;
    vel_stats_t mon_out;
    int input_min; /* 1-127 */
    int input_max; /* 1-127 */
    int vel_min;   /* 1-127 */
    int vel_max;   /* 1-127 */
    int curve;     /* -100..+100, 0 = linear (no-op) */
} velocity_scale_instance_t;

static const host_api_v1_t *g_host = NULL;

/* Named Curve presets (just convenience setters for the continuous Curve). */
typedef struct { const char *name; int value; } curve_preset_t;
static const curve_preset_t CURVE_PRESETS[] = {
    { "Linear",  0 },
    { "Soft",   40 },
    { "Softer", 70 },
    { "Hard",  -40 },
    { "Harder",-70 },
};
#define NUM_CURVE_PRESETS ((int)(sizeof(CURVE_PRESETS) / sizeof(CURVE_PRESETS[0])))

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void stats_add(vel_stats_t *s, int v, int first) {
    s->last = v;
    if (first || v < s->lo) s->lo = v;
    if (first || v > s->hi) s->hi = v;
}

/* The grid may send a trigger as the option index ("1") or the option text. */
static int trigger_fired(const char *val, const char *fire_text) {
    if (!val[0]) return 0;
    return strcmp(val, "1") == 0 || strcmp(val, fire_text) == 0;
}

/* access:"read" makes the monitor values dotted readouts; access:"write" makes
 * Reset a push button rather than a latching enum. */
static const char CHAIN_PARAMS_JSON[] =
    "["
    "{\"key\":\"input_min\",\"name\":\"Input Min\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
    "{\"key\":\"input_max\",\"name\":\"Input Max\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
    "{\"key\":\"min\",\"name\":\"Min Velocity\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
    "{\"key\":\"max\",\"name\":\"Max Velocity\",\"type\":\"int\",\"min\":1,\"max\":127,\"step\":1},"
    "{\"key\":\"curve\",\"name\":\"Curve\",\"type\":\"int\",\"min\":-100,\"max\":100,\"step\":1,\"default\":0},"
    "{\"key\":\"curve_preset\",\"name\":\"Curve Preset\",\"type\":\"enum\",\"options\":[\"Linear\",\"Soft\",\"Softer\",\"Hard\",\"Harder\"]},"
    "{\"key\":\"mon_in_last\",\"name\":\"Input Last\",\"short_name\":\"In\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1,\"access\":\"read\"},"
    "{\"key\":\"mon_in_lo\",\"name\":\"Input Lowest\",\"short_name\":\"In Lo\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1,\"access\":\"read\"},"
    "{\"key\":\"mon_in_hi\",\"name\":\"Input Highest\",\"short_name\":\"In Hi\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1,\"access\":\"read\"},"
    "{\"key\":\"mon_out_last\",\"name\":\"Output Last\",\"short_name\":\"Out\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1,\"access\":\"read\"},"
    "{\"key\":\"mon_out_lo\",\"name\":\"Output Lowest\",\"short_name\":\"OutLo\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1,\"access\":\"read\"},"
    "{\"key\":\"mon_out_hi\",\"name\":\"Output Highest\",\"short_name\":\"OutHi\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1,\"access\":\"read\"},"
    "{\"key\":\"mon_reset\",\"name\":\"Reset Monitor\",\"short_name\":\"Reset\",\"type\":\"enum\",\"options\":[\"-\",\"Reset!\"],\"access\":\"write\"}"
    "]";

static void* velocity_scale_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;

    velocity_scale_instance_t *inst = calloc(1, sizeof(velocity_scale_instance_t));
    if (!inst) return NULL;

    inst->input_min = 1;
    inst->input_max = 127;
    inst->vel_min = 1;
    inst->vel_max = 127;
    inst->curve = 0;
    return inst;
}

static void velocity_scale_destroy_instance(void *instance) {
    if (instance) free(instance);
}

static int velocity_scale_process_midi(void *instance,
                                       const uint8_t *in_msg, int in_len,
                                       uint8_t out_msgs[][3], int out_lens[],
                                       int max_out) {
    velocity_scale_instance_t *inst = (velocity_scale_instance_t *)instance;
    if (!inst || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0] & 0xF0;

    /* Only scale velocity on note-on messages with velocity > 0 */
    if (status == 0x90 && in_len >= 3 && in_msg[2] > 0) {
        int vel = in_msg[2];  /* 1-127 */

        int in_lo = inst->input_min;
        int in_hi = inst->input_max;
        if (in_lo > in_hi) { int tmp = in_lo; in_lo = in_hi; in_hi = tmp; }

        int lo = inst->vel_min;
        int hi = inst->vel_max;
        if (lo > hi) { int tmp = lo; lo = hi; hi = tmp; }

        /* Stage 1: Input range rescaling to normalized [0.0, 1.0] */
        double n;
        if (in_hi == in_lo) {
            n = vel >= in_hi ? 1.0 : 0.0;  /* zero-width range acts as a threshold */
        } else {
            n = (double)(clampi(vel, in_lo, in_hi) - in_lo) / (double)(in_hi - in_lo);
        }

        /* Stage 2: MPC-style power curve, gamma = 2^(-curve/50).
         * +100 -> 0.25 (soft hits louder), -100 -> 4.0 (needs harder hits).
         * Endpoints 0 and 1 are fixed for any curve. */
        double shaped = n;
        if (inst->curve != 0) {
            shaped = pow(n, pow(2.0, -inst->curve / 100.0 * 2.0));
        }

        /* Stage 3: Output min/max range mapping, rounded once (half-up) */
        int scaled = (int)floor(lo + shaped * (hi - lo) + 0.5);

        stats_add(&inst->mon_in, vel, !inst->has_notes);
        stats_add(&inst->mon_out, scaled, !inst->has_notes);
        inst->has_notes = 1;

        out_msgs[0][0] = in_msg[0];
        out_msgs[0][1] = in_msg[1];
        out_msgs[0][2] = (uint8_t)scaled;
        out_lens[0] = 3;
        return 1;
    }

    /* Pass through all other messages unchanged */
    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int velocity_scale_tick(void *instance,
                               int frames, int sample_rate,
                               uint8_t out_msgs[][3], int out_lens[],
                               int max_out) {
    (void)instance;
    (void)frames;
    (void)sample_rate;
    (void)out_msgs;
    (void)out_lens;
    (void)max_out;
    return 0;  /* No time-based output */
}

static void velocity_scale_set_param(void *instance, const char *key, const char *val) {
    velocity_scale_instance_t *inst = (velocity_scale_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "input_min") == 0) {
        inst->input_min = clampi(atoi(val), 1, 127);
    }
    else if (strcmp(key, "input_max") == 0) {
        inst->input_max = clampi(atoi(val), 1, 127);
    }
    else if (strcmp(key, "min") == 0) {
        inst->vel_min = clampi(atoi(val), 1, 127);
    }
    else if (strcmp(key, "max") == 0) {
        inst->vel_max = clampi(atoi(val), 1, 127);
    }
    else if (strcmp(key, "curve") == 0) {
        inst->curve = clampi(atoi(val), -100, 100);
    }
    else if (strcmp(key, "mon_reset") == 0) {
        if (trigger_fired(val, "Reset!")) {
            inst->has_notes = 0;
            memset(&inst->mon_in, 0, sizeof(inst->mon_in));
            memset(&inst->mon_out, 0, sizeof(inst->mon_out));
        }
    }
    else if (strcmp(key, "curve_preset") == 0) {
        for (int i = 0; i < NUM_CURVE_PRESETS; i++) {
            if (strcmp(val, CURVE_PRESETS[i].name) == 0) {
                inst->curve = CURVE_PRESETS[i].value;
                break;
            }
        }
    }
    else if (strcmp(key, "state") == 0) {
        int v;
        /* States saved before input range existed lack these keys: reset to full range. */
        inst->input_min = json_get_int(val, "input_min", &v) ? clampi(v, 1, 127) : 1;
        inst->input_max = json_get_int(val, "input_max", &v) ? clampi(v, 1, 127) : 127;
        if (json_get_int(val, "min", &v)) inst->vel_min = clampi(v, 1, 127);
        if (json_get_int(val, "max", &v)) inst->vel_max = clampi(v, 1, 127);
        if (json_get_int(val, "curve", &v)) inst->curve = clampi(v, -100, 100);
    }
}

static int velocity_scale_get_param(void *instance, const char *key, char *buf, int buf_len) {
    velocity_scale_instance_t *inst = (velocity_scale_instance_t *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "input_min") == 0) {
        return snprintf(buf, buf_len, "%d", inst->input_min);
    }
    else if (strcmp(key, "input_max") == 0) {
        return snprintf(buf, buf_len, "%d", inst->input_max);
    }
    else if (strcmp(key, "min") == 0) {
        return snprintf(buf, buf_len, "%d", inst->vel_min);
    }
    else if (strcmp(key, "max") == 0) {
        return snprintf(buf, buf_len, "%d", inst->vel_max);
    }
    else if (strcmp(key, "curve") == 0) {
        return snprintf(buf, buf_len, "%d", inst->curve);
    }
    else if (strcmp(key, "mon_in_last") == 0)  return snprintf(buf, buf_len, "%d", inst->mon_in.last);
    else if (strcmp(key, "mon_in_lo") == 0)    return snprintf(buf, buf_len, "%d", inst->mon_in.lo);
    else if (strcmp(key, "mon_in_hi") == 0)    return snprintf(buf, buf_len, "%d", inst->mon_in.hi);
    else if (strcmp(key, "mon_out_last") == 0) return snprintf(buf, buf_len, "%d", inst->mon_out.last);
    else if (strcmp(key, "mon_out_lo") == 0)   return snprintf(buf, buf_len, "%d", inst->mon_out.lo);
    else if (strcmp(key, "mon_out_hi") == 0)   return snprintf(buf, buf_len, "%d", inst->mon_out.hi);
    else if (strcmp(key, "mon_reset") == 0)    return snprintf(buf, buf_len, "0");
    else if (strcmp(key, "curve_preset") == 0) {
        const char *name = CURVE_PRESETS[0].name;
        for (int i = 0; i < NUM_CURVE_PRESETS; i++) {
            if (CURVE_PRESETS[i].value == inst->curve) {
                name = CURVE_PRESETS[i].name;
                break;
            }
        }
        return snprintf(buf, buf_len, "%s", name);
    }
    else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len, "{\"input_min\":%d,\"input_max\":%d,\"min\":%d,\"max\":%d,\"curve\":%d}",
                        inst->input_min, inst->input_max, inst->vel_min, inst->vel_max, inst->curve);
    }
    else if (strcmp(key, "chain_params") == 0) {
        int n = (int)sizeof(CHAIN_PARAMS_JSON) - 1;
        if (n >= buf_len) return -1;  /* truncated JSON is worse than the module.json fallback */
        memcpy(buf, CHAIN_PARAMS_JSON, (size_t)n + 1);
        return n;
    }

    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = velocity_scale_create_instance,
    .destroy_instance = velocity_scale_destroy_instance,
    .process_midi = velocity_scale_process_midi,
    .tick = velocity_scale_tick,
    .set_param = velocity_scale_set_param,
    .get_param = velocity_scale_get_param
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}