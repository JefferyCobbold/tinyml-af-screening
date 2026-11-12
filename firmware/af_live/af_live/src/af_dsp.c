#include "af_dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Band-pass filter: scipy.signal.butter(2, (0.5, 8.0), btype="bandpass",
 * fs=100, output="sos"). Two second-order sections: b0 b1 b2 a1 a2 (a0 = 1).
 */
static const float sos[2][5] = {
	{ 4.1253537242e-02f, 8.2507074483e-02f, 4.1253537242e-02f,
	  -1.3658733440e+00f, 5.3692604607e-01f },
	{ 1.0000000000e+00f, -2.0000000000e+00f, 1.0000000000e+00f,
	  -1.9561871344e+00f, 9.5726757526e-01f },
};

#define PEAK_DISTANCE   33      /* 0.33 s at 100 Hz */
#define PEAK_PROMINENCE 0.5f
#define MIN_BEATS       15
#define IBI_MIN_MS      300.0f
#define IBI_MAX_MS      2000.0f
#define PNN_FRACTION    0.065f

/* Scratch buffers (static: too large for a thread stack) */
static float norm_win[AF_WIN_SAMPLES];
static float template_buf[AF_WIN_SAMPLES];
static int peak_buf[AF_MAX_PEAKS];
static int order_buf[AF_MAX_PEAKS];
static uint8_t keep_buf[AF_MAX_PEAKS];
static float ibi_buf[AF_MAX_PEAKS];
static float diff_buf[AF_MAX_PEAKS];
static int int_buf[AF_MAX_PEAKS];

/* ------------------------------------------------------------------ */
/* Filter: transposed direct form II, the same structure as sosfilt   */

static float filter_step(float z[2][2], float x)
{
	for (int s = 0; s < 2; s++) {
		const float *c = sos[s];
		float y = c[0] * x + z[s][0];

		z[s][0] = c[1] * x - c[3] * y + z[s][1];
		z[s][1] = c[2] * x - c[4] * y;
		x = y;
	}
	return x;
}

/* ------------------------------------------------------------------ */
/* Beat detection: scipy.signal.find_peaks(x, distance=33, prominence=0.5) */

static const float *sort_values;

static int cmp_by_value(const void *a, const void *b)
{
	float va = sort_values[*(const int *)a];
	float vb = sort_values[*(const int *)b];

	if (va < vb) {
		return -1;
	}
	if (va > vb) {
		return 1;
	}
	return (*(const int *)a) - (*(const int *)b);
}

int af_find_peaks(const float *x, int n, int *peaks)
{
	int m = 0;

	/* 1) Local maxima, including the midpoint of flat-topped peaks
	 *    (scipy _local_maxima_1d). */
	for (int i = 1; i < n - 1; i++) {
		if (x[i - 1] < x[i]) {
			int ahead = i + 1;

			while (ahead < n - 1 && x[ahead] == x[i]) {
				ahead++;
			}
			if (x[ahead] < x[i]) {
				if (m < AF_MAX_PEAKS) {
					peak_buf[m++] = (i + ahead - 1) / 2;
				}
				i = ahead;
			}
		}
	}

	/* 2) Minimum distance: keep the tallest peaks first and remove any
	 *    neighbour closer than PEAK_DISTANCE (scipy _select_by_peak_distance). */
	static float heights[AF_MAX_PEAKS];

	for (int i = 0; i < m; i++) {
		order_buf[i] = i;
		keep_buf[i] = 1;
		heights[i] = x[peak_buf[i]];
	}
	sort_values = heights;
	qsort(order_buf, m, sizeof(int), cmp_by_value);

	for (int i = m - 1; i >= 0; i--) {
		int j = order_buf[i];

		if (!keep_buf[j]) {
			continue;
		}
		for (int k = j - 1; k >= 0 && peak_buf[j] - peak_buf[k] < PEAK_DISTANCE; k--) {
			keep_buf[k] = 0;
		}
		for (int k = j + 1; k < m && peak_buf[k] - peak_buf[j] < PEAK_DISTANCE; k++) {
			keep_buf[k] = 0;
		}
	}

	/* 3) Prominence: height above the higher of the two lowest points
	 *    reached before a taller sample on each side (scipy _peak_prominences). */
	int out = 0;

	for (int i = 0; i < m; i++) {
		if (!keep_buf[i]) {
			continue;
		}
		int p = peak_buf[i];
		float left_min = x[p];
		float right_min = x[p];

		for (int k = p; k >= 0 && x[k] <= x[p]; k--) {
			if (x[k] < left_min) {
				left_min = x[k];
			}
		}
		for (int k = p; k < n && x[k] <= x[p]; k++) {
			if (x[k] < right_min) {
				right_min = x[k];
			}
		}
		float prominence = x[p] - (left_min > right_min ? left_min : right_min);

		if (prominence >= PEAK_PROMINENCE) {
			peaks[out++] = p;
		}
	}
	return out;
}

