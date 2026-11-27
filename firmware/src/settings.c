/*
 * Tiny persisted key/value store on the LittleFS volume. Format: one
 * `key=value\n` per line in /NAND:/.blikachu.cfg. The whole file lives
 * in a 512-byte buffer — plenty for device settings, and small enough
 * that rewriting the file on every set is a non-issue (the flash
 * driver's QMI-safe path makes writes safe mid-game too).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include "settings.h"
#include "storage.h"

#define SETTINGS_PATH    STORAGE_MOUNT_POINT "/.blikachu.cfg"
#define SETTINGS_TEMP    STORAGE_MOUNT_POINT "/.blikachu.tmp"
#define SETTINGS_BUF_MAX 512

static K_MUTEX_DEFINE(g_lock);
static char g_buf[SETTINGS_BUF_MAX];  /* current file content */
static char g_nbuf[SETTINGS_BUF_MAX]; /* rebuilt content for writes */

/* Read the whole file into g_buf (NUL-terminated). Returns content
 * length, 0 if the file doesn't exist, negative errno on I/O error.
 */
static ssize_t load_locked(void)
{
	struct fs_dirent ent;
	struct fs_file_t fp;

	g_buf[0] = '\0';
	/* Stat first — fs_open logs an ERR for a missing file. */
	int rc = fs_stat(SETTINGS_PATH, &ent);
	if (rc != 0) {
		return rc == -ENOENT ? 0 : rc;
	}
	fs_file_t_init(&fp);
	rc = fs_open(&fp, SETTINGS_PATH, FS_O_READ);

	if (rc) {
		return rc;
	}
	ssize_t rd = fs_read(&fp, g_buf, sizeof(g_buf) - 1);

	(void)fs_close(&fp);
	if (rd < 0) {
		return rd;
	}
	g_buf[rd] = '\0';
	return rd;
}

static int store_locked(const char *content, size_t len)
{
	struct fs_file_t fp;

	fs_file_t_init(&fp);
	int rc = fs_open(&fp, SETTINGS_TEMP, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc) {
		return rc;
	}
	ssize_t wr = fs_write(&fp, content, len);
	rc = wr == (ssize_t)len ? fs_sync(&fp) : -EIO;
	int close_rc = fs_close(&fp);
	if (!rc) {
		rc = close_rc;
	}
	return rc ? rc : fs_rename(SETTINGS_TEMP, SETTINGS_PATH);
}

/* Find the value of `key` inside g_buf. Returns pointer to the value
 * and its length, or NULL.
 */
static const char *find_locked(const char *key, size_t *vlen)
{
	size_t klen = strlen(key);

	for (const char *line = g_buf; *line != '\0';) {
		const char *nl = strchr(line, '\n');
		size_t llen = nl ? (size_t)(nl - line) : strlen(line);

		if (llen > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
			*vlen = llen - klen - 1;
			return line + klen + 1;
		}
		line += llen + (nl ? 1 : 0);
	}
	return NULL;
}

int settings_get_str(const char *key, char *out, size_t cap)
{
	int rc = -ENOENT;

	k_mutex_lock(&g_lock, K_FOREVER);
	if (load_locked() >= 0) {
		size_t vlen;
		const char *val = find_locked(key, &vlen);

		if (val) {
			if (vlen + 1 > cap) {
				rc = -EMSGSIZE;
			} else {
				memcpy(out, val, vlen);
				out[vlen] = '\0';
				rc = 0;
			}
		}
	}
	k_mutex_unlock(&g_lock);
	return rc;
}

/* Rebuild g_nbuf without `key`, then append `key=val` if val != NULL.
 * Caller holds the lock and has loaded g_buf.
 */
static int rewrite_locked(const char *key, const char *val)
{
	size_t klen = strlen(key);
	size_t n = 0;

	for (const char *line = g_buf; *line != '\0';) {
		const char *nl = strchr(line, '\n');
		size_t llen = nl ? (size_t)(nl - line) : strlen(line);
		bool is_key = llen > klen && strncmp(line, key, klen) == 0 && line[klen] == '=';

		if (!is_key && llen > 0) {
			if (n + llen + 1 >= sizeof(g_nbuf)) {
				return -ENOMEM;
			}
			memcpy(g_nbuf + n, line, llen);
			n += llen;
			g_nbuf[n++] = '\n';
		}
		line += llen + (nl ? 1 : 0);
	}

	if (val) {
		int need = snprintf(g_nbuf + n, sizeof(g_nbuf) - n, "%s=%s\n", key, val);

		if (need < 0 || (size_t)need >= sizeof(g_nbuf) - n) {
			return -ENOMEM;
		}
		n += need;
	}
	return store_locked(g_nbuf, n);
}

int settings_set_str(const char *key, const char *val)
{
	if (strchr(key, '=') || strchr(key, '\n') || strchr(val, '\n')) {
		return -EINVAL;
	}

	k_mutex_lock(&g_lock, K_FOREVER);
	ssize_t len = load_locked();
	int rc = (len < 0) ? (int)len : rewrite_locked(key, val);

	k_mutex_unlock(&g_lock);
	return rc;
}

int settings_delete(const char *key)
{
	k_mutex_lock(&g_lock, K_FOREVER);
	ssize_t len = load_locked();
	int rc = (len <= 0) ? (int)MIN(len, 0) : rewrite_locked(key, NULL);

	k_mutex_unlock(&g_lock);
	return rc;
}

int settings_get_int(const char *key, int fallback)
{
	char buf[16];

	if (settings_get_str(key, buf, sizeof(buf)) != 0) {
		return fallback;
	}
	char *end = NULL;
	long v = strtol(buf, &end, 10);

	return (end && end != buf) ? (int)v : fallback;
}

int settings_set_int(const char *key, int val)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", val);
	return settings_set_str(key, buf);
}

void settings_list(void (*out)(void *ctx, const char *line), void *ctx)
{
	char line[96];

	k_mutex_lock(&g_lock, K_FOREVER);
	if (load_locked() > 0) {
		for (const char *p = g_buf; *p != '\0';) {
			const char *nl = strchr(p, '\n');
			size_t llen = nl ? (size_t)(nl - p) : strlen(p);

			if (llen > 0) {
				snprintf(line, sizeof(line), "%.*s", (int)MIN(llen, sizeof(line) - 1),
					 p);
				out(ctx, line);
			}
			p += llen + (nl ? 1 : 0);
		}
	}
	k_mutex_unlock(&g_lock);
}
