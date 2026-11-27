/*
 * Embedded WebUI (Zephyr native http_server): ROM manager v1.
 *  GET  /            — single-page UI (drag'n'drop upload, list, delete)
 *  GET  /api/roms    — JSON list of /NAND: files
 *  POST /api/upload?name=<file> — raw body streamed to /NAND:/<file>
 *  POST /api/delete?name=<file> — delete
 *
 * Remote play (WebSocket framebuffer + input) and OTA land next — see
 * TODO.md WiFi roadmap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/net/websocket.h>
#include <zephyr/net/socket.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include "emu.h"

#include "psram_buf.h"
#include "storage.h"
#include "webui.h"
#include "libretro/workspace.h"

LOG_MODULE_REGISTER(webui, LOG_LEVEL_INF);

static uint16_t webui_port = 80;

/* ---- static UI ---- */

/* GitHub-dark look shared by all pages (Will's metrics.html palette) */
#define PIKA_FAVICON "<link rel=icon href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAERlWElmTU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKACAAQAAAABAAAAIKADAAQAAAABAAAAIAAAAACshmLzAAADBUlEQVRYCe1UW0gUYRQ+s5dx3ZvuqrOy6tK6pm43NROsSKwoLc2gMBAkutDlwaCwHuuph4LIzCARkh6CoDAQrMxVnyQlM8lb6y13zV0dtXXX3dG9OPs3EwohaP+Cj3tgOMw535zvm+/8MwDhCDsQdiDsAL4DhFwuvw8gj8N/ZGuRBq02yUupVOUbjBVuUN+0LNi0+09TIpIk3SmTRlRfFOSulsV8risVZ969mVOVn39kIDXV+IwriVb7WAkbLFUSY4W7XdOsV1oy1lJmFItntIHF4WGVniqwu2Mkpq5ZdnnZywtYqT0NO643whCWglBA9RXqBv/EOYTYe6ijuRy1vjuMRj/n+UtPGbizIc7kZ/W/1F+mvx1l2h7oruDMxl7Brf0Q2TNNZY1bF8A10gEWqwVs5gB4fvqYyfbxGgSBvqmm9Ecp+em1VBol1RqpiqvZ8HdNmwnBFrAyp8y4cVysD9JWoL+MQnEWA8fm3eB9Y492JkJWX53+YUKusVIgU4iADYJMo9yTmwzbNyPne9hnYH6anbBPTn/PS5dmBFYEgNy/AfYiiNEHUQkRWZwQ57/GTowAy32lrWZEfzLZa+QAY1sm4DXD0OR4lOmQMjYDLTgAWASklgBDdiJBDEYqf4wv1if5nBeq3loeVzcwzzniyf+R833sFfDYonxN8hKpBqRLAz/3pqxGD4J4HaSmRatfNc0Osf4VUrZM9HJYLPKQBdCTi5kCCILTyYLNSUJQogDmFw0qcrloXwo86frq+tBr843yg3ED+wycL1TkZGXGxNMWJ8h1GnB4WIgYm4MEQxTsinWj250Ch5D1kh/NwDuAHdgCoiXoxMGdSMqI1CBTk6BRqcDn9AC4HaDXCAlKIzF3DCxVcswEdyFcBdj/720ykYtk3Zd0QofQ1GKl+3tmbDLfYoxKxBBdve65xg7f2W4LO4BLvIbDd0Ajo7qHAkPvm+n2us7gU27A1Bmj6MDJAsULsw152gb9Ie1+TUCombd3XShiKSrKsK4Yvg07EHYg7AC2A38ApD8VeIns+KIAAAAASUVORK5CYII='>"
#define WEBUI_STYLE \
"<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>" \
PIKA_FAVICON \
"<style>:root{--bg:#0d1117;--bg2:#161b22;--border:#30363d;--fg:#c9d1d9;--muted:#8b949e;" \
"--green:#7ee787;--blue:#79c0ff;--yellow:#d29922;--red:#f85149}" \
"*{box-sizing:border-box}body{background:var(--bg);color:var(--fg);" \
"font:13px/1.5 ui-monospace,Menlo,Consolas,monospace;margin:0}" \
"header{display:flex;gap:1em;align-items:center;padding:8px 14px;background:var(--bg2);" \
"border-bottom:1px solid var(--border);position:sticky;top:0}" \
"header h1{font-size:13px;margin:0;font-weight:600}header img{width:22px;height:22px}" \
"header .spacer{flex:1}header a{color:var(--blue);text-decoration:none;font-size:12px}" \
"#st{font-size:11px;color:var(--muted)}#st.ok{color:var(--green)}#st.err{color:var(--red)}" \
"main{padding:12px;display:grid;gap:12px;max-width:760px;margin:0 auto}" \
".card{background:var(--bg2);border:1px solid var(--border);border-radius:6px;padding:12px}" \
".card h2{font-size:11px;font-weight:600;letter-spacing:.04em;text-transform:uppercase;" \
"color:var(--muted);margin:0 0 10px}" \
"table{width:100%;border-collapse:collapse}td{padding:5px 6px;border-top:1px solid var(--border)}" \
"td.sz{color:var(--muted);text-align:right;white-space:nowrap}" \
"button{background:#21262d;color:var(--fg);border:1px solid var(--border);border-radius:6px;" \
"padding:3px 10px;font:inherit;cursor:pointer}button:hover{border-color:var(--muted)}" \
"button.play{color:var(--green)}button.del{color:var(--red)}button.warn{color:var(--yellow)}" \
"#drop{border:1px dashed var(--border);border-radius:6px;padding:18px;text-align:center;" \
"color:var(--muted)}#drop.over{border-color:var(--blue);color:var(--blue)}" \
"canvas{width:480px;max-width:100%;image-rendering:pixelated;display:block;margin:0 auto;" \
"border:1px solid var(--border);border-radius:4px}" \
"input[type=file]{display:none}.bar{height:6px;background:#21262d;border-radius:3px;overflow:hidden;margin-top:8px}" \
".bar i{display:block;height:100%;background:var(--blue);width:0}</style>"

