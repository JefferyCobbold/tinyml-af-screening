/*
 * Live AF screening on the Arduino Nano 33 BLE Sense.
 *
 * Place a finger on the MAX30102. Once contact has settled, the board
 * analyses each 30 s of pulse on its own and reports:
 *   normal / possible AF / unreliable signal
 * together with heart rate and signal quality. Nothing runs on the PC except
 * a serial terminal to read the output.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/timing/timing.h>

#include <math.h>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "af_dsp.h"
#include "af_model.h"
#include "af_session.h"
#include "sensor.h"

namespace {

constexpr size_t kArenaSize = 4 * 1024;
alignas(16) uint8_t tensor_arena[kArenaSize];

tflite::MicroInterpreter *interpreter;
TfLiteTensor *model_in;
TfLiteTensor *model_out;

struct af_session session;

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

/* Returns the model's AF probability; *raw gets the int8 output (AF if >= 0). */
float run_model(const float features[AF_NUM_FEATURES], int8_t *raw)
{
	const float scale = model_in->params.scale;
	const int zp = model_in->params.zero_point;

	for (int f = 0; f < AF_NUM_FEATURES; f++) {
		int32_t q = static_cast<int32_t>(lroundf(features[f] / scale)) + zp;

		model_in->data.int8[f] = static_cast<int8_t>(q < -128 ? -128 : (q > 127 ? 127 : q));
	}
	interpreter->Invoke();
	*raw = model_out->data.int8[0];
	return (*raw - model_out->params.zero_point) * model_out->params.scale;
}

void print_time()
{
	int64_t s = k_uptime_get() / 1000;

	printk("[%02d:%02d] ", (int)(s / 60), (int)(s % 60));
}

}  // namespace

int main(void)
{
	k_sleep(K_SECONDS(2));
	printk("\n=== Live AF screening on %s ===\n", CONFIG_BOARD_TARGET);

	if (!model_init()) {
		return -1;
	}
	while (sensor_status() != 0) {
		printk("MAX30102 not found (error %d): check wiring and power\n", sensor_status());
		k_sleep(K_SECONDS(1));
	}

	print_time();
	printk("Sensor ready. Place your finger lightly on the sensor.\n");

	timing_init();
	timing_start();
	af_session_reset(&session);

	int window = 0;
	uint32_t last_overflows = 0;

	while (true) {
		int32_t ir;

		if (sensor_get(&ir, K_SECONDS(1)) != 0) {
			continue;
		}

		struct af_window_result r;
		timing_t t0 = timing_counter_get();
		enum af_event event = af_session_push(&session, ir, &r);

		switch (event) {
		case AF_EVENT_FINGER_DETECTED:
			print_time();
			printk("Finger detected, hold still...\n");
			break;

		case AF_EVENT_MEASURING:
			window = 0;
			print_time();
			printk("Measuring. First result in about 32 s.\n");
			break;

		case AF_EVENT_FINGER_LIFTED:
			print_time();
			printk("Finger lifted. Place your finger to start again.\n");
			break;

		case AF_EVENT_WINDOW_DONE: {
			int8_t raw = -128;
			float p_af = 0.0f;

			if (r.features_valid) {
				p_af = run_model(r.features, &raw);
			}
			timing_t t1 = timing_counter_get();
			uint32_t us = (uint32_t)(timing_cycles_to_ns(timing_cycles_get(&t0, &t1)) / 1000);

			print_time();
			if (!r.passes_gate) {
				printk("Window %d: unreliable signal (SQI %.2f, %d beats). Keep still, "
				       "light touch.\n", window, (double)r.sqi, r.n_beats);
			} else {
				printk("Window %d: %s | HR %.0f bpm | SQI %.2f | p(AF) %.2f | %u us\n",
				       window, raw >= 0 ? "POSSIBLE AF" : "normal",
				       (double)r.heart_rate_bpm, (double)r.sqi, (double)p_af, us);
			}
			window++;

			struct sensor_stats st;

			sensor_get_stats(&st);
			if (st.fifo_overflows != last_overflows || st.queue_drops != 0) {
				printk("    warning: samples lost (fifo %u, queue %u, i2c errors %u)\n",
				       st.fifo_overflows, st.queue_drops, st.i2c_errors);
				last_overflows = st.fifo_overflows;
			}
			break;
		}
		default:
			break;
		}
	}
	return 0;
}
