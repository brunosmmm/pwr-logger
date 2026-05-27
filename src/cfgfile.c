#include "cfgfile.h"
#include "config.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(cfgfile, LOG_LEVEL_INF);

/* Plenty of headroom for the current ~22-key config -- each line is at most
 * 32 chars (longest key + " = " + 4-char value). 2 KB fits the entire file. */
#define CFGFILE_MAX_BYTES   2048u

static char *skip_ws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

static void rstrip(char *p)
{
	size_t n = strlen(p);
	while (n > 0 && (p[n-1] == ' ' || p[n-1] == '\t' ||
			 p[n-1] == '\r' || p[n-1] == '\n')) {
		p[--n] = '\0';
	}
}

/* Parse one in-place modified line "key = value" -> calls config_set.
 * Lines beginning '#' or ';' (after leading whitespace) are skipped.
 * Returns 1 on accepted key, 0 on skipped (comment/blank), <0 on error. */
static int apply_line(char *line)
{
	char *p = skip_ws(line);
	if (*p == '\0' || *p == '#' || *p == ';') {
		return 0;
	}
	char *eq = strchr(p, '=');
	if (!eq) {
		LOG_WRN("cfgfile: malformed line (no '='): %s", p);
		return -EINVAL;
	}
	*eq = '\0';
	char *key = p;
	rstrip(key);

	char *val = skip_ws(eq + 1);
	rstrip(val);
	if (*key == '\0' || *val == '\0') {
		LOG_WRN("cfgfile: empty key or value");
		return -EINVAL;
	}

	int rc = config_set(key, val);
	if (rc == -ENOENT) {
		LOG_WRN("cfgfile: unknown key '%s' (skipped)", key);
		return 0;     /* don't count as error -- forward-compat */
	}
	if (rc < 0) {
		LOG_WRN("cfgfile: %s = %s -> %d (skipped)", key, val, rc);
		return 0;
	}
	LOG_INF("cfgfile: %s = %s", key, val);
	return 1;
}

int cfgfile_apply_from_sd(void)
{
	static char buf[CFGFILE_MAX_BYTES + 1];
	size_t n = 0;
	int rc = storage_read_file(CFGFILE_PATH, buf, sizeof(buf) - 1, &n);
	if (rc) {
		if (rc == -ENOENT) {
			LOG_INF("cfgfile: %s not present (nothing to apply)", CFGFILE_PATH);
		} else {
			LOG_WRN("cfgfile: read %s -> %d", CFGFILE_PATH, rc);
		}
		return rc;
	}
	buf[n] = '\0';

	int applied = 0;
	char *line = buf;
	while (line < buf + n) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = '\0';
		int r = apply_line(line);
		if (r > 0) applied++;
		line = nl ? nl + 1 : buf + n;
	}
	LOG_INF("cfgfile: %d key(s) applied from %s", applied, CFGFILE_PATH);
	return applied;
}

/* config_foreach callback: append "  key = value\n" to a growing buffer. */
struct export_ctx {
	char  *p;
	size_t cap;
	size_t len;
};

static void export_one(const char *key, const char *val, void *ctx)
{
	struct export_ctx *c = ctx;
	int n = snprintf(c->p + c->len, c->cap - c->len, "%s = %s\n", key, val);
	if (n > 0 && (size_t)n < c->cap - c->len) {
		c->len += (size_t)n;
	}
}

int cfgfile_export_to_sd(void)
{
	static char out[CFGFILE_MAX_BYTES];
	const char hdr[] = "# pwr-logger config snapshot -- restored by `pwr config import`\n";
	struct export_ctx ctx = { out, sizeof(out), 0 };

	if (sizeof(hdr) - 1 < ctx.cap) {
		memcpy(ctx.p, hdr, sizeof(hdr) - 1);
		ctx.len = sizeof(hdr) - 1;
	}
	config_foreach(export_one, &ctx);

	int rc = storage_write_file(CFGFILE_PATH, out, ctx.len);
	if (rc) {
		LOG_WRN("cfgfile: export %s -> %d", CFGFILE_PATH, rc);
	} else {
		LOG_INF("cfgfile: exported %zu B to %s", ctx.len, CFGFILE_PATH);
	}
	return rc;
}
