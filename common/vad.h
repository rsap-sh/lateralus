#pragma once
/*
 * Simple energy-based Voice Activity Detector
 *
 * Uses short-term energy with adaptive noise floor estimation.
 * No external dependencies — pure C, works on raw PCM samples.
 */

#include <stdint.h>
#include <math.h>
#include <string.h>

#define VAD_SAMPLE_RATE     48000
#define VAD_FRAME_SIZE      480       /* 10ms                              */
#define VAD_HANG_FRAMES     20        /* Keep VAD active N frames after cut */
#define VAD_NOISE_ALPHA     0.995f    /* Noise floor decay (slower = stable) */
#define VAD_SIGNAL_ALPHA    0.90f     /* Signal energy decay                */
#define VAD_THRESHOLD_DB    3.0f      /* dB above noise floor to trigger    */
#define VAD_PREEMPH         0.97f     /* Pre-emphasis coefficient           */

typedef struct {
    float    noise_floor;   /* Estimated background noise energy */
    float    signal_energy; /* Short-term signal energy           */
    float    preemph_prev;  /* Previous sample for pre-emphasis   */
    int      hang_counter;  /* Hangover counter                   */
    int      active;        /* Current VAD state                  */
} vad_state_t;

static inline void vad_init(vad_state_t *v) {
    memset(v, 0, sizeof(*v));
    v->noise_floor   = 1e-6f;  /* Small non-zero init */
    v->signal_energy = 1e-6f;
}

/*
 * Process one frame of int16 PCM.
 * Returns 1 if voice is detected, 0 if silence.
 */
static inline int vad_process(vad_state_t *v, const int16_t *pcm, int n) {
    /* Pre-emphasis + energy accumulation */
    float energy = 0.0f;
    float prev   = v->preemph_prev;

    for (int i = 0; i < n; i++) {
        float s   = (float)pcm[i] / 32768.0f;
        float emp = s - VAD_PREEMPH * prev;
        energy   += emp * emp;
        prev      = s;
    }
    v->preemph_prev = prev;

    energy /= (float)n;

    /* Update noise floor only during silence (or slowly always) */
    if (!v->active) {
        v->noise_floor = VAD_NOISE_ALPHA * v->noise_floor
                       + (1.0f - VAD_NOISE_ALPHA) * energy;
        if (v->noise_floor < 1e-10f) v->noise_floor = 1e-10f;
    }

    /* Smooth signal energy */
    v->signal_energy = VAD_SIGNAL_ALPHA * v->signal_energy
                     + (1.0f - VAD_SIGNAL_ALPHA) * energy;

    /* SNR in dB */
    float snr_db = 10.0f * log10f(v->signal_energy / v->noise_floor + 1e-10f);

    if (snr_db >= VAD_THRESHOLD_DB) {
        v->hang_counter = VAD_HANG_FRAMES;
        v->active       = 1;
    } else if (v->hang_counter > 0) {
        v->hang_counter--;
        v->active = 1;
    } else {
        v->active = 0;
        /* Update noise floor faster when clearly silent */
        v->noise_floor = (VAD_NOISE_ALPHA * 0.99f) * v->noise_floor
                       + (1.0f - VAD_NOISE_ALPHA * 0.99f) * energy;
    }

    return v->active;
}