#define WEBUI_HEADER(page) \
"<header><img src='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAAXNSR0IArs4c6QAAAERlWElmTU0AKgAAAAgAAYdpAAQAAAABAAAAGgAAAAAAA6ABAAMAAAABAAEAAKACAAQAAAABAAAAIKADAAQAAAABAAAAIAAAAACshmLzAAADBUlEQVRYCe1UW0gUYRQ+s5dx3ZvuqrOy6tK6pm43NROsSKwoLc2gMBAkutDlwaCwHuuph4LIzCARkh6CoDAQrMxVnyQlM8lb6y13zV0dtXXX3dG9OPs3EwohaP+Cj3tgOMw535zvm+/8MwDhCDsQdiDsAL4DhFwuvw8gj8N/ZGuRBq02yUupVOUbjBVuUN+0LNi0+09TIpIk3SmTRlRfFOSulsV8risVZ969mVOVn39kIDXV+IwriVb7WAkbLFUSY4W7XdOsV1oy1lJmFItntIHF4WGVniqwu2Mkpq5ZdnnZywtYqT0NO643whCWglBA9RXqBv/EOYTYe6ijuRy1vjuMRj/n+UtPGbizIc7kZ/W/1F+mvx1l2h7oruDMxl7Brf0Q2TNNZY1bF8A10gEWqwVs5gB4fvqYyfbxGgSBvqmm9Ecp+em1VBol1RqpiqvZ8HdNmwnBFrAyp8y4cVysD9JWoL+MQnEWA8fm3eB9Y492JkJWX53+YUKusVIgU4iADYJMo9yTmwzbNyPne9hnYH6anbBPTn/PS5dmBFYEgNy/AfYiiNEHUQkRWZwQ57/GTowAy32lrWZEfzLZa+QAY1sm4DXD0OR4lOmQMjYDLTgAWASklgBDdiJBDEYqf4wv1if5nBeq3loeVzcwzzniyf+R833sFfDYonxN8hKpBqRLAz/3pqxGD4J4HaSmRatfNc0Osf4VUrZM9HJYLPKQBdCTi5kCCILTyYLNSUJQogDmFw0qcrloXwo86frq+tBr843yg3ED+wycL1TkZGXGxNMWJ8h1GnB4WIgYm4MEQxTsinWj250Ch5D1kh/NwDuAHdgCoiXoxMGdSMqI1CBTk6BRqcDn9AC4HaDXCAlKIzF3DCxVcswEdyFcBdj/720ykYtk3Zd0QofQ1GKl+3tmbDLfYoxKxBBdve65xg7f2W4LO4BLvIbDd0Ajo7qHAkPvm+n2us7gU27A1Bmj6MDJAsULsw152gb9Ie1+TUCombd3XShiKSrKsK4Yvg07EHYg7AC2A38ApD8VeIns+KIAAAAASUVORK5CYII='>" \
"<h1>BLikachu Pocket</h1><span id=st>" page "</span><span class=spacer></span>" \
"<a href='/'>roms</a><a href='/ota'>ota</a></header>"

