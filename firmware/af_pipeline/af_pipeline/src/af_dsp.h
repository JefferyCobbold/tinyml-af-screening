/*
 * AF screening signal processing, ported from the Python pipeline.
 *
 * Every step must match the Python code exactly (see CLAUDE.md):
 *   invert -> remove DC (mean of first 1 s) -> causal band-pass 0.5-8 Hz
 *   -> skip 2 s -> 30 s windows -> z-score -> beat detection
 *   -> quality gate (template SQI) -> rhythm features
 *
 * Plain C with no Zephyr dependencies, so it also compiles on a PC for
 * testing against the Python reference.
 */
#ifndef AF_DSP_H_
#define AF_DSP_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_FS            100     /* samples per second */
#define AF_DC_SAMPLES    100     /* 1 s used to estimate the DC level */
#define AF_SKIP_SAMPLES  200     /* 2 s skipped while the filter settles */
#define AF_WIN_SAMPLES   3000    /* 30 s analysis window */
#define AF_MAX_PEAKS     1600    /* upper bound on local maxima in one window */
#define AF_NUM_FEATURES  4
#define AF_SQI_THRESHOLD 0.80f

enum af_class {
	AF_CLASS_UNRELIABLE = 0,   /* quality gate rejected the window */
	AF_CLASS_NORMAL = 1,
	AF_CLASS_POSSIBLE_AF = 2,
};

/* Result for one 30 s window (the classification is filled in by the caller,
 * which runs the model). */
struct af_window_result {
	int n_beats;                        /* detected beats in the window */
	float sqi;                          /* template-matching quality, 0..1 */
	bool features_valid;                /* false if too few valid beats */
	float features[AF_NUM_FEATURES];    /* nrmssd, cv, pnn_rel, mad_rel */
	float heart_rate_bpm;
	bool passes_gate;                   /* features valid and sqi >= threshold */
};

/* Streaming pipeline state: feed raw MAX30102 IR counts one at a time. */
struct af_pipeline {
	int32_t dc_buf[AF_DC_SAMPLES];
	int dc_fill;
	bool dc_ready;
	float dc;                           /* DC level of the inverted signal */
	float z[2][2];                      /* filter state, 2 second-order sections */
	int skipped;
	int win_fill;
	float win[AF_WIN_SAMPLES];
};

/* Start a new recording (call when a finger has been placed and has settled). */
void af_pipeline_reset(struct af_pipeline *p);

/* Add one raw IR sample. Returns true when a 30 s window has just been
 * completed and analysed; the result is written to *out. */
bool af_pipeline_push(struct af_pipeline *p, int32_t raw_ir, struct af_window_result *out);

/* Building blocks, exposed for testing. */
int af_find_peaks(const float *x, int n, int *peaks);
float af_template_sqi(const float *w, int n, const int *peaks, int n_peaks);
bool af_rhythm_features(const int *peaks, int n_peaks, float features[AF_NUM_FEATURES],
			float *heart_rate_bpm);
void af_analyse_window(float *w, struct af_window_result *out);

#ifdef __cplusplus
}
#endif

#endif /* AF_DSP_H_ */
