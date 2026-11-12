/*
 * Minimal MAX30102 driver on Zephyr's I2C API.
 *
 * Configuration (matches the Python pipeline):
 *   SpO2 mode (red + IR LEDs), 400 Hz internal sampling,
 *   4-sample averaging in the FIFO -> 100 Hz output,
 *   411 us LED pulses (18-bit ADC resolution), 4096 nA ADC range.
 */
#pragma once

#include <stdint.h>
#include <zephyr/drivers/i2c.h>

struct max30102_sample {
	uint32_t red;   /* 18-bit raw counts */
	uint32_t ir;    /* 18-bit raw counts; the AF pipeline uses this channel */
};

#define MAX30102_FIFO_DEPTH 32

/* Check the part ID, reset the chip and apply the configuration above.
 * Returns 0 on success, negative errno on failure. */
int max30102_init(const struct i2c_dt_spec *i2c, uint8_t led_current);

/* Read and clear the interrupt status registers (releases the INT pin). */
int max30102_clear_interrupts(const struct i2c_dt_spec *i2c);

/* Read every sample currently in the FIFO (up to max).
 * Returns the number of samples read, or negative errno.
 * *overflow is set to the number of samples the chip had to discard
 * because the FIFO was full (0 in normal operation). */
int max30102_read_fifo(const struct i2c_dt_spec *i2c,
		       struct max30102_sample *out, int max, int *overflow);