static const char index_html[] =
"<!doctype html><title>BLikachu Pocket</title>" WEBUI_STYLE
WEBUI_HEADER("rom manager")
"<main>"
"<div class=card><h2>roms</h2><table id=list></table></div>"
"<div class=card><h2>upload</h2>"
"<div id=drop>drop GB / GBC / NES / SNES files here or <button onclick=\"document.getElementById('f').click()\">browse</button>"
"<input id=f type=file multiple accept='.gb,.gbc,.nes,.sfc,.smc'>"
"<div class=bar><i id=pb></i></div><div id=msg></div></div></div>"
"<script>"
"const L=document.getElementById('list'),M=document.getElementById('msg'),"
"S=document.getElementById('st'),PB=document.getElementById('pb');"
"function fmt(n){return n>1048576?(n/1048576).toFixed(1)+' MB':(n/1024|0)+' KB'}"
"async function refresh(){try{const d=await(await fetch('/api/roms')).json();"
"S.textContent=`${d.roms.length} roms — ${fmt(d.free)} free`;S.className='ok';"
"L.innerHTML=d.roms.map(f=>`<tr><td>${f.name}</td><td class=sz>${fmt(f.size)}</td>"
"<td style='text-align:right;white-space:nowrap'>"
"<button class=play onclick='play(\"${encodeURIComponent(f.name)}\")'>&#9654; play</button> "
"<button class=del onclick='del(\"${encodeURIComponent(f.name)}\")'>&#10005;</button></td></tr>`).join('')"
"}catch(e){S.textContent='offline';S.className='err'}}"
"async function del(n){await fetch('/api/delete?name='+n,{method:'POST'});refresh()}"
"async function play(n){M.textContent='launching...';"
"const r=await fetch('/api/run?name='+n,{method:'POST'});"
"if(r.ok)location.href='/play';else M.textContent='launch failed'}"
"function upload(url,file,bar,cb){const x=new XMLHttpRequest();x.open('POST',url);"
"x.upload.onprogress=e=>{bar.style.width=(100*e.loaded/e.total)+'%'};"
"x.onload=()=>cb(x.status==200&&x.responseText=='ok');x.onerror=()=>cb(false);x.send(file)}"
"async function up(files){for(const f of files){let ok=false;"
"for(let a=1;a<=3&&!ok;a++){M.textContent=`uploading ${f.name} (try ${a})...`;"
"ok=await new Promise(r=>upload('/api/upload?name='+encodeURIComponent(f.name),f,PB,r))}"
"M.textContent=ok?'done':'FAILED: '+f.name;PB.style.width=0;refresh()}refresh()}"
"const D=document.getElementById('drop');"
"D.ondragover=e=>{e.preventDefault();D.classList.add('over')};"
"D.ondragleave=()=>D.classList.remove('over');"
"D.ondrop=e=>{e.preventDefault();D.classList.remove('over');up(e.dataTransfer.files)};"
"document.getElementById('f').onchange=e=>up(e.target.files);"
"refresh();setInterval(refresh,10000)"
"</script></main>";

static const char ota_html[] =
"<!doctype html><title>BLikachu Pocket OTA</title>" WEBUI_STYLE
WEBUI_HEADER("firmware update")
"<main><div class=card><h2>ota firmware update</h2>"
"<p id=fw-info>choose a whole_img.bin, then flash. the device verifies, "
"reboots itself and is back on wifi in ~10 s.</p>"
"<button onclick=\"document.getElementById('fw').click()\">choose image</button> "
"<button class=warn id=go style='display:none' onclick='otaGo()'>flash it</button>"
"<input id=fw type=file accept='.bin' style='display:none'>"
"<div class=bar><i id=fpb></i></div><div id=fmsg></div></div>"
"<script>"
"const S=document.getElementById('st');S.textContent='ready';S.className='ok';"
"function fmt(n){return n>1048576?(n/1048576).toFixed(1)+' MB':(n/1024|0)+' KB'}"
"function upload(url,file,bar,cb){const x=new XMLHttpRequest();x.open('POST',url);"
"x.upload.onprogress=e=>{bar.style.width=(100*e.loaded/e.total)+'%'};"
"x.onload=()=>cb(x.status==200&&x.responseText=='ok');x.onerror=()=>cb(false);x.send(file)}"
"let fwFile=null;const FM=document.getElementById('fmsg'),FPB=document.getElementById('fpb');"
"document.getElementById('fw').onchange=e=>{fwFile=e.target.files[0];"
"document.getElementById('fw-info').textContent=`${fwFile.name} (${fmt(fwFile.size)})`;"
"document.getElementById('go').style.display=''};"
"async function otaGo(){if(!confirm('flash '+fwFile.name+' and reboot?'))return;"
"FM.textContent='staging...';"
"let ok=false;for(let a=1;a<=3&&!ok;a++)"
"ok=await new Promise(r=>upload('/api/upload?name=update.bin',fwFile,FPB,r));"
"if(!ok){FM.textContent='staging failed';return}"
"FM.textContent='flashing — device reboots, back shortly...';"
"await fetch('/api/ota',{method:'POST'});setTimeout(()=>location.href='/',30000)}"
"</script></main>";

