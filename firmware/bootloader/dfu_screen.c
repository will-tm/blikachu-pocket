#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <bootutil/mcuboot_status.h>
#include "gfx.h"
#include "uf2_disk.h"
#include "external/font8x8_basic.h"

#define BOOT_BG RGB565(14, 19, 27)
static const struct device *const lcd = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(DT_ALIAS(backlight));
static const struct pwm_dt_spec buzzer = PWM_DT_SPEC_GET(DT_ALIAS(buzzer));
static uint16_t pixels[LCD_WIDTH * GFX_LARGE_FONT_H];

static void rect(int x, int y, int w, int h, uint16_t color)
{
	for (int row = 0; row < h; row += GFX_LARGE_FONT_H) {
		int rows = MIN(h - row, GFX_LARGE_FONT_H);
		for (int i = 0; i < w * rows; i++) {
			pixels[i] = color;
		}
		struct display_buffer_descriptor desc = {
			.width = w, .pitch = w, .height = rows,
			.buf_size = w * rows * sizeof(uint16_t),
		};
		(void)display_write(lcd, x, y + row, &desc, pixels);
	}
}

static void text(int y, const char *s, uint16_t ink)
{
	int w = strlen(s) * GFX_LARGE_FONT_W;
	for (int row = 0; row < GFX_LARGE_FONT_H; row++) {
		for (int x = 0; x < w; x++) {
			uint8_t bits = font8x8_basic[(uint8_t)s[x / GFX_LARGE_FONT_W]][row * 8 / GFX_LARGE_FONT_H];
			unsigned bit = (x % GFX_LARGE_FONT_W) * 8 / GFX_LARGE_FONT_W;
			pixels[row * w + x] = bits & BIT(bit) ? ink : BOOT_BG;
		}
	}
	struct display_buffer_descriptor desc = {
		.width = w, .pitch = w, .height = GFX_LARGE_FONT_H,
		.buf_size = w * GFX_LARGE_FONT_H * sizeof(uint16_t),
	};
	(void)display_write(lcd, (LCD_WIDTH - w) / 2, y, &desc, pixels);
}

static void power_hold(void)
{
	const struct gpio_dt_spec d = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pwr_d_gpios);
	const struct gpio_dt_spec clk = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pwr_clk_gpios);
	gpio_pin_configure_dt(&d, GPIO_INPUT);
	gpio_pin_configure_dt(&clk, GPIO_OUTPUT_INACTIVE);
	if (gpio_pin_get_dt(&d) == 0) {
		gpio_pin_set_dt(&clk, 1);
		k_busy_wait(2);
		gpio_pin_set_dt(&clk, 0);
	}
}

void dfu_screen_message(const char *title, const char *subtitle)
{
	if (!device_is_ready(lcd)) {
		return;
	}
	(void)pwm_set_dt(&buzzer, PWM_USEC(20), 0);
	(void)pwm_set_dt(&backlight, PWM_USEC(20), PWM_USEC(10));
	(void)display_blanking_off(lcd);
	rect(0, 0, LCD_PANEL_W, LCD_PANEL_H, BOOT_BG);
	/* USB plug symbol. */
	rect(108, 68, 24, 22, COLOR_PIKACHU);
	rect(113, 57, 14, 11, COLOR_PIKACHU);
	rect(116, 59, 2, 6, BOOT_BG);
	rect(122, 59, 2, 6, BOOT_BG);
	rect(118, 90, 4, 17, COLOR_PIKACHU);
	text(126, title, COLOR_PIKACHU);
	text(150, subtitle, COLOR_WHITE);
}

void dfu_screen_progress(unsigned percent)
{
	if (!device_is_ready(lcd)) {
		return;
	}
	int width = 200 * MIN(percent, 100u) / 100;
	rect(20, 174, 200, 8, COLOR_DGRAY);
	if (width) {
		rect(20, 174, width, 8, COLOR_PIKACHU);
	}
}

void mcuboot_status_change(mcuboot_status_type_t status)
{
	switch (status) {
	case MCUBOOT_STATUS_STARTUP:
		power_hold();
		break;
	case MCUBOOT_STATUS_USB_DFU_ENTERED:
	case MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND:
		dfu_screen_message("DFU MODE", "Copy firmware.uf2");
		break;
	case MCUBOOT_STATUS_UPGRADING:
		dfu_screen_message("UPDATING FIRMWARE", "Keep power connected");
		break;
	default:
		break;
	}
}
