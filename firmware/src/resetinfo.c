/*
 * Crash forensics. CDC ACM can't transmit from fault context (USB needs
 * ISRs), so a fault dump over the console is never visible. Instead:
 * k_sys_fatal_error_handler() stashes the reason + exception frame into
 * __noinit RAM (survives a warm reset) and reboots; the `resetinfo`
 * shell command prints that record after the fact, alongside the hardware reset cause.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/shell/shell.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/hwinfo.h>

#include "emu.h"

#define FAULT_MAGIC 0xFA07C0DE

struct fault_record {
	uint32_t magic;
	uint32_t reason;
	uint32_t esf_valid;
	uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
	uint32_t uptime_ms;
	char thread[16];
};

__noinit static struct fault_record g_fault_record;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	struct fault_record *fr = &g_fault_record;

	fr->reason = reason;
	fr->uptime_ms = k_uptime_get_32();
	fr->esf_valid = (esf != NULL);
	if (esf != NULL) {
		/* Same record layout, riscv naming: pc=mepc lr=ra
		 * r0-r3=a0-a3 r12=t0 xpsr=mstatus.
		 */
		fr->r0 = esf->a0;
		fr->r1 = esf->a1;
		fr->r2 = esf->a2;
		fr->r3 = esf->a3;
		fr->r12 = esf->t0;
		fr->lr = esf->ra;
		fr->pc = esf->mepc;
		fr->xpsr = esf->mstatus;
	}

	const char *name = k_thread_name_get(k_current_get());
	strncpy(fr->thread, name ? name : "?", sizeof(fr->thread) - 1);
	fr->thread[sizeof(fr->thread) - 1] = '\0';

	fr->magic = FAULT_MAGIC;

	/* Raw synchronous dump to UART0's TX FIFO — works from any
	 * context, even IRQ-locked fault-in-fault, unlike every logging
	 * path. 16 hex chars + labels at 2 Mbaud-ish worth of spinning.
	 */
	{
		static const char hdr[] = "\r\nFATAL r=";
		const char *hx = "0123456789abcdef";
		uint32_t vals[3] = {reason, fr->pc, fr->lr};

		for (const char *c = hdr; *c; c++) {
			*(volatile uint32_t *)(0x2000a000 + 0x88) = *c;
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
		for (int v = 0; v < 3; v++) {
			for (int n = 28; n >= 0; n -= 4) {
				*(volatile uint32_t *)(0x2000a000 + 0x88) =
					hx[(vals[v] >> n) & 0xf];
				for (volatile int i = 0; i < 20000; i++) {
				}
			}
			*(volatile uint32_t *)(0x2000a000 + 0x88) = ' ';
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}

	/* No LOG_PANIC()/flush — CDC ACM TX is dead here. Just reboot;
	 * the record is read back post-mortem via `resetinfo`.
	 */
	sys_reboot(SYS_REBOOT_WARM);
}

/* Hardware watchdog: 4 s timeout, fed every second from a priority-1
 * thread. Only a system-level wedge (interrupts dead, scheduler
 * corrupted, fault-in-fault lockup) starves the feeder — a runaway
 * lower-priority thread like the emu worker doesn't trip it. On bite
 * the chip resets, SRAM (and the fault record above, if one was
 * written) survives, and chip_reset shows the WATCHDOG bits.
 */
/* bflb wdt counts in a 16-bit @32kHz-ish domain: 1999 ms max. */
#define WDT_TIMEOUT_MS 1900

/* Emu-stall watchdog, piggybacked on the feeder's 1 s tick: a running,
 * unpaused game whose frame counter stops advancing is wedged inside
 * retro_run (the stop flag is only checked at frame boundaries, so
 * `gb stop` can't help either). Two stages: after QUIT_S stalled
 * seconds try the polite emu_quit() (recovers a loop that still
 * reaches the boundary eventually); after RESET_S write a fault record
 * (custom reason and frame counter) and reboot.
 */
#define EMU_STALL_QUIT_S  5
#define EMU_STALL_RESET_S 10
#define REASON_EMU_STALL  0xE5000000

static void emu_stall_check(void)
{
	static uint32_t last_frame;
	static uint32_t stall_s;
	struct emu_stats s;

	emu_get_stats(&s);
	if (!s.running || s.paused || s.frame != last_frame) {
		last_frame = s.frame;
		stall_s = 0;
		return;
	}

	stall_s++;
	if (stall_s == EMU_STALL_QUIT_S) {
		printk("emu stall: no frame progress for %u s — quitting game\n", stall_s);
		emu_quit();
	} else if (stall_s >= EMU_STALL_RESET_S) {
		struct fault_record *fr = &g_fault_record;

		memset(fr, 0, sizeof(*fr));
		fr->reason = REASON_EMU_STALL;
		fr->uptime_ms = k_uptime_get_32();
		fr->esf_valid = 1;
		fr->r0 = s.frame;
		strncpy(fr->thread, "emu", sizeof(fr->thread) - 1);
		fr->magic = FAULT_MAGIC;
		sys_reboot(SYS_REBOOT_WARM);
	}
}

#if defined(CONFIG_WATCHDOG)
static const struct device *const wdt = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0));
#else
static const struct device *const wdt; /* feeder becomes stall-check only */
#endif