static struct http_resource_detail_static ota_page_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/html",
	},
	.static_data = ota_html,
	.static_data_len = sizeof(ota_html) - 1,
};

static struct http_resource_detail_static index_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/html",
	},
	.static_data = index_html,
	.static_data_len = sizeof(index_html) - 1,
};

/* ---- helpers ---- */

/* "?name=foo%20bar.gb" -> "foo bar.gb" (tiny URL-decode) */
static bool query_name(struct http_client_ctx *client, char *out, size_t cap)
{
	const char *q = strchr(client->url_buffer, '?');

	if (q == NULL || strncmp(q + 1, "name=", 5) != 0) {
		return false;
	}
	q += 6;
	size_t o = 0;

	while (*q != '\0' && *q != '&' && o + 1 < cap) {
		if (*q == '%' && q[1] != '\0' && q[2] != '\0') {
			char hx[3] = {q[1], q[2], '\0'};

			out[o++] = (char)strtol(hx, NULL, 16);
			q += 3;
		} else if (*q == '+') {
			out[o++] = ' ';
			q++;
		} else {
			out[o++] = *q++;
		}
	}
	out[o] = '\0';
	/* No path tricks. */
	return o > 0 && strstr(out, "..") == NULL && strchr(out, '/') == NULL;
}

/* ---- GET /api/roms ---- */

static BLIKACHU_BIGBUF char g_json[1536];

static int roms_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	struct fs_dir_t dir;
	struct fs_dirent ent;
	size_t o = 0;

	fs_dir_t_init(&dir);
	struct fs_statvfs sv = {0};

	(void)fs_statvfs(STORAGE_MOUNT_POINT, &sv);
	o += snprintf(g_json + o, sizeof(g_json) - o, "{\"free\":%llu,\"roms\":[",
		      (unsigned long long)sv.f_bfree * sv.f_frsize);
	if (fs_opendir(&dir, STORAGE_MOUNT_POINT) == 0) {
		bool first = true;

		while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
			if (ent.type != FS_DIR_ENTRY_FILE || !emu_content_extension_supported(ent.name)) {
				continue;
			}
			if (o + strlen(ent.name) + 48 > sizeof(g_json)) {
				break;
			}
			o += snprintf(g_json + o, sizeof(g_json) - o,
				      "%s{\"name\":\"%s\",\"size\":%u}",
				      first ? "" : ",", ent.name, (unsigned)ent.size);
			first = false;
		}
		fs_closedir(&dir);
	}
	o += snprintf(g_json + o, sizeof(g_json) - o, "]}");

	response_ctx->body = (uint8_t *)g_json;
	response_ctx->body_len = o;
	response_ctx->final_chunk = true;
	return 0;
}

/* ---- POST /api/upload?name= ---- */

/* Uploads stage into PSRAM and hit FATFS once at the end — 256-byte
 * HTTP chunks straight to fs_write mean thousands of read-modify-
 * write flash cycles (minutes for a 1 MB ROM).
 */
#define UPLOAD_MAX LR_STAGING_BYTES
#define g_up_buf ((uint8_t *)lr_workspace_data())
uint8_t *webui_staging_buf(size_t *cap)
{
	*cap = UPLOAD_MAX;
	return g_up_buf;
}

uint8_t *webui_staging_claim(size_t *cap)
{
	emu_quit();
	emu_wait();
	uint8_t *buf = lr_workspace_claim(LR_WORKSPACE_STAGING);

	*cap = buf ? UPLOAD_MAX : 0;
	return buf;
}

void webui_staging_release(void)
{
	if (!lr_workspace_release(LR_WORKSPACE_STAGING)) {
		LOG_ERR("staging release without ownership");
	}
}

