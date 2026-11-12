#include "max30102.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* Register map (MAX30102 datasheet) */
#define REG_INT_STATUS_1   0x00
#define REG_INT_STATUS_2   0x01
#define REG_INT_ENABLE_1   0x02
#define REG_INT_ENABLE_2   0x03
#define REG_FIFO_WR_PTR    0x04
#define REG_OVF_COUNTER    0x05
#define REG_FIFO_RD_PTR    0x06
#define REG_FIFO_DATA      0x07
#define REG_FIFO_CONFIG    0x08
#define REG_MODE_CONFIG    0x09
#define REG_SPO2_CONFIG    0x0A
#define REG_LED1_PA        0x0C   /* red LED current */
#define REG_LED2_PA        0x0D   /* IR LED current */
#define REG_PART_ID        0xFF

#define PART_ID_MAX30102   0x15

/* MODE_CONFIG */
#define MODE_RESET         BIT(6)
#define MODE_SPO2          0x03   /* red + IR */

/* FIFO_CONFIG: SMP_AVE[7:5], FIFO_ROLLOVER_EN[4], FIFO_A_FULL[3:0] */
#define FIFO_SMP_AVE_4     (0x2 << 5)
#define FIFO_ROLLOVER_EN   BIT(4)
#define FIFO_A_FULL_17     0x0F   /* interrupt when 32 - 15 = 17 samples are waiting */

/* SPO2_CONFIG: ADC_RGE[6:5], SR[4:2], LED_PW[1:0] */
#define SPO2_ADC_4096NA    (0x1 << 5)
#define SPO2_SR_400HZ      (0x3 << 2)
#define SPO2_PW_411US      0x3

/* INT_ENABLE_1 */
#define INT_A_FULL_EN      BIT(7)

#define BYTES_PER_SAMPLE   6      /* 3 bytes red + 3 bytes IR */
#define SAMPLE_MASK        0x3FFFF /* 18-bit ADC */

static int write_reg(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte_dt(i2c, reg, val);
}

static int read_reg(const struct i2c_dt_spec *i2c, uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte_dt(i2c, reg, val);
}

int max30102_init(const struct i2c_dt_spec *i2c, uint8_t led_current)
{
	uint8_t id;
	int ret = read_reg(i2c, REG_PART_ID, &id);

	if (ret) {
		return ret;                    /* no answer on I2C: wiring problem */
	}
	if (id != PART_ID_MAX30102) {
		printk("# MAX30102: unexpected part ID 0x%02x\n", id);
		return -ENODEV;
	}

	/* Software reset, then wait for the chip to clear the reset bit */
	ret = write_reg(i2c, REG_MODE_CONFIG, MODE_RESET);
	if (ret) {
		return ret;
	}
	for (int i = 0; i < 10; i++) {
		uint8_t mode;

		k_msleep(10);
		if (read_reg(i2c, REG_MODE_CONFIG, &mode) == 0 && !(mode & MODE_RESET)) {
			break;
		}
	}

	uint8_t dummy;
	read_reg(i2c, REG_INT_STATUS_1, &dummy);   /* clear power-ready flag */

	const uint8_t config[][2] = {
		{ REG_INT_ENABLE_1, INT_A_FULL_EN },
		{ REG_INT_ENABLE_2, 0x00 },
		{ REG_FIFO_WR_PTR,  0x00 },
		{ REG_OVF_COUNTER,  0x00 },
		{ REG_FIFO_RD_PTR,  0x00 },
		{ REG_FIFO_CONFIG,  FIFO_SMP_AVE_4 | FIFO_ROLLOVER_EN | FIFO_A_FULL_17 },
		{ REG_SPO2_CONFIG,  SPO2_ADC_4096NA | SPO2_SR_400HZ | SPO2_PW_411US },
		{ REG_LED1_PA,      led_current },
		{ REG_LED2_PA,      led_current },
		{ REG_MODE_CONFIG,  MODE_SPO2 },           /* starts sampling */
	};

	for (size_t i = 0; i < ARRAY_SIZE(config); i++) {
		ret = write_reg(i2c, config[i][0], config[i][1]);
		if (ret) {
			return ret;
		}
	}
	return 0;
}

int max30102_clear_interrupts(const struct i2c_dt_spec *i2c)
{
	uint8_t status[2];

	/* Reading both status registers clears them and releases INT */
	return i2c_burst_read_dt(i2c, REG_INT_STATUS_1, status, sizeof(status));
}

int max30102_read_fifo(const struct i2c_dt_spec *i2c,
		       struct max30102_sample *out, int max, int *overflow)
{
	uint8_t ptrs[3];   /* WR_PTR, OVF_COUNTER, RD_PTR are consecutive registers */
	int ret = i2c_burst_read_dt(i2c, REG_FIFO_WR_PTR, ptrs, sizeof(ptrs));

	if (ret) {
		return ret;
	}

	*overflow = ptrs[1];
	int count = (ptrs[0] - ptrs[2]) & (MAX30102_FIFO_DEPTH - 1);

	if (count == 0 && ptrs[1] != 0) {
		count = MAX30102_FIFO_DEPTH;   /* FIFO completely full after overflow */
	}
	if (count > max) {
		count = max;
	}
	if (count == 0) {
		return 0;
	}

	/* One burst read: the FIFO_DATA register does not auto-increment,
	 * so the chip streams consecutive samples from the same address. */
	uint8_t raw[MAX30102_FIFO_DEPTH * BYTES_PER_SAMPLE];

	ret = i2c_burst_read_dt(i2c, REG_FIFO_DATA, raw, count * BYTES_PER_SAMPLE);
	if (ret) {
		return ret;
	}

	for (int i = 0; i < count; i++) {
		const uint8_t *p = &raw[i * BYTES_PER_SAMPLE];

		out[i].red = ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) & SAMPLE_MASK;
		out[i].ir  = ((uint32_t)p[3] << 16 | (uint32_t)p[4] << 8 | p[5]) & SAMPLE_MASK;
	}
	return count;
}