/* ------------------------------------------------------------------ */
/* Medians (numpy semantics: mean of the two middle values when even)  */

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int cmp_float(const void *a, const void *b)
{
	float fa = *(const float *)a;
	float fb = *(const float *)b;

	return (fa > fb) - (fa < fb);
}

static float median_float(float *v, int n)
{
	qsort(v, n, sizeof(float), cmp_float);
	return (n % 2) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* ------------------------------------------------------------------ */
/* Quality gate: template-matching SQI                                 */

float af_template_sqi(const float *w, int n, const int *peaks, int n_peaks)
{
	if (n_peaks < 5) {
		return 0.0f;
	}

	/* half = int(0.5 * median(diff(peaks))), computed exactly in integers */
	int nd = n_peaks - 1;

	for (int i = 0; i < nd; i++) {
		int_buf[i] = peaks[i + 1] - peaks[i];
	}
	qsort(int_buf, nd, sizeof(int), cmp_int);
	int half = (nd % 2) ? int_buf[nd / 2] / 2 : (int_buf[nd / 2 - 1] + int_buf[nd / 2]) / 4;
	int len = 2 * half;

	/* Template = average of all complete beats, centred on each peak */
	int n_beats = 0;

	memset(template_buf, 0, sizeof(float) * len);
	for (int i = 0; i < n_peaks; i++) {
		int s = peaks[i] - half;

		if (s < 0 || peaks[i] + half > n) {
			continue;
		}
		for (int j = 0; j < len; j++) {
			template_buf[j] += w[s + j];
		}
		n_beats++;
	}
	if (n_beats < 5 || len == 0) {
		return 0.0f;
	}

	float t_mean = 0.0f;

	for (int j = 0; j < len; j++) {
		template_buf[j] /= (float)n_beats;
		t_mean += template_buf[j];
	}
	t_mean /= (float)len;

	float t_ss = 0.0f;

	for (int j = 0; j < len; j++) {
		float d = template_buf[j] - t_mean;

		t_ss += d * d;
	}

	/* Mean Pearson correlation of each beat with the template */
	float r_sum = 0.0f;

	for (int i = 0; i < n_peaks; i++) {
		int s = peaks[i] - half;

		if (s < 0 || peaks[i] + half > n) {
			continue;
		}
		float b_mean = 0.0f;

		for (int j = 0; j < len; j++) {
			b_mean += w[s + j];
		}
		b_mean /= (float)len;

		float cross = 0.0f;
		float b_ss = 0.0f;

		for (int j = 0; j < len; j++) {
			float db = w[s + j] - b_mean;

			cross += db * (template_buf[j] - t_mean);
			b_ss += db * db;
		}
		r_sum += cross / sqrtf(b_ss * t_ss);
	}
	return r_sum / (float)n_beats;
}

/* ------------------------------------------------------------------ */
/* Rhythm features from beat-to-beat intervals                         */

bool af_rhythm_features(const int *peaks, int n_peaks, float features[AF_NUM_FEATURES],
			float *heart_rate_bpm)
{
	if (n_peaks < MIN_BEATS) {
		return false;
	}

	int n_ibi = 0;

	for (int i = 0; i < n_peaks - 1; i++) {
		float ibi = (float)(peaks[i + 1] - peaks[i]) * (1000.0f / AF_FS);

		if (ibi > IBI_MIN_MS && ibi < IBI_MAX_MS) {
			ibi_buf[n_ibi++] = ibi;
		}
	}
	if (n_ibi < MIN_BEATS - 1) {
		return false;
	}

	float mean = 0.0f;

	for (int i = 0; i < n_ibi; i++) {
		mean += ibi_buf[i];
	}
	mean /= (float)n_ibi;

	float var = 0.0f;

	for (int i = 0; i < n_ibi; i++) {
		float d = ibi_buf[i] - mean;

		var += d * d;
	}
	var /= (float)n_ibi;

	int nd = n_ibi - 1;
	float sq_sum = 0.0f;
	int above = 0;

	for (int i = 0; i < nd; i++) {
		diff_buf[i] = fabsf(ibi_buf[i + 1] - ibi_buf[i]);
		sq_sum += diff_buf[i] * diff_buf[i];
		if (diff_buf[i] > PNN_FRACTION * mean) {
			above++;
		}
	}

	features[0] = sqrtf(sq_sum / (float)nd) / mean;      /* nrmssd */
	features[1] = sqrtf(var) / mean;                      /* cv */
	features[2] = (float)above / (float)nd;               /* pnn_rel */
	features[3] = median_float(diff_buf, nd) / mean;      /* mad_rel */
	*heart_rate_bpm = 60000.0f / mean;
	return true;
}

/* ------------------------------------------------------------------ */
/* One 30 s window: normalise, detect beats, quality gate, features    */

void af_analyse_window(float *w, struct af_window_result *out)
{
	float mean = 0.0f;

	for (int i = 0; i < AF_WIN_SAMPLES; i++) {
		mean += w[i];
	}
	mean /= (float)AF_WIN_SAMPLES;

	float var = 0.0f;

	for (int i = 0; i < AF_WIN_SAMPLES; i++) {
		float d = w[i] - mean;

		var += d * d;
	}
	float sd = sqrtf(var / (float)AF_WIN_SAMPLES);

	memset(out, 0, sizeof(*out));
	if (sd <= 1e-6f) {
		return;   /* flat signal: nothing to analyse */
	}
	for (int i = 0; i < AF_WIN_SAMPLES; i++) {
		w[i] = (w[i] - mean) / sd;
	}

	static int peaks[AF_MAX_PEAKS];

	out->n_beats = af_find_peaks(w, AF_WIN_SAMPLES, peaks);
	out->sqi = af_template_sqi(w, AF_WIN_SAMPLES, peaks, out->n_beats);
	out->features_valid = af_rhythm_features(peaks, out->n_beats, out->features,
						 &out->heart_rate_bpm);
	out->passes_gate = out->features_valid && out->sqi >= AF_SQI_THRESHOLD;
}

/* ------------------------------------------------------------------ */
/* Streaming pipeline                                                  */

void af_pipeline_reset(struct af_pipeline *p)
{
	memset(p, 0, sizeof(*p));
}

static bool feed_filtered(struct af_pipeline *p, float x, struct af_window_result *out)
{
	float y = filter_step(p->z, x - p->dc);

	if (p->skipped < AF_SKIP_SAMPLES) {
		p->skipped++;
		return false;
	}
	p->win[p->win_fill++] = y;
	if (p->win_fill < AF_WIN_SAMPLES) {
		return false;
	}
	p->win_fill = 0;
	memcpy(norm_win, p->win, sizeof(norm_win));
	af_analyse_window(norm_win, out);
	return true;
}

bool af_pipeline_push(struct af_pipeline *p, int32_t raw_ir, struct af_window_result *out)
{
	if (!p->dc_ready) {
		/* Collect the first second, then use its mean as the DC level */
		p->dc_buf[p->dc_fill++] = raw_ir;
		if (p->dc_fill < AF_DC_SAMPLES) {
			return false;
		}
		float sum = 0.0f;

		for (int i = 0; i < AF_DC_SAMPLES; i++) {
			sum += -(float)p->dc_buf[i];
		}
		p->dc = sum / (float)AF_DC_SAMPLES;
		p->dc_ready = true;

		/* The first second is part of the signal too: filter it now */
		bool done = false;

		for (int i = 0; i < AF_DC_SAMPLES; i++) {
			done |= feed_filtered(p, -(float)p->dc_buf[i], out);
		}
		return done;
	}
	return feed_filtered(p, -(float)raw_ir, out);
}