static char g_up_path[96];
static bool g_up_active;
static size_t g_up_len;
static const void *g_up_client; /* owner of the in-flight upload */

/* resetinfo.c: the BL61x wdt tops out at 1.9 s; multi-MB FATFS
 * commits and big chain unlinks legitimately run longer.
 */
extern void resetinfo_wdt_pause(void);
extern void resetinfo_wdt_resume(void);

static int up_commit(void)
{
	struct fs_file_t f;
	int rc = -EIO;

	struct fs_statvfs sv;

	/* Boot mount is still flaky after tool/OTA resets — rescue
	 * here rather than failing the user's upload.
	 */
	if (fs_statvfs(STORAGE_MOUNT_POINT, &sv) != 0) {
		LOG_WRN("commit: /NAND: not mounted — mounting now");
		(void)storage_mount();
	}

	/* Preflight: a commit that hits ENOSPC mid-write strands the
	 * partially-allocated cluster chain (lost clusters, only mkfs
	 * reclaims them) — refuse instead.
	 */
	if (fs_statvfs(STORAGE_MOUNT_POINT, &sv) == 0 &&
	    (uint64_t)sv.f_bfree * sv.f_frsize < g_up_len + (64 * 1024)) {
		LOG_WRN("commit: need %zu, free %llu — refusing", g_up_len,
			(unsigned long long)sv.f_bfree * sv.f_frsize);
		return -ENOSPC;
	}

	resetinfo_wdt_pause();
	fs_file_t_init(&f);
	/* Only unlink what exists — a blind unlink makes the fs layer
	 * log a scary-looking ENOENT for every fresh filename.
	 */
	{
		struct fs_dirent ent;

		if (fs_stat(g_up_path, &ent) == 0) {
			(void)fs_unlink(g_up_path);
		}
	}
	if (fs_open(&f, g_up_path, FS_O_CREATE | FS_O_WRITE) == 0) {
		size_t off = 0;

		rc = 0;
		while (off < g_up_len && rc == 0) {
			size_t chunk = MIN((size_t)(64 * 1024), g_up_len - off);
			ssize_t wr = -1;

			/* Flash writes can transiently fail while the radio
			 * is still busy right after the transfer — retry
			 * before declaring the commit dead.
			 */
			for (int attempt = 0; attempt < 3; attempt++) {
				wr = fs_write(&f, &g_up_buf[off], chunk);
				if (wr == (ssize_t)chunk) {
					break;
				}
				LOG_WRN("commit: write @%zu rc=%d (attempt %d)",
					off, (int)wr, attempt + 1);
				if (wr > 0) {
					off += wr; /* partial: continue after it */
					chunk -= wr;
				}
				k_sleep(K_MSEC(100));
			}
			if (wr != (ssize_t)chunk) {
				rc = -EIO;
			}
			off += chunk;
			/* Breathe between bursts: let the radio/feeder run
			 * and cap the sustained flash current.
			 */
			k_sleep(K_MSEC(20));
		}
		fs_close(&f);
	}
	if (rc != 0) {
		(void)fs_unlink(g_up_path);
	}
	resetinfo_wdt_resume();
	LOG_INF("upload: commit %s (%zu bytes) rc=%d", g_up_path, g_up_len, rc);
	return rc;
}

static int upload_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		if (g_up_active && client == g_up_client) {
			g_up_active = false;
			webui_staging_release();
		}
		return 0;
	}
	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}

	/* A different client means the previous transfer died without a
	 * clean ABORT (zombie slot) — its staged state is garbage.
	 */
	if (g_up_active && client != g_up_client) {
		LOG_WRN("upload: stale transfer dropped (%zu bytes)", g_up_len);
		g_up_active = false;
		webui_staging_release();
	}

	if (!g_up_active) {
		char name[64];

		if (!query_name(client, name, sizeof(name))) {
			return -EINVAL;
		}
		size_t cap;

		if (webui_staging_claim(&cap) == NULL) {
			return -EBUSY;
		}
		g_up_client = client;
		snprintf(g_up_path, sizeof(g_up_path), STORAGE_MOUNT_POINT "/%s", name);
		g_up_active = true;
		g_up_len = 0;
		LOG_INF("upload: %s", g_up_path);
	}

	if (request_ctx->data_len > 0) {
		if (g_up_len + request_ctx->data_len > UPLOAD_MAX) {
			g_up_active = false;
			webui_staging_release();
			return -EFBIG;
		}
		memcpy(&g_up_buf[g_up_len], request_ctx->data, request_ctx->data_len);
		if ((g_up_len / (128 * 1024)) !=
		    ((g_up_len + request_ctx->data_len) / (128 * 1024))) {
			LOG_INF("upload: %zu KB", (g_up_len + request_ctx->data_len) / 1024);
		}
		g_up_len += request_ctx->data_len;
	}

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		/* Commit synchronously (64K bursts + breathers so the
		 * watchdog feeder and the radio get scheduled between
		 * the IRQ-locked flash windows) — "ok" means the file
		 * is really on disk.
		 */
		int rc = up_commit();
		static char cmsg[24];

		g_up_active = false;
		webui_staging_release();
		snprintf(cmsg, sizeof(cmsg), rc == 0 ? "ok" : "err:%d", rc);
		response_ctx->body = (uint8_t *)cmsg;
		response_ctx->body_len = strlen(cmsg);
		response_ctx->final_chunk = true;
	}
	return 0;
}

