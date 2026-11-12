/*
 * F3: stream raw MAX30102 samples over USB.
 *
 * Two threads, the standard pattern for sensor firmware:
 *   - sensor thread: woken by the MAX30102 interrupt, reads the FIFO over I2C
 *     and puts samples into a queue. It never waits on the USB link.
 *   - main thread: takes samples from the queue and prints them as CSV.
 *
 * Output (one line per sample, 100 lines per second):
 *   seq,ir,red
 * Lines starting with '#' are status messages, not data.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

#include "max30102.h"

#define LED_CURRENT 0x24   /* about 7 mA; raise if the signal is weak */

#define MAX_NODE DT_NODELABEL(max30102)
static const struct i2c_dt_spec sensor_i2c = I2C_DT_SPEC_GET(MAX_NODE);
static const struct gpio_dt_spec sensor_int = GPIO_DT_SPEC_GET(MAX_NODE, int_gpios);

struct stamped_sample {
	uint32_t seq;
	uint32_t ir;
	uint32_t red;
};

K_MSGQ_DEFINE(sample_queue, sizeof(struct stamped_sample), 256, 4);
K_SEM_DEFINE(data_ready, 0, 1);

static struct gpio_callback int_cb;
static volatile uint32_t fifo_overflows;
static volatile uint32_t queue_drops;
static volatile uint32_t irq_wakeups;      /* thread woken by the INT pin */
static volatile uint32_t timeout_wakeups;  /* thread woken by the safety-net timer */
static volatile uint32_t i2c_errors;
static volatile uint32_t max_batch;        /* most samples waiting in one read (FIFO holds 32) */
static volatile int sensor_status = -EAGAIN;   /* 0 once the sensor is running */

static void sensor_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_sem_give(&data_ready);   /* keep the ISR short: just wake the thread */
}

static void sensor_thread(void *a, void *b, void *c)
{
	struct max30102_sample batch[MAX30102_FIFO_DEPTH];
	uint32_t seq = 0;

	/* Keep retrying so a loose wire can be fixed without reflashing */
	while ((sensor_status = max30102_init(&sensor_i2c, LED_CURRENT)) != 0) {
		k_msleep(1000);
	}

	gpio_pin_configure_dt(&sensor_int, GPIO_INPUT);
	gpio_init_callback(&int_cb, sensor_isr, BIT(sensor_int.pin));
	gpio_add_callback_dt(&sensor_int, &int_cb);
	gpio_pin_interrupt_configure_dt(&sensor_int, GPIO_INT_EDGE_TO_ACTIVE);

	while (true) {
		/* Wait for the interrupt. The timeout is a safety net in case an
		 * edge is missed: 100 ms is 10 samples, far below the 32-sample
		 * FIFO, so no data is lost even if the interrupt never arrives. */
		if (k_sem_take(&data_ready, K_MSEC(100)) == 0) {
			irq_wakeups++;
		} else {
			timeout_wakeups++;
		}

		if (max30102_clear_interrupts(&sensor_i2c) != 0) {
			i2c_errors++;
		}

		int overflow = 0;
		int n = max30102_read_fifo(&sensor_i2c, batch, ARRAY_SIZE(batch), &overflow);

		if (n < 0) {
			i2c_errors++;
			continue;
		}
		fifo_overflows += overflow;
		if ((uint32_t)n > max_batch) {
			max_batch = n;
		}

		for (int i = 0; i < n; i++) {
			struct stamped_sample s = { seq++, batch[i].ir, batch[i].red };

			if (k_msgq_put(&sample_queue, &s, K_NO_WAIT) != 0) {
				queue_drops++;
			}
		}
	}
}

/* Priority 2 is higher than main (7, set in prj.conf): collecting samples
 * must always preempt printing them. Lower number = higher priority. */
K_THREAD_DEFINE(sensor_tid, 2048, sensor_thread, NULL, NULL, NULL, 2, 0, 0);

int main(void)
{
	if (!i2c_is_ready_dt(&sensor_i2c) || !gpio_is_ready_dt(&sensor_int)) {
		printk("# I2C or GPIO controller not ready\n");
		return -1;
	}

	/* Until the sensor answers, repeat the error so it is visible
	 * whenever the serial terminal is opened. */
	while (sensor_status != 0) {
		k_msleep(1000);
		if (sensor_status != 0) {
			printk("# MAX30102 not found (error %d): check wiring and power\n",
			       sensor_status);
		}
	}

	printk("# MAX30102 streaming at 100 Hz: seq,ir,red\n");

	struct stamped_sample s;

	while (true) {
		k_msgq_get(&sample_queue, &s, K_FOREVER);
		printk("%u,%u,%u\n", s.seq, s.ir, s.red);

		if (s.seq % 1000 == 999) {
			printk("# status: fifo_overflows=%u queue_drops=%u i2c_errors=%u "
			       "irq_wakeups=%u timeout_wakeups=%u max_batch=%u\n",
			       fifo_overflows, queue_drops, i2c_errors,
			       irq_wakeups, timeout_wakeups, max_batch);
		}
	}
	return 0;
}
