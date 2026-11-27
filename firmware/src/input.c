#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include "input.h"

LOG_MODULE_REGISTER(blikachu_input, LOG_LEVEL_INF);

static input_handler_t g_handler;
static input_filter_t g_filter;

void input_set_handler(input_handler_t h)
{
	g_handler = h;
}

void input_set_filter(input_filter_t f)
{
	g_filter = f;
}

void input_inject(uint16_t code, bool pressed)
{
	struct input_event evt = {
		.type = INPUT_EV_KEY,
		.code = code,
		.value = pressed ? 1 : 0,
		.sync = 1,
	};
	input_handler_t h = g_handler;

	if (h) {
		h(&evt);
	}
}

static void dispatch(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("key type=%u code=%u val=%u", evt->type, evt->code, evt->value);

	input_filter_t f = g_filter;

	if (f && f(evt)) {
		return;
	}

	input_handler_t h = g_handler;
	if (h) {
		h(evt);
	}
}
INPUT_CALLBACK_DEFINE(NULL, dispatch, NULL);