/* ---- POST /api/delete?name= ---- */

static int delete_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	char name[64];
	char path[96];

	if (!query_name(client, name, sizeof(name))) {
		return -EINVAL;
	}
	storage_guard_lock();
	if (lr_workspace_core_owned()) {
		storage_guard_unlock();
		return -EBUSY;
	}
	snprintf(path, sizeof(path), STORAGE_MOUNT_POINT "/%s", name);
	resetinfo_wdt_pause();
	int rc = fs_unlink(path);

	resetinfo_wdt_resume();
	storage_guard_unlock();

	LOG_INF("delete %s: %d", path, rc);
	response_ctx->body = (uint8_t *)(rc == 0 ? "ok" : "err");
	response_ctx->body_len = rc == 0 ? 2 : 3;
	response_ctx->final_chunk = true;
	return 0;
}

/* ---- remote play: /play page + /ws frame/input socket ---- */

static const char play_html[] =
"<!doctype html><title>BLikachu Pocket Play</title>" WEBUI_STYLE
WEBUI_HEADER("connecting...")
"<main><div class=card>"
"<canvas id=c width=240 height=216></canvas>"
"<p style='text-align:center;color:var(--muted)'>arrows = d-pad &nbsp; Z=A &nbsp; X=B &nbsp; "
"Enter=START &nbsp; Shift=SELECT &nbsp; A=X S=Y Q=L W=R</p></div>"
"<script>"
"const cv=document.getElementById('c'),cx=cv.getContext('2d'),"
"st=document.getElementById('st'),im=cx.createImageData(240,216);"
"let ws,t0=0,n=0,retry=0;"
"function connect(){ws=new WebSocket(`ws://${location.host}/ws`);ws.binaryType='arraybuffer';"
"ws.onopen=()=>{retry=0;st.textContent='live';st.className='ok'};"
"ws.onclose=()=>{st.textContent=`reconnecting (${++retry})...`;st.className='err';"
"setTimeout(connect,Math.min(500*retry,3000))};"
"ws.onmessage=e=>{const u=new Uint8Array(e.data);"
"if(u[0]==0xAA){aud(u.subarray(1));return}"
"if(u[0]!==3||u.length!==103681)return;const px=im.data;"
"for(let p=0,i=1;i<u.length;i+=2){const rgb=(u[i]<<8)|u[i+1];"
"px[p++]=(rgb>>11&31)<<3;px[p++]=(rgb>>5&63)<<2;px[p++]=(rgb&31)<<3;px[p++]=255}"
"cx.putImageData(im,0,0);n++;"
"if(!t0)t0=performance.now();"
"if(n%60==0)st.textContent=`live ${(n*1000/(performance.now()-t0)).toFixed(1)} fps`}};"
"const KM={ArrowUp:4,ArrowDown:5,ArrowLeft:6,ArrowRight:7,z:8,x:0,a:9,s:1,q:10,w:11,Enter:3,Shift:2};"
"function key(e,down){const m=KM[e.key];if(m===undefined)return;e.preventDefault();"
"if(ws.readyState==1)ws.send(new Uint8Array([0x4b,m,down?1:0]))}"
"onkeydown=e=>{if(!e.repeat)key(e,1)};onkeyup=e=>key(e,0);"
"let ac,at=0;"
"function aud(pcm){if(!ac){try{ac=new AudioContext()}catch(e){return}}"
"if(ac.state=='suspended')ac.resume();"
"const b=ac.createBuffer(1,pcm.length,10000),d=b.getChannelData(0);"
"for(let i=0;i<pcm.length;i++)d[i]=(pcm[i]-128)/128;"
"const src=ac.createBufferSource();src.buffer=b;src.connect(ac.destination);"
"if(at<ac.currentTime)at=ac.currentTime+0.05;"
"src.start(at);at+=b.duration}"
"connect();"
"</script></main>";

