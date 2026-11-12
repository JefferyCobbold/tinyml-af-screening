/*
 * F4: the full AF screening pipeline on the board, verified against Python.
 *
 * Golden segments (golden_dsp.h, exported from the notebook) contain raw
 * MAX30102-style IR counts: your own recording and a real AF patient from
 * MIMIC. Each segment is streamed sample by sample through the same code the
 * live sensor will use, and every 30 s window is compared with Python:
 * beat count, SQI, features, quality gate and model output.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/timing/timing.h>

#include <math.h>
#include <stdlib.h>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "af_dsp.h"
#include "af_model.h"
#include "golden_dsp.h"

namespace {

constexpr size_t kArenaSize = 4 * 1024;
alignas(16) uint8_t tensor_arena[kArenaSize];

tflite::MicroInterpreter *interpreter;
TfLiteTensor *model_in;
TfLiteTensor *model_out;

struct af_pipeline pipeline;

bool model_init()
{
	const tflite::Model *model = tflite::GetModel(af_model);

	if (model->version() != TFLITE_SCHEMA_VERSION) {
		printk("Model schema mismatch\n");
		return false;
	}
	static tflite::MicroMutableOpResolver<4> resolver;

	resolver.AddFullyConnected();
	resolver.AddLogistic();
	resolver.AddSub();
	resolver.AddMul();

	static tflite::MicroInterpreter interp(model, resolver, tensor_arena, kArenaSize);

	if (interp.AllocateTensors() != kTfLiteOk) {
		printk("AllocateTensors failed\n");
		return false;
	}
	interpreter = &interp;
	model_in = interp.input(0);
	model_out = interp.output(0);
	return true;
}

int8_t run_model(const float features[AF_NUM_FEATURES])
{
	const float scale = model_in->params.scale;
	const int zp = model_in->params.zero_point;

	for (int f = 0; f < AF_NUM_FEATURES; f++) {
		int32_t q = static_cast<int32_t>(lroundf(features[f] / scale)) + zp;

		model_in->data.int8[f] = static_cast<int8_t>(q < -128 ? -128 : (q > 127 ? 127 : q));
	}
	interpreter->Invoke();
	return model_out->data.int8[0];
}

const char *class_name(bool gate, int8_t out)
{
	if (!gate) {
		return "unreliable signal";
	}
	return out >= 0 ? "possible AF" : "normal";
}

}  // namespace

int main(void)
{
	k_sleep(K_SECONDS(2));
	printk("\n=== AF pipeline verification on %s ===\n", CONFIG_BOARD_TARGET);

	if (!model_init()) {
		return -1;
	}
	timing_init();
	timing_start();

	while (true) {
		int windows = 0, matched = 0;
		uint64_t worst_ns = 0;
		float worst_sqi = 0.0f, worst_feat = 0.0f;

		for (int s = 0; s < GOLDEN_NUM_SEGMENTS; s++) {
			printk("\n%s (true rhythm: %s)\n", golden_seg_name[s],
			       golden_seg_label[s] ? "AF" : "normal");
			printk(" win beats   SQI     HR  model  board result       python\n");

			af_pipeline_reset(&pipeline);
			int k = 0;

			for (int i = 0; i < GOLDEN_SEG_LEN && k < GOLDEN_WINDOWS_PER_SEG; i++) {
				struct af_window_result r;

				timing_t t0 = timing_counter_get();
				bool done = af_pipeline_push(&pipeline, golden_raw[s][i], &r);
				int8_t out = -128;

				if (done && r.features_valid) {
					out = run_model(r.features);
				}
				timing_t t1 = timing_counter_get();

				if (!done) {
					continue;
				}

				/* The push that completes a window does all the analysis */
				uint64_t ns = timing_cycles_to_ns(timing_cycles_get(&t0, &t1));

				if (ns > worst_ns) {
					worst_ns = ns;
				}

				const struct golden_window *g = &golden_expected[s][k];
				float d_sqi = fabsf(r.sqi - g->sqi);
				float d_feat = 0.0f;

				for (int f = 0; f < AF_NUM_FEATURES; f++) {
					float d = fabsf(r.features[f] - g->features[f]);

					d_feat = d > d_feat ? d : d_feat;
				}
				worst_sqi = d_sqi > worst_sqi ? d_sqi : worst_sqi;
				worst_feat = d_feat > worst_feat ? d_feat : worst_feat;

				bool ok = r.n_beats == g->n_beats && d_sqi < 1e-3f &&
					  r.features_valid == (bool)g->valid &&
					  r.passes_gate == (bool)g->gate &&
					  (!g->valid || (d_feat < 1e-4f && abs(out - g->model_out) <= 1));

				windows++;
				matched += ok;

				printk("  %d   %3d   %.3f %6.1f  %4d   %-17s  %s %s\n", k, r.n_beats,
				       (double)r.sqi, (double)r.heart_rate_bpm, out,
				       class_name(r.passes_gate, out),
				       class_name(g->gate, g->model_out), ok ? "OK" : "MISMATCH");
				k++;
			}
		}

		printk("\nWindows matching Python: %d/%d\n", matched, windows);
		printk("Largest difference: SQI %.2e, features %.2e\n",
		       (double)worst_sqi, (double)worst_feat);
		printk("Time to analyse one 30 s window (DSP + model): %u us\n",
		       (unsigned)(worst_ns / 1000));
		printk("%s\n", matched == windows ? "RESULT: PASS" : "RESULT: FAIL");

		k_sleep(K_SECONDS(15));
	}
	return 0;
}
