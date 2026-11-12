/*
 * Sensor acquisition: a high-priority thread reads the MAX30102 FIFO and
 * queues raw IR samples at 100 Hz for the processing thread.
 */
#ifndef SENSOR_H_
#define SENSOR_H_

#include <stdint.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sensor_stats {
	uint32_t fifo_overflows;   /* samples the chip discarded (FIFO full) */
	uint32_t queue_drops;      /* samples dropped because processing fell behind */
	uint32_t i2c_errors;
	uint32_t irq_wakeups;
	uint32_t timeout_wakeups;
};

/* 0 once the sensor answers and is configured, negative errno before that. */
int sensor_status(void);

/* Wait for the next raw IR sample. Returns 0 on success. */
int sensor_get(int32_t *ir, k_timeout_t timeout);

void sensor_get_stats(struct sensor_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_H_ */