static struct http_resource_detail_static play_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/html",
	},
	.static_data = play_html,
	.static_data_len = sizeof(play_html) - 1,
};

#define WS_RAW_BYTES (240 * 216 * 2)
static BLIKACHU_BIGBUF uint16_t g_ws_rgb[240 * 216];
static uint8_t g_ws_rxbuf[128];
static BLIKACHU_BIGBUF uint8_t g_ws_audio[1 + 4096];
static int g_ws_sock = -1;
static struct k_sem g_ws_go;

static void ws_play_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	k_sem_init(&g_ws_go, 0, 1);
	for (;;) {
		k_sem_take(&g_ws_go, K_FOREVER);
		int sock = g_ws_sock;

		if (sock < 0) {
			continue;
		}
		LOG_INF("play: client on sock %d", sock);
		emu_audio_tap_enable(true);
		/* WiFi power-save batches traffic to ~beacon intervals —
		 * that (not bandwidth) caps the frame rate. Latency mode
		 * while a player is connected, PS back on after.
		 */
		{
			struct net_if *ifc = net_if_get_first_wifi();
			struct wifi_ps_params ps = { .enabled = WIFI_PS_DISABLED };

			if (ifc != NULL) {
				(void)net_mgmt(NET_REQUEST_WIFI_PS, ifc, &ps, sizeof(ps));
			}
		}
		/* Server-push: frames go out on our own clock — the old
		 * request-reply shape paid a full RTT per frame and capped
		 * everything at ~5 fps regardless of frame size. The recv
		 * doubles as the pacer (keys arrive instantly, timeout
		 * paces pushes; consecutive timeouts are fine).
		 */
		int idle_errs = 0;

		for (;;) {
			uint32_t msg_type = 0;
			uint64_t remaining;
			int len = websocket_recv_msg(sock, g_ws_rxbuf, sizeof(g_ws_rxbuf),
						     &msg_type, &remaining, 15);

			if (len == -EAGAIN || len == -ETIMEDOUT || len == -EWOULDBLOCK) {
				len = 0; /* no input this tick — still push */
			} else if (len < 0) {
				if (++idle_errs > 3) {
					break;
				}
				len = 0;
			} else {
				idle_errs = 0;
			}
			if ((msg_type & WEBSOCKET_FLAG_CLOSE) != 0) {
				break;
			}
			if ((msg_type & WEBSOCKET_FLAG_PING) != 0) {
				(void)websocket_send_msg(sock, g_ws_rxbuf, len,
							 WEBSOCKET_OPCODE_PONG, false,
							 true, 2000);
				continue;
			}
			if (len >= 3 && g_ws_rxbuf[0] == 'K') {
				emu_retro_button(g_ws_rxbuf[1], g_ws_rxbuf[2] != 0);
				continue;
			}
			{

				/* audio as its own message: measured faster
				 * than merging (smaller video msgs win).
				 */
				g_ws_audio[0] = 0xAA;
				{
					size_t a = emu_audio_tap_read(&g_ws_audio[1],
								      sizeof(g_ws_audio) - 1);

					if (a > 0) {
						(void)websocket_send_msg(sock, g_ws_audio, 1 + a,
									 WEBSOCKET_OPCODE_DATA_BINARY,
									 false, true, 2000);
					}
				}

				if (emu_frame_snapshot(g_ws_rgb) != 0) {
					memset(g_ws_rgb, 0, sizeof(g_ws_rgb));
				}
				/* One binary websocket message, with an RGB565 format marker. */
				uint8_t mode = 3;
				int src = websocket_send_msg(sock, &mode, 1, WEBSOCKET_OPCODE_DATA_BINARY,
							     false, false, 5000);
				if (src >= 0) {
					src = websocket_send_msg(sock, (uint8_t *)g_ws_rgb, sizeof(g_ws_rgb),
								 WEBSOCKET_OPCODE_CONTINUE, false, true, 5000);
				}
				if (src < 0) {
					break;
				}
			}
		}
		emu_audio_tap_enable(false);
		{
			struct net_if *ifc = net_if_get_first_wifi();
			struct wifi_ps_params ps = { .enabled = WIFI_PS_ENABLED };

			if (ifc != NULL) {
				(void)net_mgmt(NET_REQUEST_WIFI_PS, ifc, &ps, sizeof(ps));
			}
		}
		LOG_INF("play: client gone");
		(void)websocket_unregister(sock);
		/* only clear if a takeover hasn't already replaced us */
		if (g_ws_sock == sock) {
			g_ws_sock = -1;
		} else if (g_ws_sock >= 0) {
			k_sem_give(&g_ws_go); /* serve the new client */
		}
	}
}
K_THREAD_DEFINE(ws_play_tid, 2048, ws_play_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(11), 0, 0);

