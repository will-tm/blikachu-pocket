/*
 * WiFi provisioning: up to 8 stored networks, auto-connect, IP HUD.
 *
 * Persistence (settings.c keys wifi0..wifi7): "<psk-hex64>;<ssid>".
 * Per Will: the clear passphrase is NEVER stored — `wifi_cred add`
 * derives the WPA2 PSK once (PBKDF2-HMAC-SHA1(passphrase, ssid, 4096,
 * 32), hostap's own pbkdf2_sha1) and persists the hex PSK. Connects
 * pass the 64-char hex PSK to the supplicant, which hostap parses as
 * a raw PSK (same rule as wpa_supplicant.conf).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "hud.h"
#include "settings.h"

LOG_MODULE_REGISTER(wifi_prov, LOG_LEVEL_INF);

#define WIFI_SLOTS   8
#define PSK_HEX_LEN  64

/* hostap crypto (already linked via WIFI_NM_WPA_SUPPLICANT) */
extern int pbkdf2_sha1(const char *passphrase, const uint8_t *ssid, size_t ssid_len,
		       int iterations, uint8_t *buf, size_t buflen);
/* resetinfo.c — PBKDF2 (1.5 s CPU) + settings flash write outlast the
 * 1.9 s BL61x wdt window under load.
 */
extern void resetinfo_wdt_pause(void);
extern void resetinfo_wdt_resume(void);

static void slot_key(int i, char *out)
{
	snprintf(out, 8, "wifi%d", i);
}

/* Returns slot value split into psk/ssid; value buffer is caller's. */
static bool slot_get(int i, char *val, size_t cap, char **psk, char **ssid)
{
	char key[8];

	slot_key(i, key);
	if (settings_get_str(key, val, cap) != 0) {
		return false;
	}
	char *sep = strchr(val, ';');

	if (sep == NULL || (sep - val) != PSK_HEX_LEN) {
		return false;
	}
	*sep = '\0';
	*psk = val;
	*ssid = sep + 1;
	return true;
}

static int cmd_wifi_add(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh, "usage: wifi_cred add <ssid> <passphrase>");
		return -EINVAL;
	}
	const char *ssid = argv[1];
	const char *pass = argv[2];
	size_t plen = strlen(pass);

	if (plen < 8 || plen > 63) {
		shell_error(sh, "passphrase must be 8..63 chars");
		return -EINVAL;
	}

	uint8_t psk[32];
	char val[PSK_HEX_LEN + 1 + WIFI_SSID_MAX_LEN + 1];

	shell_print(sh, "deriving PSK (PBKDF2, 4096 iters)...");
	resetinfo_wdt_pause();
	if (pbkdf2_sha1(pass, (const uint8_t *)ssid, strlen(ssid), 4096, psk,
			sizeof(psk)) != 0) {
		resetinfo_wdt_resume();
		shell_error(sh, "PBKDF2 failed");
		return -EIO;
	}

	for (int i = 0; i < 32; i++) {
		snprintf(&val[i * 2], 3, "%02x", psk[i]);
	}
	snprintf(&val[PSK_HEX_LEN], sizeof(val) - PSK_HEX_LEN, ";%s", ssid);

	/* Reuse the slot holding this SSID, else the first free one. */
	int slot = -1;

	for (int i = 0; i < WIFI_SLOTS && slot < 0; i++) {
		char tmp[sizeof(val)];
		char *tpsk, *tssid;

		if (!slot_get(i, tmp, sizeof(tmp), &tpsk, &tssid)) {
			slot = i;
		} else if (strcmp(tssid, ssid) == 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		shell_error(sh, "all %d slots full — `wifi_cred del <n>` first", WIFI_SLOTS);
		return -ENOSPC;
	}

	char key[8];

	slot_key(slot, key);
	int rc = settings_set_str(key, val);

	resetinfo_wdt_resume();
	if (rc) {
		shell_error(sh, "persist failed: %d", rc);
		return rc;
	}
	shell_print(sh, "slot %d: \"%s\" (PSK stored, passphrase discarded)", slot, ssid);
	return 0;
}

static int cmd_wifi_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (int i = 0; i < WIFI_SLOTS; i++) {
		char val[PSK_HEX_LEN + 1 + WIFI_SSID_MAX_LEN + 1];
		char *psk, *ssid;

		if (slot_get(i, val, sizeof(val), &psk, &ssid)) {
			shell_print(sh, "%d: \"%s\" (psk %.4s...)", i, ssid, psk);
		}
	}
	return 0;
}

static int cmd_wifi_del(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: wifi_cred del <slot>");
		return -EINVAL;
	}
	char key[8];

	slot_key((int)strtol(argv[1], NULL, 10), key);
	settings_delete(key);
	shell_print(sh, "%s deleted", key);
	return 0;
}

/* ---- auto-connect ---- */

