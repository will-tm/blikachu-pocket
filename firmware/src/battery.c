/* Battery monitor: VBAT_MES (GPIO10 / ADC channel 7) carries VBAT/2. */

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define VBAT_ADC_CH     7	/* GPIO10 = ADC CH7 on the v2.0 board */
#define SAMPLE_PERIOD   K_SECONDS(30)
#define ADC_RESOLUTION  12

static const struct device *const adc = DEVICE_DT_GET(DT_ALIAS(vbat_adc));
static atomic_t g_percent = ATOMIC_INIT(-1);

int battery_cached_percent(void)
{
	return atomic_get(&g_percent);
}

/* LiPo open-circuit voltage → charge, coarse piecewise map. */
static const struct {
	uint16_t mv;
	uint8_t pct;
} lipo_curve[] = {
	{4200, 100}, {4060, 90}, {3980, 80}, {3920, 70}, {3870, 60},
	{3820, 50},  {3790, 40}, {3770, 30}, {3740, 20}, {3680, 10},
	{3450, 5},   {3300, 0},
};

uint8_t battery_percent(uint16_t mv)
{
	if (mv >= lipo_curve[0].mv) {
		return 100;
	}
	for (size_t i = 1; i < ARRAY_SIZE(lipo_curve); i++) {
		if (mv >= lipo_curve[i].mv) {
			/* Linear interpolation inside the segment. */
			uint16_t mv_hi = lipo_curve[i - 1].mv;
			uint16_t mv_lo = lipo_curve[i].mv;
			uint8_t pct_hi = lipo_curve[i - 1].pct;
			uint8_t pct_lo = lipo_curve[i].pct;

			return pct_lo + (uint32_t)(mv - mv_lo) * (pct_hi - pct_lo) /
					(mv_hi - mv_lo);
		}
	}
	return 0;
}

int battery_sample_mv(uint16_t *raw_out)
{
	uint16_t raw;
	struct adc_sequence seq = {
		.channels = BIT(VBAT_ADC_CH),
		.buffer = &raw,
		.buffer_size = sizeof(raw),
		.resolution = ADC_RESOLUTION,
	};

	int rc = adc_read(adc, &seq);

	if (rc) {
		return rc;
	}
	if (raw_out) {
		*raw_out = raw;
	}
	/* Use the driver's internal reference, then undo the /2 divider. */
	int32_t mv = raw;

	rc = adc_raw_to_millivolts(adc_ref_internal(adc), ADC_GAIN_1,
				 ADC_RESOLUTION, &mv);
	return rc == 0 ? mv * 2 : rc;
}

static void battery_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(g_battery_work, battery_work_fn);

static void battery_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int mv = battery_sample_mv(NULL);

	if (mv >= 0) {
		atomic_set(&g_percent, battery_percent(mv));
		LOG_INF("battery: %d mV (%u%%)", mv, battery_percent(mv));
	} else {
		LOG_WRN("battery: adc_read failed (%d)", mv);
	}

	k_work_reschedule(&g_battery_work, SAMPLE_PERIOD);
}

static int battery_init(void)
{
	if (!device_is_ready(adc)) {
		LOG_WRN("battery: ADC not ready, monitor disabled");
		return 0;
	}

	const struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = VBAT_ADC_CH,
		.input_positive = VBAT_ADC_CH,
	};
	int rc = adc_channel_setup(adc, &cfg);

	if (rc) {
		LOG_WRN("battery: channel setup failed (%d)", rc);
		return 0;
	}

	k_work_reschedule(&g_battery_work, K_SECONDS(1));
	return 0;
}
SYS_INIT(battery_init, APPLICATION, 90);