static int ws_play_setup(int sock, struct http_request_ctx *request_ctx, void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);
	/* Single player: a new connection TAKES OVER — closing the old
	 * socket unblocks the streamer thread mid-recv/send. Refusing
	 * with -EBUSY instead created a permanent jam once a zombie
	 * session held the slot (browser reconnect spiral).
	 */
	int old = g_ws_sock;

	if (old >= 0) {
		/* shutdown (not close!) — unblocks the streamer's recv;
		 * it then unregisters/closes the fd it owns. Closing here
		 * yanked an fd the server core still referenced.
		 */
		LOG_INF("play: kicking stale client (sock %d)", old);
		(void)zsock_shutdown(old, ZSOCK_SHUT_RDWR);
	}
	g_ws_sock = sock;
	k_sem_give(&g_ws_go);
	return 0;
}

static uint8_t g_ws_upgrade_buf[256];

static struct http_resource_detail_websocket ws_play_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_WEBSOCKET,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = ws_play_setup,
	.data_buffer = g_ws_upgrade_buf,
	.data_buffer_len = sizeof(g_ws_upgrade_buf),
};

/* Launch a ROM (webui play button). */
static int run_handler(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *request_ctx,
		       struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}

	char name[64];

	if (!query_name(client, name, sizeof(name))) {
		return -EINVAL;
	}
	if (emu_shell_launch(name) != 0) {
		return -EIO;
	}
	response_ctx->body = (uint8_t *)"ok";
	response_ctx->body_len = 2;
	response_ctx->final_chunk = true;
	return 0;
}

/* OTA: respond first, then flash the staged /NAND:/update.bin after a
 * short delay (the blast never returns — it ends in a chip reset).
 */
extern int ota_apply_file(const char *path);

static void ota_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	int rc = ota_apply_file(STORAGE_MOUNT_POINT "/update.bin");

	LOG_ERR("ota: apply failed rc=%d", rc); /* only reached on error */
}
static K_WORK_DELAYABLE_DEFINE(g_ota_work, ota_work_fn);

static int ota_handler(struct http_client_ctx *client, enum http_transaction_status status,
		       const struct http_request_ctx *request_ctx,
		       struct http_response_ctx *response_ctx, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	k_work_schedule(&g_ota_work, K_MSEC(800));
	response_ctx->body = (uint8_t *)"ok";
	response_ctx->body_len = 2;
	response_ctx->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic run_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
	},
	.cb = run_handler,
};

static struct http_resource_detail_dynamic ota_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
	},
	.cb = ota_handler,
};

static struct http_resource_detail_dynamic roms_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = roms_handler,
};

static struct http_resource_detail_dynamic upload_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST) | BIT(HTTP_PUT),
	},
	.cb = upload_handler,
};

static struct http_resource_detail_dynamic delete_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
	},
	.cb = delete_handler,
};

HTTP_SERVICE_DEFINE(webui, "0.0.0.0", &webui_port, 1, 2, NULL, NULL, NULL);

HTTP_RESOURCE_DEFINE(webui_index, webui, "/", &index_detail);
HTTP_RESOURCE_DEFINE(webui_roms, webui, "/api/roms", &roms_detail);
HTTP_RESOURCE_DEFINE(webui_upload, webui, "/api/upload*", &upload_detail);
HTTP_RESOURCE_DEFINE(webui_delete, webui, "/api/delete*", &delete_detail);
HTTP_RESOURCE_DEFINE(webui_play, webui, "/play", &play_detail);
HTTP_RESOURCE_DEFINE(webui_ws, webui, "/ws", &ws_play_detail);
HTTP_RESOURCE_DEFINE(webui_run, webui, "/api/run*", &run_detail);
HTTP_RESOURCE_DEFINE(webui_ota, webui, "/api/ota", &ota_detail);
HTTP_RESOURCE_DEFINE(webui_otapage, webui, "/ota", &ota_page_detail);

static int webui_init(void)
{
	http_server_start();
	return 0;
}
SYS_INIT(webui_init, APPLICATION, 96);