static struct net_mgmt_event_callback g_wifi_cb;
static struct net_mgmt_event_callback g_ipv4_cb;
static struct k_work_delayable g_connect_work;
static int g_slot = -1; /* slot currently being tried */
static char g_val[PSK_HEX_LEN + 1 + WIFI_SSID_MAX_LEN + 1];

static uint8_t g_psk_raw[WIFI_PSK_PBKDF2_KEY_LEN];

static void try_next_slot(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	char *psk, *ssid;

	if (iface == NULL) {
		return;
	}

	for (g_slot++; g_slot < WIFI_SLOTS; g_slot++) {
		if (!slot_get(g_slot, g_val, sizeof(g_val), &psk, &ssid)) {
			continue;
		}

		/* Zephyr's precomputed-PSK convention (see wifi_shell's
		 * PBKDF2 path): raw 32 bytes, psk_length = 32.
		 */
		size_t n = hex2bin(psk, PSK_HEX_LEN, g_psk_raw, sizeof(g_psk_raw));

		if (n != WIFI_PSK_PBKDF2_KEY_LEN) {
			LOG_WRN("wifi: slot %d has a malformed PSK", g_slot);
			continue;
		}

		struct wifi_connect_req_params params = {
			.ssid = (const uint8_t *)ssid,
			.ssid_length = strlen(ssid),
			.psk = g_psk_raw,
			.psk_length = WIFI_PSK_PBKDF2_KEY_LEN,
			.psk_is_pbkdf2 = true,
			.security = WIFI_SECURITY_TYPE_PSK,
			.channel = WIFI_CHANNEL_ANY,
			.band = WIFI_FREQ_BAND_UNKNOWN,
			.mfp = WIFI_MFP_OPTIONAL,
		};

		LOG_INF("wifi: trying slot %d \"%s\"", g_slot, ssid);
		if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params,
			     sizeof(params)) == 0) {
			return; /* result arrives via event */
		}
		LOG_WRN("wifi: connect request for slot %d failed", g_slot);
	}
	/* Walk exhausted (or nothing stored yet): try again later —
	 * the AP may be booting, the supplicant may have raced us, or
	 * creds may get added meanwhile.
	 */
	LOG_INF("wifi: no (more) stored networks — retry in 15 s");
	k_work_schedule(&g_connect_work, K_SECONDS(15));
}

static void connect_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	g_slot = -1;
	try_next_slot();
}

/* Both net_mgmt callbacks run on the tiny net_mgmt event thread —
 * defer real work (FATFS reads, HUD drawing through gfx) to the
 * system workqueue (8K stack).
 */
static void retry_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	try_next_slot();
}
static K_WORK_DEFINE(g_retry_work, retry_work_fn);

static char g_ip_str[NET_IPV4_ADDR_LEN];
static void ip_hud_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	hud_show_text(g_ip_str);
}
static K_WORK_DEFINE(g_ip_hud_work, ip_hud_work_fn);

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t evt,
			       struct net_if *iface)
{
	ARG_UNUSED(iface);
	if (evt == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *st = cb->info;

		if (st != NULL && st->status != 0 && g_slot >= 0) {
			LOG_WRN("wifi: slot %d failed (%d)", g_slot, st->status);
			k_work_submit(&g_retry_work);
		}
	} else if (evt == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		/* Link dropped (AP kick, roam, RF hiccup): walk the
		 * stored networks again after a short backoff.
		 */
		LOG_WRN("wifi: disconnected — reconnecting in 3 s");
		k_work_schedule(&g_connect_work, K_SECONDS(3));
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb, uint64_t evt,
			       struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (evt != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4 != NULL) {
		net_addr_ntop(AF_INET, &ipv4->unicast[0].ipv4.address.in_addr,
			      g_ip_str, sizeof(g_ip_str));
		LOG_INF("wifi: ip %s", g_ip_str);
		k_work_submit(&g_ip_hud_work);
	}
}

static int wifi_prov_init(void)
{
	net_mgmt_init_event_callback(&g_wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
				     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&g_wifi_cb);
	net_mgmt_init_event_callback(&g_ipv4_cb, ipv4_event_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&g_ipv4_cb);

	/* Give storage_mount/settings + the wifi iface time to come up. */
	k_work_init_delayable(&g_connect_work, connect_work_fn);
	k_work_schedule(&g_connect_work, K_SECONDS(4));
	return 0;
}
SYS_INIT(wifi_prov_init, APPLICATION, 95);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_wifi_cred,
	SHELL_CMD_ARG(add, NULL, "<ssid> <passphrase> — derive+store PSK", cmd_wifi_add, 3, 0),
	SHELL_CMD(list, NULL, "list stored networks", cmd_wifi_list),
	SHELL_CMD_ARG(del, NULL, "<slot> — delete stored network", cmd_wifi_del, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(wifi_cred, &sub_wifi_cred,
		   "Stored WiFi networks (PSK only, no passphrases)", NULL);
