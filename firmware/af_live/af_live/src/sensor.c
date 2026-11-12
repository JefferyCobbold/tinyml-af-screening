#include "sensor.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>

#include "max30102.h"

#define LED_CURRENT 0x30   /* about 9.6 mA (0.2 mA per step); 0x24 (7.2 mA) gave a weak pulse */

#define MAX_NODE DT_NODELABEL(max30102)
static const struct i2c_dt_spec sensor_i2c = I2C_DT_SPEC_GET(MAX_NODE);
static const struct gpio_dt_spec sensor_int = GPIO_DT_SPEC_GET(MAX_NODE, int_gpios);

/* 512 samples = 5 s of buffering, far more than one window analysis (~12 ms) needs */
K_MSGQ_DEFINE(ir_queue, sizeof(int32_t), 512, 4);
K_SEM_DEFINE(data_ready, 0, 1);

static struct gpio_callback int_cb;
static struct sensor_stats stats;
static volatile int status = -EAGAIN;

int sensor_status(void)
{
	return status;
}

int sensor_get(int32_t *ir, k_timeout_t timeout)
{
	return k_msgq_get(&ir_queue, ir, timeout);
}

void sensor_get_stats(struct sensor_stats *out)
{
	*out = stats;
}

static void sensor_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_sem_give(&data_ready);
}

static void sensor_thread(void *a, void *b, void *c)
{
	struct max30102_sample batch[MAX30102_FIFO_DEPTH];

	if (!i2c_is_ready_dt(&sensor_i2c) || !gpio_is_ready_dt(&sensor_int)) {
		status = -ENODEV;
		return;
	}

	/* Keep retrying so a loose wire can be fixed without reflashing */
	while ((status = max30102_init(&sensor_i2c, LED_CURRENT)) != 0) {
		k_msleep(1000);
	}

	gpio_pin_configure_dt(&sensor_int, GPIO_INPUT);
	gpio_init_callback(&int_cb, sensor_isr, BIT(sensor_int.pin));
	gpio_add_callback_dt(&sensor_int, &int_cb);
	gpio_pin_interrupt_configure_dt(&sensor_int, GPIO_INT_EDGE_TO_ACTIVE);

	while (true) {
		/* Interrupt, or a 100 ms safety-net timer (10 samples, FIFO holds 32) */
		if (k_sem_take(&data_ready, K_MSEC(100)) == 0) {
			stats.irq_wakeups++;
		} else {
			stats.timeout_wakeups++;
		}

		if (max30102_clear_interrupts(&sensor_i2c) != 0) {
			stats.i2c_errors++;
		}

		int overflow = 0;
		int n = max30102_read_fifo(&sensor_i2c, batch, ARRAY_SIZE(batch), &overflow);

		if (n < 0) {
			stats.i2c_errors++;
			continue;
		}
		stats.fifo_overflows += overflow;

		for (int i = 0; i < n; i++) {
			int32_t ir = (int32_t)batch[i].ir;

			if (k_msgq_put(&ir_queue, &ir, K_NO_WAIT) != 0) {
				stats.queue_drops++;
			}
		}
	}
}

/* Priority 2: higher than processing (main, priority 7), so acquiring samples
 * always preempts analysing them. Lower number = higher priority. */
K_THREAD_DEFINE(sensor_tid, 2048, sensor_thread, NULL, NULL, NULL, 2, 0, 0);
