/*
 * AF screening firmware — Step F5a: run the int8 model on the board.
 *
 * No sensor yet. The firmware feeds "golden" feature vectors exported from
 * Python (test_vectors.h) into the model and checks that the board produces
 * the same int8 outputs that TensorFlow Lite produced on the PC. It also
 * measures inference time and memory use.
 *
 * Once this passes, the model is known to be deployed correctly, and any later
 * mismatch must come from the signal processing, not the model.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/timing/timing.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "af_model.h"      /* the model, as a C byte array (from the notebook) */
#include "test_vectors.h"  /* golden inputs + expected outputs (from the notebook) */

namespace {

/* Working memory for the model's tensors. TFLite Micro never calls malloc;
 * everything lives in this one static buffer. Start generous, then shrink to
 * what arena_used_bytes() reports. */
constexpr size_t kArenaSize = 8 * 1024;
alignas(16) uint8_t tensor_arena[kArenaSize];

/* Register only the operations this model uses (keeps flash small).
 * The Normalization layer converts to SUB + MUL. If AllocateTensors() fails
 * with "Didn't find op", add that op here and raise the template count. */
using AfOpResolver = tflite::MicroMutableOpResolver<4>;

int8_t quantize(float x, float scale, int zero_point)
{
	int32_t q = static_cast<int32_t>(lroundf(x / scale)) + zero_point;
	if (q < -128) q = -128;
	if (q > 127) q = 127;
	return static_cast<int8_t>(q);
}

}  // namespace

int main(void)
{
	/* Give the PC a moment to open the USB serial port. */
	k_sleep(K_SECONDS(2));
	printk("\n=== AF model test on %s ===\n", CONFIG_BOARD_TARGET);

	const tflite::Model *model = tflite::GetModel(af_model);
	if (model->version() != TFLITE_SCHEMA_VERSION) {
		printk("Model schema %u does not match runtime %d\n",
		       model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	}

	static AfOpResolver resolver;
	resolver.AddFullyConnected();
	resolver.AddLogistic();
	resolver.AddSub();
	resolver.AddMul();

	static tflite::MicroInterpreter interpreter(model, resolver,
						    tensor_arena, kArenaSize);
	if (interpreter.AllocateTensors() != kTfLiteOk) {
		printk("AllocateTensors failed (arena too small or op missing)\n");
		return -1;
	}

	TfLiteTensor *input = interpreter.input(0);
	TfLiteTensor *output = interpreter.output(0);

	/* Read quantization parameters from the model itself, so retraining
	 * the model never requires editing these numbers by hand. */
	const float in_scale = input->params.scale;
	const int in_zp = input->params.zero_point;
	const float out_scale = output->params.scale;
	const int out_zp = output->params.zero_point;

	printk("Model: %u bytes | arena used: %u of %u bytes\n",
	       af_model_len, (unsigned)interpreter.arena_used_bytes(),
	       (unsigned)kArenaSize);
	printk("Input  scale %.6f zero point %d\n", (double)in_scale, in_zp);
	printk("Output scale %.6f zero point %d\n\n", (double)out_scale, out_zp);

	timing_init();
	timing_start();

	while (true) {
		int exact = 0, close = 0, same_decision = 0;
		uint64_t total_ns = 0;

		for (int i = 0; i < AF_NUM_TESTS; i++) {
			for (int f = 0; f < AF_NUM_FEATURES; f++) {
				input->data.int8[f] =
					quantize(af_test_features[i][f], in_scale, in_zp);
			}

			timing_t t0 = timing_counter_get();
			if (interpreter.Invoke() != kTfLiteOk) {
				printk("Invoke failed on test %d\n", i);
				return -1;
			}
			timing_t t1 = timing_counter_get();
			total_ns += timing_cycles_to_ns(timing_cycles_get(&t0, &t1));

			const int8_t got = output->data.int8[0];
			const int8_t want = af_test_expected[i];
			const float prob = (got - out_zp) * out_scale;

			exact += (got == want);
			close += (abs(got - want) <= 1);
			same_decision += ((got >= 0) == (want >= 0));

			printk("%2d  label %-6s  board %4d  python %4d  p(AF)=%.3f  -> %s\n",
			       i, af_test_label[i] ? "AF" : "normal", got, want,
			       (double)prob, got >= 0 ? "possible AF" : "normal");
		}

		printk("\nExact match %d/%d | within 1 step %d/%d | same decision %d/%d\n",
		       exact, AF_NUM_TESTS, close, AF_NUM_TESTS,
		       same_decision, AF_NUM_TESTS);
		printk("Average inference time: %u us\n",
		       (unsigned)(total_ns / AF_NUM_TESTS / 1000));
		printk("%s\n\n", same_decision == AF_NUM_TESTS ? "RESULT: PASS" : "RESULT: FAIL");

		k_sleep(K_SECONDS(10));
	}
	return 0;
}