/* Long foreground flash operations (multi-MB WebUI commits, big FAT
 * chain unlinks) legitimately exceed the BL61x 1.9 s wdt ceiling —
 * pause/resume instead of sprinkling feeds through fs internals.
 */
static volatile bool g_wdt_paused;
static volatile int g_wdt_ch = -2; /* -2 = not yet started */

void resetinfo_wdt_pause(void)
{
	g_wdt_paused = true;
	if (wdt != NULL && device_is_ready(wdt) && g_wdt_ch >= 0) {
		(void)wdt_disable(wdt);
		g_wdt_ch = -1; /* re-arm on resume */
	}
}

void resetinfo_wdt_resume(void)
{
	g_wdt_paused = false;
}

static void wdt_feeder(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (!wdt || !device_is_ready(wdt)) {
		return;
	}

	const struct wdt_timeout_cfg cfg = {
		.window.max = WDT_TIMEOUT_MS,
		.flags = WDT_FLAG_RESET_SOC,
	};

	g_wdt_ch = wdt_install_timeout(wdt, &cfg);
	if (g_wdt_ch < 0 || wdt_setup(wdt, 0) < 0) {
		g_wdt_ch = -2;
		return;
	}

	for (;;) {
		static uint32_t tick;

		if (g_wdt_paused) {
			k_sleep(K_MSEC(250));
			continue;
		}
		if (g_wdt_ch == -1) {
			/* resumed after a pause: re-arm */
			int ch2 = wdt_install_timeout(wdt, &cfg);

			if (ch2 >= 0 && wdt_setup(wdt, 0) == 0) {
				g_wdt_ch = ch2;
			}
			k_sleep(K_MSEC(250));
			continue;
		}
		(void)wdt_feed(wdt, g_wdt_ch);
		/* emu_stall_check assumes 1 Hz calls */
		if ((tick++ % (4)) == 0) {
			emu_stall_check();
		}
		/* BL61x wdt maxes at ~2 s and big FATFS writes hold IRQs
		 * locked in back-to-back per-sector windows — a 1 s sleep
		 * starves past the timeout mid-write. Feed often; the
		 * emu-stall check stays effectively 1 Hz via its own
		 * counter.
		 */
		k_sleep(K_MSEC(250));
	}
}
/* BL61x: the WiFi blob's cooperative threads can hog the CPU past the
 * 2 s hardware limit under heavy RX — a preemptible feeder starves.
 * Run the feeder cooperative-highest: it gets in at every yield/pend,
 * and the blob pends constantly.
 */
K_THREAD_DEFINE(wdt_feeder_tid, 512, wdt_feeder, NULL, NULL, NULL, K_PRIO_COOP(0), 0, 0);

static const char *reason_str(uint32_t reason)
{
	switch (reason) {
	case K_ERR_CPU_EXCEPTION:
		return "CPU exception";
	case K_ERR_SPURIOUS_IRQ:
		return "spurious IRQ";
	case K_ERR_STACK_CHK_FAIL:
		return "stack overflow";
	case K_ERR_KERNEL_OOPS:
		return "kernel oops";
	case K_ERR_KERNEL_PANIC:
		return "kernel panic";
	case REASON_EMU_STALL:
		return "emu stall watchdog (r0=frame)";
	default:
		return "?";
	}
}


static int cmd_resetinfo(const struct shell *sh, size_t argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
		g_fault_record.magic = 0;
		shell_print(sh, "fault record cleared");
		return 0;
	}

	uint32_t cause = 0;

	(void)hwinfo_get_reset_cause(&cause);
	shell_print(sh, "reset cause: 0x%08x%s%s%s%s", cause,
		    (cause & RESET_POR) ? " POR" : "",
		    (cause & RESET_PIN) ? " PIN" : "",
		    (cause & RESET_BROWNOUT) ? " BROWNOUT" : "",
		    (cause & RESET_WATCHDOG) ? " WATCHDOG" : "");

	const struct fault_record *fr = &g_fault_record;

	if (fr->magic != FAULT_MAGIC) {
		shell_print(sh, "no fault record");
		return 0;
	}

	shell_print(sh, "FAULT: %s (reason %u) in thread '%s' at uptime %u ms",
		    reason_str(fr->reason), fr->reason, fr->thread, fr->uptime_ms);
	if (fr->esf_valid) {
		shell_print(sh, "  pc=%08x lr=%08x xpsr=%08x", fr->pc, fr->lr, fr->xpsr);
		shell_print(sh, "  r0=%08x r1=%08x r2=%08x r3=%08x r12=%08x", fr->r0, fr->r1,
			    fr->r2, fr->r3, fr->r12);
	} else {
		shell_print(sh, "  (no exception frame)");
	}
	shell_print(sh, "record persists until `resetinfo clear` or the next fault");
	return 0;
}
SHELL_CMD_ARG_REGISTER(resetinfo, NULL, "Show last reset cause + saved fault record [clear]",
		       cmd_resetinfo, 1, 1);
