/*
 * MCP73871 charger status: STAT1=GPIO11, STAT2=GPIO12, PG=GPIO13. Open-drain, active-low, each
 * loaded by a charge-LED through 1K — the released level floats near
 * the LED threshold, so internal pull-ups make the high solid.
 *
 * IRQ on both edges of all three lines logs decoded state changes;
 * the `charger` shell command prints the state + a live VBAT sample.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "battery.h"
#include "charger.h"

LOG_MODULE_REGISTER(charger, LOG_LEVEL_INF);

static const struct gpio_dt_spec stat1 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), chg_stat1_gpios);
static const struct gpio_dt_spec stat2 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), chg_stat2_gpios);
static const struct gpio_dt_spec pg = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), chg_pg_gpios);

/* MCP73871 status truth table; args are "asserted" (pin low). */
static const char *charger_decode(bool s1, bool s2, bool pg_in)
{
	if (!pg_in) {
		return "on battery (no input power)";
	}
	if (s1 && s2) {
		return "FAULT (safety timer / temperature)";
	}
	if (s1) {
		return "charging";
	}
	if (s2) {
		return "charge complete";
	}
	return "standby (charge suspended / no battery)";
}

bool charger_input_present(void)
{
	return gpio_pin_get_dt(&pg) == 1;
}

static void chg_log_state(void)
{
	bool s1 = gpio_pin_get_dt(&stat1) == 1;
	bool s2 = gpio_pin_get_dt(&stat2) == 1;
	bool p = gpio_pin_get_dt(&pg) == 1;

	LOG_INF("charger: PG=%d STAT1=%d STAT2=%d — %s", p, s1, s2, charger_decode(s1, s2, p));
}

static void chg_irq_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	chg_log_state();
}
static struct gpio_callback g_chg_cb;

static int cmd_charger(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bool s1 = gpio_pin_get_dt(&stat1) == 1;
	bool s2 = gpio_pin_get_dt(&stat2) == 1;
	bool p = gpio_pin_get_dt(&pg) == 1;

	shell_print(sh, "PG=%d STAT1=%d STAT2=%d (1 = asserted/low) — %s", p, s1, s2,
		    charger_decode(s1, s2, p));

	uint16_t raw = 0;
	int mv = battery_sample_mv(&raw);

	if (mv >= 0) {
		shell_print(sh, "VBAT: %d mV (%u%%, raw %u = pin %u mV)", mv, battery_percent(mv),
			    raw, (unsigned)((uint32_t)raw * 3300 / 4096));
	} else {
		shell_error(sh, "VBAT: adc_read failed (%d)", mv);
	}
	return 0;
}
SHELL_CMD_REGISTER(charger, NULL, "MCP73871 status (PG/STAT1/STAT2) + VBAT", cmd_charger);

static int charger_init(void)
{
	const struct gpio_dt_spec *specs[] = {&stat1, &stat2, &pg};

	for (size_t i = 0; i < ARRAY_SIZE(specs); i++) {
		if (!gpio_is_ready_dt(specs[i])) {
			LOG_WRN("charger: gpio not ready, monitor disabled");
			return 0;
		}
		(void)gpio_pin_configure_dt(specs[i], GPIO_INPUT | GPIO_PULL_UP);
		(void)gpio_pin_interrupt_configure_dt(specs[i], GPIO_INT_EDGE_BOTH);
	}

	gpio_init_callback(&g_chg_cb, chg_irq_cb,
			   BIT(stat1.pin) | BIT(stat2.pin) | BIT(pg.pin));
	(void)gpio_add_callback(stat1.port, &g_chg_cb);

	chg_log_state(); /* baseline in the boot log */
	return 0;
}
SYS_INIT(charger_init, APPLICATION, 91);
