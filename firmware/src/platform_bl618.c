/*
 * blikachu-pocket platform backend — BL618 (v2.0 hardware).
 * Toggle power latch and timer-driven display backlight.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/bflb_bl61x_clock.h>
#include <zephyr/sys/sys_io.h>
#include <bouffalolab/bl61x/bflb_soc.h>
#include <bouffalolab/bl61x/glb_reg.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "platform.h"

LOG_MODULE_REGISTER(platform, LOG_LEVEL_INF);

/* 74LVC1G80 power latch. R40 (0R) ties D (GPIO19, /PWR_ON) to ~Q, so
 * U6 is a toggle flip-flop: every rising edge on CP (GPIO33) inverts
 * ~Q. ~Q drives Q3, which pulls the Q5 PMOS gate low: ~Q high = power
 * held (on USB too — Q5 sits between the charger output and the buck).
 * GPIO19 shares the node with U6's push-pull output, so it stays an
 * input and only reads the latch state; driving it fights U6.
 */
static const struct gpio_dt_spec pwr_d =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pwr_d_gpios);
static const struct gpio_dt_spec pwr_clk =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pwr_clk_gpios);

static void pwr_latch_set(bool hold)
{
	gpio_pin_configure_dt(&pwr_d, GPIO_INPUT);
	gpio_pin_configure_dt(&pwr_clk, GPIO_OUTPUT_INACTIVE);
	if (gpio_pin_get_dt(&pwr_d) == (hold ? 1 : 0)) {
		return;
	}
	gpio_pin_set_dt(&pwr_clk, 1);	/* rising edge toggles ~Q */
	k_busy_wait(2);
	gpio_pin_set_dt(&pwr_clk, 0);
}

/* ---- Backlight soft-PWM on timer0 ----
 *
 * The bflb PWM block has ONE period register shared by all channels;
 * every buzzer note retuned it and modulated the backlight. The
 * backlight pin (GPIO28) is now plain GPIO toggled by timer0 alarms:
 * top-of-period -> high, duty alarm -> low, ~1 kHz.
 */
static const struct gpio_dt_spec g_bl_gpio =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), backlight_gpios);
static const struct device *const g_bl_timer = DEVICE_DT_GET(DT_NODELABEL(timer0));
static uint32_t g_bl_top;          /* ticks per period */
static volatile uint32_t g_bl_duty; /* ticks high; 0 = off, >=top = solid */

static void bl_duty_alarm(const struct device *dev, uint8_t chan, uint32_t ticks,
			  void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user_data);
	(void)gpio_pin_set_dt(&g_bl_gpio, 0);
}

static void bl_top_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint32_t duty = g_bl_duty;

	if (duty == 0) {
		(void)gpio_pin_set_dt(&g_bl_gpio, 0);
		return;
	}
	(void)gpio_pin_set_dt(&g_bl_gpio, 1);
	if (duty >= g_bl_top) {
		return; /* solid on — no low edge */
	}

	struct counter_alarm_cfg alarm = {
		.callback = bl_duty_alarm,
		.ticks = duty,
	};

	(void)counter_set_channel_alarm(dev, 0, &alarm);
}

void platform_backlight_set_pct(int pct)
{
	static bool ready;

	if (!device_is_ready(g_bl_timer) || !device_is_ready(g_bl_gpio.port)) {
		return;
	}
	if (!ready) {
		struct counter_top_cfg top = {
			.callback = bl_top_cb,
			.ticks = counter_get_frequency(g_bl_timer) / 1000, /* 1 kHz */
		};

		(void)gpio_pin_configure_dt(&g_bl_gpio, GPIO_OUTPUT_INACTIVE);
		g_bl_top = top.ticks;
		if (counter_set_top_value(g_bl_timer, &top) != 0 ||
		    counter_start(g_bl_timer) != 0) {
			return;
		}
		ready = true;
	}
	pct = CLAMP(pct, 0, 100);
	g_bl_duty = (uint32_t)((uint64_t)g_bl_top * pct / 100);
}

void platform_init(void)
{
	/* Engage the power latch ASAP so the power button can be
	 * released once the console is alive.
	 */
	pwr_latch_set(true);
	LOG_INF("power latch %s (~Q=GPIO19 readback)",
		gpio_pin_get_dt(&pwr_d) == 1 ? "engaged" : "NOT engaged");

}

void platform_reboot(void)
{
	sys_reboot(SYS_REBOOT_WARM);
}

int platform_deep_sleep(void)
{
	/* "sleep" = drop the power latch, a clean
	 * power-off on battery and USB alike (power button re-latches =
	 * wake as a full reboot). The rail only
	 * stays up while the power button is held — re-latch and report
	 * so the caller re-lights the screen. TODO: BL61x HBN state for
	 * a true low-power sleep with GPIO wake.
	 */
	pwr_latch_set(false);
	k_msleep(300);

	/* Still alive: power button held. */
	pwr_latch_set(true);
	LOG_WRN("deep sleep refused: power still up (latch re-engaged)");
	return -ENOTSUP;
}

static int cmd_poweroff(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "releasing power latch...");
	k_sleep(K_MSEC(50));
	pwr_latch_set(false);
	return 0;
}
SHELL_CMD_REGISTER(poweroff, NULL, "Release the 74LVC1G80 power latch", cmd_poweroff);

/* Compare CPU cycles against the independently clocked machine timer. */
static uint32_t read_cpu_cycle(void)
{
	uint32_t cycle;
	__asm__ volatile("csrr %0, mcycle" : "=r"(cycle));
	return cycle;
}

static int cmd_perfclock(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	const struct device *clk = DEVICE_DT_GET_ANY(bflb_clock_controller);
	uint32_t hclk = 0, bclk = 0, mux = 0;

	clock_control_get_rate(clk, (void *)BL61X_CLKID_CLK_ROOT, &hclk);
	clock_control_get_rate(clk, (void *)BL61X_CLKID_CLK_BCLK, &bclk);
	clock_control_get_rate(clk, (void *)BL61X_CLKID_CLK_160M, &mux);
	uint32_t t0 = k_cycle_get_32();
	uint32_t c0 = read_cpu_cycle();
	k_busy_wait(2000);
	uint32_t cycles = read_cpu_cycle() - c0;
	uint32_t elapsed = k_cycle_get_32() - t0;
	uint32_t measured = (uint64_t)cycles * sys_clock_hw_cycles_per_sec() / MAX(elapsed, 1U);
	shell_print(sh, "cpu=%u measured=%u bclk=%u peripheral=%u", hclk, measured, bclk, mux);
	shell_print(sh, "sys=%08x mux=%08x flash=%08x spi=%08x psram=%08x",
		    sys_read32(GLB_BASE + GLB_SYS_CFG0_OFFSET),
		    sys_read32(GLB_BASE + GLB_DIG_CLK_CFG1_OFFSET),
		    sys_read32(GLB_BASE + GLB_SF_CFG0_OFFSET),
		    sys_read32(GLB_BASE + GLB_SPI_CFG0_OFFSET),
		    sys_read32(GLB_BASE + GLB_PSRAM_CFG0_OFFSET));
	return 0;
}
SHELL_CMD_REGISTER(perfclock, NULL, "Report measured CPU and peripheral clock configuration", cmd_perfclock);
