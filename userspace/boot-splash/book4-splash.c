/*
 * book4-splash - boot splash for the Samsung Galaxy Book4 Edge (X1P42100).
 *
 * Replaces plymouth, which fails on this hardware in three ways:
 *   - its udev hotplug path drops msm's card add event, so no splash is drawn;
 *   - it opens /dev/tty1, which ends fbcon's deferred takeover and destroys
 *     the firmware logo about a second into the boot;
 *   - about a second of daemon init sits between msm binding and the first pixel.
 *
 * This does none of that: no udev, no tty, no VT, no D-Bus. It polls /dev/dri
 * directly and opens no terminal, so the firmware logo survives until msm
 * resets the display block on its own.
 *
 * The frame is composed into the dumb buffer BEFORE drmModeSetCrtc (frecon's
 * trick), so the first scanout is already correct and there is no black frame.
 *
 * Handoff: on SIGUSR1 we drmDropMaster but keep the fd open and the buffer
 * alive, so the CRTC keeps scanning out our last frame while the display
 * manager and compositor come up. We exit once a wayland socket exists;
 * closing the last DRM fd earlier triggers drm_lastclose() ->
 * drm_fbdev_client_restore(), which commits the blank fbcon framebuffer.
 * Tested with SDDM + Hyprland.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* ---- tunables ------------------------------------------------------------ */

/* Defaults; THEME_FILE overrides them (see theme_load()). Match your
 * compositor's background so the handover shows no cut. */
#define BG_R 0x14
#define BG_G 0x13
#define BG_B 0x15

#define ACCENT_R 0xB8
#define ACCENT_G 0xA6
#define ACCENT_B 0xE8

/* "background=#RRGGBB" and "accent=#RRGGBB", one per line; optional */
#define THEME_FILE "/etc/book4-splash/theme.conf"

static uint8_t bg_r = BG_R, bg_g = BG_G, bg_b = BG_B;
static uint8_t ac_r = ACCENT_R, ac_g = ACCENT_G, ac_b = ACCENT_B;

#define CARD_WAIT_MS     20000   /* give up looking for a real DRM card */
#define SESSION_WAIT_MS  30000   /* hard cap before we exit regardless */
#define AUTO_RELEASE_MS  10000   /* drop master even if SIGUSR1 never comes */
#define HANDOVER_GRACE_MS 2500   /* after the wayland socket appears */
#define FRAME_MS         33      /* ~30fps */
/* Optional full-panel picture from mkground.sh. Without it, the flat colour. */
#define GROUND_FILE "/usr/share/book4-splash/ground.bgra"


/* ---- small helpers ------------------------------------------------------- */

static int verbose;

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * Failure paths are always logged, never gated on -v. This daemon's failure
 * mode is "black screen"; a silent exit(0) is the worst possible diagnostic.
 */
static void errmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * key=#RRGGBB, one per line. Anything unparseable leaves the fallback in place:
 * a malformed theme file must never cost us the splash.
 */
static void theme_load(void)
{
	char line[128];
	FILE *f = fopen(THEME_FILE, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		unsigned r, g, b;

		if (sscanf(line, "background=#%2x%2x%2x", &r, &g, &b) == 3) {
			bg_r = r; bg_g = g; bg_b = b;
		} else if (sscanf(line, "accent=#%2x%2x%2x", &r, &g, &b) == 3) {
			ac_r = r; ac_g = g; ac_b = b;
		}
	}
	fclose(f);
	logmsg("theme: bg #%02x%02x%02x accent #%02x%02x%02x",
	       bg_r, bg_g, bg_b, ac_r, ac_g, ac_b);
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ---- assets -------------------------------------------------------------- */

/*
 * Raw pre-converted BGRA, 8-byte header (u32 w, u32 h little-endian).
 * Produced by mkassets.sh so no PNG decoder is on the boot path.
 */
struct asset {
	uint32_t w, h;
	const uint8_t *px;      /* BGRA, w*h*4 */
	void *map;
	size_t maplen;
};

static int asset_load(struct asset *a, const char *path)
{
	struct stat st;
	int fd;
	uint32_t hdr[2];

	memset(a, 0, sizeof(*a));

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		logmsg("asset %s: %s", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
	    (size_t)st.st_size < sizeof(hdr)) {
		close(fd);
		return -1;
	}
	a->map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (a->map == MAP_FAILED) {
		a->map = NULL;
		return -1;
	}
	a->maplen = st.st_size;

	memcpy(hdr, a->map, sizeof(hdr));
	a->w = hdr[0];
	a->h = hdr[1];

	/*
	 * Bound the dimensions BEFORE multiplying: w and h come straight off
	 * disk, and w = h = 0x80000000 would make w*h*4 wrap to 0 and sail
	 * through the size check.
	 */
	if (!a->w || !a->h || a->w > 16384 || a->h > 16384 ||
	    (uint64_t)a->w * a->h * 4 + sizeof(hdr) > (uint64_t)st.st_size) {
		logmsg("asset %s: bad header %ux%u for %zu bytes",
		       path, a->w, a->h, (size_t)st.st_size);
		munmap(a->map, a->maplen);
		a->map = NULL;
		return -1;
	}
	a->px = (const uint8_t *)a->map + sizeof(hdr);
	logmsg("asset %s: %ux%u", path, a->w, a->h);
	return 0;
}

static volatile sig_atomic_t want_release;
static volatile sig_atomic_t want_quit;

/* ---- framebuffer --------------------------------------------------------- */

struct fb {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	uint8_t *map;
};

struct canvas {
	uint8_t *px;            /* XRGB8888 */
	uint32_t w, h, pitch;
};

static void canvas_fill(struct canvas *c, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
	uint32_t x, y;

	for (y = 0; y < c->h; y++) {
		uint32_t *row = (uint32_t *)(c->px + (uint64_t)y * c->pitch);
		for (x = 0; x < c->w; x++)
			row[x] = v;
	}
}

/* ---- the scene ----------------------------------------------------------- */

struct scene {
	struct asset ground;
};

/* One static picture. Anything animated would be cut at the handover anyway. */
static void scene_draw(struct canvas *c, struct scene *s)
{
	if (s->ground.px && s->ground.w == c->w && s->ground.h == c->h) {
		/* opaque ground: straight row copies, no blend */
		uint32_t y;
		for (y = 0; y < c->h; y++)
			memcpy(c->px + (uint64_t)y * c->pitch,
			       s->ground.px + (uint64_t)y * s->ground.w * 4,
			       (size_t)c->w * 4);
	} else {
		canvas_fill(c, bg_r, bg_g, bg_b);
	}
}

/* ---- DRM ----------------------------------------------------------------- */

struct display {
	int fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	drmModeModeInfo mode;
	drmModeCrtcPtr saved_crtc;
	struct fb fb[2];
	int front;
	/* atomic path (see present()) */
	int atomic_ok;
	uint32_t plane_id;
	uint32_t mode_blob, ctm_blob;
	uint32_t p_conn_crtc, p_conn_link;
	uint32_t p_crtc_active, p_crtc_mode, p_crtc_vrr, p_crtc_ctm;
	uint32_t p_pl_fb, p_pl_crtc, p_pl_sx, p_pl_sy, p_pl_sw, p_pl_sh,
		 p_pl_cx, p_pl_cy, p_pl_cw, p_pl_ch;
};

/*
 * Why atomic, and why exactly these properties: the compositor's first commit is an
 * atomic modeset that sets connector CRTC_ID + link-status, CRTC ACTIVE + MODE_ID (+
 * VRR_ENABLED), and the primary plane. If the state it finds is bit-for-bit what it
 * asks for, the kernel treats that commit as a page flip and the panel never
 * power-cycles at the handover. The one that bit: msm_atomic_check() forces a full
 * modeset whenever a commit adds or removes the CRTC's CTM blob, and the compositor's
 * first commit always carries an identity CTM. So we set the same state, the same way,
 * identity CTM included.
 */
static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name,
			uint64_t *cur)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0, i;
	if (!props)
		return 0;
	for (i = 0; i < props->count_props && !id; i++) {
		drmModePropertyPtr pr = drmModeGetProperty(fd, props->props[i]);
		if (!pr)
			continue;
		if (!strcmp(pr->name, name)) {
			id = pr->prop_id;
			if (cur)
				*cur = props->prop_values[i];
		}
		drmModeFreeProperty(pr);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static int atomic_setup(struct display *dp)
{
	drmModeResPtr res;
	drmModePlaneResPtr pres;
	int crtc_index = -1, i;
	uint32_t j;

	dp->atomic_ok = 0;
	dp->plane_id = 0;
	if (dp->mode_blob)
		drmModeDestroyPropertyBlob(dp->fd, dp->mode_blob);
	if (dp->ctm_blob)
		drmModeDestroyPropertyBlob(dp->fd, dp->ctm_blob);
	dp->mode_blob = dp->ctm_blob = 0;
	if (drmSetClientCap(dp->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(dp->fd, DRM_CLIENT_CAP_ATOMIC, 1))
		return -1;
	res = drmModeGetResources(dp->fd);
	if (!res)
		return -1;
	for (i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == dp->crtc_id)
			crtc_index = i;
	drmModeFreeResources(res);
	if (crtc_index < 0)
		return -1;

	pres = drmModeGetPlaneResources(dp->fd);
	if (!pres)
		return -1;
	for (j = 0; j < pres->count_planes && !dp->plane_id; j++) {
		drmModePlanePtr pl = drmModeGetPlane(dp->fd, pres->planes[j]);
		uint64_t type = 0;
		if (!pl)
			continue;
		if ((pl->possible_crtcs & (1u << crtc_index)) &&
		    prop_id(dp->fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type) &&
		    type == DRM_PLANE_TYPE_PRIMARY)
			dp->plane_id = pl->plane_id;
		drmModeFreePlane(pl);
	}
	drmModeFreePlaneResources(pres);
	if (!dp->plane_id)
		return -1;

	dp->p_conn_crtc = prop_id(dp->fd, dp->conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	dp->p_conn_link = prop_id(dp->fd, dp->conn_id, DRM_MODE_OBJECT_CONNECTOR, "link-status", NULL);
	dp->p_crtc_active = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	dp->p_crtc_mode = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	dp->p_crtc_vrr = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", NULL);
	dp->p_crtc_ctm = prop_id(dp->fd, dp->crtc_id, DRM_MODE_OBJECT_CRTC, "CTM", NULL);
	dp->p_pl_fb = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
	dp->p_pl_crtc = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
	dp->p_pl_sx = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
	dp->p_pl_sy = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
	dp->p_pl_sw = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
	dp->p_pl_sh = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
	dp->p_pl_cx = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
	dp->p_pl_cy = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
	dp->p_pl_cw = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
	dp->p_pl_ch = prop_id(dp->fd, dp->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
	if (!dp->p_conn_crtc || !dp->p_crtc_active || !dp->p_crtc_mode || !dp->p_pl_fb ||
	    !dp->p_pl_crtc || !dp->p_pl_sx || !dp->p_pl_sy || !dp->p_pl_sw || !dp->p_pl_sh ||
	    !dp->p_pl_cx || !dp->p_pl_cy || !dp->p_pl_cw || !dp->p_pl_ch)
		return -1;
	if (drmModeCreatePropertyBlob(dp->fd, &dp->mode, sizeof(dp->mode), &dp->mode_blob))
		return -1;
	if (dp->p_crtc_ctm) {
		struct drm_color_ctm ctm = { .matrix = { 1ULL << 32, 0, 0,
							 0, 1ULL << 32, 0,
							 0, 0, 1ULL << 32 } };
		if (drmModeCreatePropertyBlob(dp->fd, &ctm, sizeof(ctm), &dp->ctm_blob))
			dp->ctm_blob = 0;
	}
	dp->atomic_ok = 1;
	return 0;
}

/* Put fb_id on the panel: an atomic modeset (first frame) or an atomic flip; legacy
 * SetCrtc if the atomic path is unavailable or refused. Returns 0 on success. */
static int present(struct display *dp, uint32_t fb_id, int modeset)
{
	if (dp->atomic_ok) {
		drmModeAtomicReqPtr req = drmModeAtomicAlloc();
		uint32_t flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
		int rc;
		if (!req)
			goto legacy;
		if (modeset) {
			drmModeAtomicAddProperty(req, dp->conn_id, dp->p_conn_crtc, dp->crtc_id);
			if (dp->p_conn_link)
				drmModeAtomicAddProperty(req, dp->conn_id, dp->p_conn_link,
							 DRM_MODE_LINK_STATUS_GOOD);
			drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_active, 1);
			drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_mode, dp->mode_blob);
			if (dp->p_crtc_vrr)
				drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_vrr, 0);
			if (dp->ctm_blob)
				drmModeAtomicAddProperty(req, dp->crtc_id, dp->p_crtc_ctm, dp->ctm_blob);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_crtc, dp->crtc_id);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sx, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sy, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sw,
						 (uint64_t)dp->mode.hdisplay << 16);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_sh,
						 (uint64_t)dp->mode.vdisplay << 16);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_cx, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_cy, 0);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_cw, dp->mode.hdisplay);
			drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_ch, dp->mode.vdisplay);
		}
		drmModeAtomicAddProperty(req, dp->plane_id, dp->p_pl_fb, fb_id);
		rc = drmModeAtomicCommit(dp->fd, req, flags, NULL);
		drmModeAtomicFree(req);
		if (rc == 0)
			return 0;
		errmsg("book4-splash: atomic %s failed (%s), legacy path",
		       modeset ? "modeset" : "flip", strerror(errno));
		dp->atomic_ok = 0;
	}
legacy:
	return drmModeSetCrtc(dp->fd, dp->crtc_id, fb_id, 0, 0, &dp->conn_id, 1, &dp->mode);
}

/*
 * Reject simpledrm: it is the firmware framebuffer holding the Samsung logo.
 *
 * CRITICAL: we must not open-then-close it. drm_release() -> drm_lastclose()
 * -> drm_client_dev_restore() -> drm_fbdev_client_restore() commits the BLANK
 * fbdev shadow buffer, which on simpledrm blits black over the firmware
 * framebuffer. Polling every 10ms would do that ~450 times before msm binds,
 * destroying the logo at ~1s -- earlier than plymouth did, and by the very
 * daemon that exists to prevent it.
 *
 * So: identify simpledrm from sysfs WITHOUT opening it, and if we ever do open
 * a card we reject for another reason, hold its fd open for our lifetime so
 * its open_count never reaches zero.
 */
static int held_fds[8];
static int held_n;

static void hold_fd(int fd)
{
	if (held_n < (int)(sizeof(held_fds) / sizeof(held_fds[0])))
		held_fds[held_n++] = fd;
	else
		close(fd);   /* should never happen; better than leaking */
}

/*
 * True if /sys/class/drm/<name> resolves through a simple-framebuffer platform
 * device. This is the same test plymouth uses (syspath_is_simpledrm), and it
 * needs no open(). Note the platform driver is called "simple-framebuffer",
 * not "simpledrm" -- comparing against the DRM driver name here would silently
 * never match.
 */
static int sysfs_is_simpledrm(const char *name)
{
	char link[PATH_MAX];
	char target[PATH_MAX];
	ssize_t n;

	snprintf(link, sizeof(link), "/sys/class/drm/%s", name);
	n = readlink(link, target, sizeof(target) - 1);
	if (n < 0)
		return 0;
	target[n] = '\0';
	return strstr(target, "simple-framebuffer") != NULL;
}

static int is_real_card(int fd)
{
	drmVersionPtr v = drmGetVersion(fd);
	int real;

	if (!v)
		return 0;
	real = strcmp(v->name, "simpledrm") != 0;
	logmsg("drm driver: %s (%s)", v->name, real ? "accept" : "skip");
	drmFreeVersion(v);
	return real;
}

static int open_real_card(uint64_t deadline)
{
	for (;;) {
		DIR *d = opendir("/dev/dri");
		struct dirent *e;

		if (d) {
			while ((e = readdir(d))) {
				char path[NAME_MAX + 16];
				int fd;

				if (strncmp(e->d_name, "card", 4) != 0)
					continue;
				/* never even open the firmware framebuffer */
				if (sysfs_is_simpledrm(e->d_name))
					continue;

				snprintf(path, sizeof(path), "/dev/dri/%s",
					 e->d_name);
				fd = open(path, O_RDWR | O_CLOEXEC);
				if (fd < 0)
					continue;
				if (is_real_card(fd)) {
					closedir(d);
					logmsg("using %s", path);
					return fd;
				}
				/* rejected: keep it open, do not trip lastclose */
				hold_fd(fd);
			}
			closedir(d);
		}
		if (want_quit)
			return -1;
		if (now_ms() >= deadline)
			return -1;
		sleep_ms(10);
	}
}

static int connector_is_internal(drmModeConnectorPtr conn)
{
	return conn->connector_type == DRM_MODE_CONNECTOR_eDP ||
	       conn->connector_type == DRM_MODE_CONNECTOR_DSI ||
	       conn->connector_type == DRM_MODE_CONNECTOR_LVDS;
}

/* Returns 0 and fills dp->conn_id / mode / crtc_id if this connector is usable. */
static int try_connector(struct display *dp, drmModeResPtr res,
			 drmModeConnectorPtr conn)
{
	int j, ok = -1;

	if (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes)
		return -1;

	/* preferred mode, else the first (they are listed best-first) */
	dp->mode = conn->modes[0];
	for (j = 0; j < conn->count_modes; j++) {
		if (conn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
			dp->mode = conn->modes[j];
			break;
		}
	}
	dp->conn_id = conn->connector_id;

	for (j = 0; j < conn->count_encoders && ok < 0; j++) {
		drmModeEncoderPtr enc =
			drmModeGetEncoder(dp->fd, conn->encoders[j]);
		int k;

		if (!enc)
			continue;
		for (k = 0; k < res->count_crtcs; k++) {
			if (!(enc->possible_crtcs & (1u << k)))
				continue;
			dp->crtc_id = res->crtcs[k];
			ok = 0;
			break;
		}
		drmModeFreeEncoder(enc);
	}
	return ok;
}

static int pick_output(struct display *dp)
{
	drmModeResPtr res = drmModeGetResources(dp->fd);
	int i, ok = -1;

	if (!res)
		return -1;

	/*
	 * Connector selection is on the critical path to first light, and
	 * drmModeGetConnector() FORCES a detect -- on eDP that is an EDID read
	 * over AUX, on a disconnected DP or HDMI port it is a detect that can
	 * block for tens of ms each. res->connectors[] here is ordered DP-1,
	 * DP-2, eDP-1, HDMI-A-1, Writeback-1, so probing in array order pays
	 * for two dead DisplayPorts before ever reaching the laptop panel.
	 *
	 * So: read the CACHED state first (no probe), and when a probe is
	 * unavoidable, decide whether we even want this connector -- from its
	 * cached type -- BEFORE paying for it.
	 *
	 *   pass 0  cached, internal only          (free)
	 *   pass 1  probe, but only internal       (one EDID read)
	 *   pass 2  cached, anything connected     (free)
	 *   pass 3  probe anything                 (last resort)
	 */
	for (i = 0; i < res->count_connectors * 4 && ok < 0; i++) {
		int pass = i / res->count_connectors;
		int idx = i % res->count_connectors;
		drmModeConnectorPtr cached, conn;

		cached = drmModeGetConnectorCurrent(dp->fd, res->connectors[idx]);
		if (!cached)
			continue;

		if (pass < 2 && !connector_is_internal(cached)) {
			drmModeFreeConnector(cached);
			continue;
		}

		if (pass == 0 || pass == 2) {
			ok = try_connector(dp, res, cached);
			drmModeFreeConnector(cached);
			continue;
		}

		/* worth a real probe */
		drmModeFreeConnector(cached);
		conn = drmModeGetConnector(dp->fd, res->connectors[idx]);
		if (!conn)
			continue;
		ok = try_connector(dp, res, conn);
		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);
	if (ok == 0)
		logmsg("connector %u crtc %u mode %ux%u@%u",
		       dp->conn_id, dp->crtc_id,
		       dp->mode.hdisplay, dp->mode.vdisplay, dp->mode.vrefresh);
	return ok;
}

static int fb_create(int fd, struct fb *f, uint32_t w, uint32_t h)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;

	memset(&creq, 0, sizeof(creq));
	creq.width = w;
	creq.height = h;
	creq.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return -1;

	f->handle = creq.handle;
	f->pitch = creq.pitch;
	f->size = creq.size;

	if (drmModeAddFB(fd, w, h, 24, 32, f->pitch, f->handle, &f->fb_id) < 0)
		return -1;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = f->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		return -1;

	f->map = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      fd, mreq.offset);
	if (f->map == MAP_FAILED) {
		f->map = NULL;
		return -1;
	}
	return 0;
}

/* ---- mode cache ---------------------------------------------------------- */

/*
 * The 171ms in `pick` is one drmModeGetConnector() on eDP-1: the first EDID
 * read over the DP AUX channel plus mode-list construction. It is not the dead
 * DisplayPorts (skipping those changed nothing), and it is not something a
 * better search order can avoid -- it is the cost of asking the panel who it
 * is, and the panel is the same panel on every boot.
 *
 * So ask once. After a successful modeset we write the connector id, CRTC id
 * and the exact modeline to disk; on later boots we go straight to that and
 * never probe. If anything about it no longer holds -- different hardware, a
 * stale id, a modeset that fails -- we fall back to the full probing path, so
 * the cache can only ever make us faster, never wrong.
 */
#define MODE_CACHE "/var/lib/book4-splash/mode.bin"
#define MODE_CACHE_MAGIC 0x4d425431u   /* "MBT1" */

struct mode_cache {
	uint32_t magic;
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t conn_type;
	drmModeModeInfo mode;
};

static int mode_cache_load(struct display *dp)
{
	struct mode_cache mc;
	drmModeConnectorPtr conn;
	int fd, ok = -1;

	fd = open(MODE_CACHE, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (read(fd, &mc, sizeof(mc)) == (ssize_t)sizeof(mc) &&
	    mc.magic == MODE_CACHE_MAGIC &&
	    mc.mode.hdisplay >= 1 && mc.mode.hdisplay <= 16384 &&
	    mc.mode.vdisplay >= 1 && mc.mode.vdisplay <= 16384) {
		/*
		 * Sanity check, but a MISSING connector is not a rejection.
		 *
		 * We open the card the instant its node appears -- earlier than
		 * anything else on the system -- so drmModeGetConnectorCurrent can
		 * still return NULL here while the driver finishes registering
		 * connectors. Treating that as "cache invalid" is what made every
		 * boot fall back to the full probe and pay ~190ms, even with a
		 * valid cache sitting on disk (measured: "probe 189" with a good
		 * mode.bin present).
		 *
		 * So: reject only on a POSITIVE mismatch. If the connector is not
		 * readable yet we take the cache anyway -- drmModeSetCrtc is the
		 * real validator, and the caller already falls back to probing if
		 * it refuses the mode.
		 */
		/*
		 * MEASURED: with the cache hit and this call still being made, the
		 * phase cost 191ms -- against 189ms for the full EDID probe it was
		 * supposed to replace. Identical. So the ~190ms is not the probe at
		 * all; it is the FIRST DRM ioctl on a device that has only just
		 * registered, and any call pays it. Skip the call entirely on a
		 * valid cache and let drmModeSetCrtc be the first one, so the next
		 * boot's "cache N" says whether that 190ms is ours to save or the
		 * driver's to settle.
		 */
		conn = NULL;
		if (!conn || conn->connector_type == mc.conn_type) {
			dp->conn_id = mc.conn_id;
			dp->crtc_id = mc.crtc_id;
			dp->mode = mc.mode;
			ok = 0;
		}
		if (conn)
			drmModeFreeConnector(conn);
	}
	close(fd);
	return ok;
}

static void mode_cache_save(struct display *dp)
{
	struct mode_cache mc;
	drmModeConnectorPtr conn;
	char tmp[] = MODE_CACHE ".XXXXXX";
	int fd;

	memset(&mc, 0, sizeof(mc));
	mc.magic = MODE_CACHE_MAGIC;
	mc.conn_id = dp->conn_id;
	mc.crtc_id = dp->crtc_id;
	mc.mode = dp->mode;

	conn = drmModeGetConnectorCurrent(dp->fd, dp->conn_id);
	if (!conn)
		return;
	mc.conn_type = conn->connector_type;
	drmModeFreeConnector(conn);

	if (mkdir("/var/lib/book4-splash", 0755) < 0 && errno != EEXIST)
		return;
	fd = mkstemp(tmp);
	if (fd < 0)
		return;
	if (write(fd, &mc, sizeof(mc)) == (ssize_t)sizeof(mc)) {
		fchmod(fd, 0644);
		close(fd);
		if (rename(tmp, MODE_CACHE) == 0)
			return;
	} else {
		close(fd);
	}
	unlink(tmp);
}

/* ---- session detection --------------------------------------------------- */

static int wayland_socket_exists(void)
{
	DIR *ud = opendir("/run/user");
	struct dirent *ue;
	int found = 0;

	if (!ud)
		return 0;
	while (!found && (ue = readdir(ud))) {
		char dir[NAME_MAX + 16];
		DIR *sd;
		struct dirent *se;

		if (ue->d_name[0] < '0' || ue->d_name[0] > '9')
			continue;
		snprintf(dir, sizeof(dir), "/run/user/%s", ue->d_name);
		sd = opendir(dir);
		if (!sd)
			continue;
		while ((se = readdir(sd))) {
			size_t n = strlen(se->d_name);
			if (strncmp(se->d_name, "wayland-", 8) != 0)
				continue;
			if (n > 5 && strcmp(se->d_name + n - 5, ".lock") == 0)
				continue;
			found = 1;
			break;
		}
		closedir(sd);
	}
	closedir(ud);
	return found;
}

/* ---- backlight + driver load -------------------------------------------- */
/*
 * The msm driver resets the panel pipeline when it loads and does not inherit the
 * firmware's configuration. Lit, that reset shows as a torn bright line after the
 * firmware logo. So this daemon owns the handover: msm is blacklisted from autoload;
 * we switch the backlight off, load msm ourselves, compose and modeset, and switch
 * the backlight on once the first frame is scanning out. Entries without `splash`
 * load msm from book4-splash-msm-load.service; book4-splash-release.service
 * restores the backlight if we died in between.
 */
static char backlight_node[PATH_MAX];

static const char *backlight_path(void)
{
	DIR *d;
	struct dirent *e;

	if (backlight_node[0])
		return backlight_node;
	d = opendir("/sys/class/backlight");
	if (!d)
		return NULL;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(backlight_node, sizeof(backlight_node),
			 "/sys/class/backlight/%s/brightness", e->d_name);
		break;
	}
	closedir(d);
	return backlight_node[0] ? backlight_node : NULL;
}
#define BACKLIGHT_NODE backlight_path()
static int saved_brightness = -1;

static int read_int(const char *path)
{
	char buf[32];
	FILE *f = fopen(path, "r");
	int v = -1;
	if (!f)
		return -1;
	if (fgets(buf, sizeof(buf), f))
		v = atoi(buf);
	fclose(f);
	return v;
}

static void write_int(const char *path, int v)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%d", v);
	fclose(f);
}

static void backlight_off(void)
{
	uint64_t deadline = now_ms() + 1500;
	int v = -1;
	/* the backlight device usually exists by now; give it a moment if not */
	while ((!BACKLIGHT_NODE || (v = read_int(BACKLIGHT_NODE)) < 0) && now_ms() < deadline)
		sleep_ms(10);
	if (v <= 0)
		return;
	saved_brightness = v;
	write_int(BACKLIGHT_NODE, 0);
	errmsg("book4-splash: backlight off (was %d)", v);
}

static void backlight_restore(void)
{
	int i;
	if (saved_brightness <= 0)
		return;
	/* the panel ramps up instead of the splash fading */
	for (i = 1; i <= 8; i++) {
		write_int(BACKLIGHT_NODE, saved_brightness * i / 8);
		if (i < 8)
			sleep_ms(25);
	}
	errmsg("book4-splash: backlight restored to %d", saved_brightness);
	saved_brightness = -1;
}

static int load_msm(void)
{
	int rc = system("/sbin/modprobe msm");
	errmsg("book4-splash: modprobe msm -> %d", rc);
	return rc;
}


/* ---- main ---------------------------------------------------------------- */

static void on_usr1(int sig) { (void)sig; want_release = 1; }
static void on_term(int sig) { (void)sig; want_quit = 1; }

static int dry_run(struct scene *sc, const char *out)
{
	struct canvas c;
	FILE *f;
	uint32_t x, y;

	c.w = 1920;
	c.h = 1080;
	c.pitch = c.w * 4;
	c.px = calloc(1, (size_t)c.pitch * c.h);
	if (!c.px)
		return 1;

	scene_draw(&c, sc);

	f = fopen(out, "wb");
	if (!f) {
		free(c.px);
		return 1;
	}
	fprintf(f, "P6\n%u %u\n255\n", c.w, c.h);
	for (y = 0; y < c.h; y++) {
		for (x = 0; x < c.w; x++) {
			uint32_t v = *(uint32_t *)(c.px + (uint64_t)y * c.pitch + x * 4);
			uint8_t rgb[3] = { (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	free(c.px);
	fprintf(stderr, "wrote %s\n", out);
	return 0;
}

int main(int argc, char **argv)
{
	struct display dp;
	struct scene sc;
	struct canvas c;
	struct canvas pre;
	struct sigaction sa;
	uint64_t t0, t_card, t_phase, deadline;
	uint64_t t_release = 0;
	uint64_t ms_pick = 0, ms_fb = 0, ms_draw = 0;
	int cached_mode = 0;
	const char *ppm = NULL;
	int check_cache = 0;
	int shutdown_mode = 0;	/* --shutdown: the same scene, held until the kernel goes */
	int i, released = 0;

	memset(&dp, 0, sizeof(dp));
	memset(&sc, 0, sizeof(sc));
	memset(&pre, 0, sizeof(pre));

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (!strcmp(argv[i], "--check-cache"))
			check_cache = 1;
		else if (!strcmp(argv[i], "--shutdown"))
			shutdown_mode = 1;
		else if (!strncmp(argv[i], "--dry-run", 9))
			ppm = (i + 1 < argc && argv[i + 1][0] != '-') ?
				argv[++i] : "/tmp/book4-splash.ppm";
	}

	theme_load();
	asset_load(&sc.ground, GROUND_FILE);
	if (!sc.ground.px)
		errmsg("book4-splash: no ground at %s, flat colour", GROUND_FILE);

	if (ppm)
		return dry_run(&sc, ppm);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	/*
	 * Pre-render into heap while we wait for msm. The panel is dark from
	 * the moment msm resets the DPU until our modeset lands, so every
	 * millisecond between those two points is visible black. Composing
	 * 1920x1080 in software costs ~31ms measured; doing it now instead of
	 * after the card appears takes that off the critical path.
	 */
	pre.w = 1920;
	pre.h = 1080;
	pre.pitch = pre.w * 4;
	pre.px = malloc((size_t)pre.pitch * pre.h);
	if (pre.px) {
		scene_draw(&pre, &sc);
	}

	if (!check_cache && !shutdown_mode) {	/* the panel is already up at shutdown; --check-cache runs on a live desktop */
		backlight_off();
		if (load_msm() != 0) {
			backlight_restore();
			return 0;
		}
	}
	dp.fd = open_real_card(now_ms() + CARD_WAIT_MS);
	t_card = now_ms();

	if (check_cache) {
		/* Verify the cache resolves, WITHOUT taking master or
		 * touching the display. Safe to run on a live desktop. */
		uint64_t t = now_ms();
		int hit = dp.fd >= 0 && mode_cache_load(&dp) == 0;
		errmsg("cache %s in %lums: conn %u crtc %u mode %ux%u@%u",
		       hit ? "HIT" : "MISS", (unsigned long)(now_ms() - t),
		       dp.conn_id, dp.crtc_id, dp.mode.hdisplay,
		       dp.mode.vdisplay, dp.mode.vrefresh);
		return hit ? 0 : 1;
	}
	if (dp.fd < 0) {
		errmsg("book4-splash: no real DRM card appeared in %dms",
		       CARD_WAIT_MS);
		backlight_restore();
		return 0;
	}
	t_phase = now_ms();
	cached_mode = mode_cache_load(&dp) == 0;
	if (!cached_mode && pick_output(&dp) < 0) {
		errmsg("book4-splash: no connected output");
		backlight_restore();
		return 0;
	}
	ms_pick = now_ms() - t_phase;
	/*
	 * At ~4.5s nothing legitimately holds master yet, but retry briefly
	 * rather than allocating two 8MB buffers only to fail the modeset.
	 */
	for (i = 0; i < (shutdown_mode ? 300 : 20); i++) {
		if (drmSetMaster(dp.fd) == 0)
			break;
		sleep_ms(10);
	}
	if (i == (shutdown_mode ? 300 : 20)) {
		errmsg("book4-splash: drmSetMaster: %s", strerror(errno));
		backlight_restore();
		return 0;
	}

	/* Only the front buffer is on the critical path to first light. */
	t_phase = now_ms();
	if (fb_create(dp.fd, &dp.fb[0], dp.mode.hdisplay,
		      dp.mode.vdisplay) < 0) {
		errmsg("book4-splash: fb_create[0]: %s", strerror(errno));
		if (cached_mode)
			unlink(MODE_CACHE);
		backlight_restore();
		return 0;
	}
	ms_fb = now_ms() - t_phase;

	c.w = dp.mode.hdisplay;
	c.h = dp.mode.vdisplay;
	/*
	 * Compose BEFORE the modeset. This is the whole point: the first
	 * scanout is already the finished frame, so the panel never shows a
	 * blank buffer at the handover.
	 */
	t0 = now_ms();
	c.px = dp.fb[0].map;
	c.pitch = dp.fb[0].pitch;
	t_phase = now_ms();
	if (pre.px && c.w == pre.w && c.h == pre.h) {
		uint32_t y;
		for (y = 0; y < c.h; y++)
			memcpy(c.px + (uint64_t)y * c.pitch,
			       pre.px + (uint64_t)y * pre.pitch,
			       (size_t)c.w * 4);
	} else {
		scene_draw(&c, &sc);
	}
	ms_draw = now_ms() - t_phase;
	t_phase = now_ms();

	if (atomic_setup(&dp) < 0)
		errmsg("book4-splash: atomic path unavailable, legacy SetCrtc");
	if (present(&dp, dp.fb[0].fb_id, 1) < 0) {
		if (!cached_mode) {
			errmsg("book4-splash: modeset: %s",
			       strerror(errno));
			backlight_restore();
		return 0;
		}
		/* stale cache: fall back to the full probe and try once more */
		errmsg("book4-splash: cached mode rejected (%s), probing",
		       strerror(errno));
		unlink(MODE_CACHE);
		cached_mode = 0;
		if (pick_output(&dp) < 0) {
			errmsg("book4-splash: no output: %s", strerror(errno));
			backlight_restore();
			return 0;
		}
		atomic_setup(&dp);
		if (present(&dp, dp.fb[0].fb_id, 1) < 0) {
			errmsg("book4-splash: modeset: %s",
			       strerror(errno));
			backlight_restore();
		return 0;
		}
	}
	if (!cached_mode)
		mode_cache_save(&dp);
	dp.front = 0;
	/* one vblank so the first scanout has happened before the panel lights */
	sleep_ms(20);
	backlight_restore();
	{
		uint64_t ls = 99, act = 99;
		prop_id(dp.fd, dp.conn_id, DRM_MODE_OBJECT_CONNECTOR, "link-status", &ls);
		prop_id(dp.fd, dp.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &act);
		uint64_t ctm = 0;
		prop_id(dp.fd, dp.crtc_id, DRM_MODE_OBJECT_CRTC, "CTM", &ctm);
		errmsg("book4-splash: %s path, plane %u, link-status %lu, ACTIVE %lu, CTM %lu",
		       dp.atomic_ok ? "atomic" : "legacy", dp.plane_id,
		       (unsigned long)ls, (unsigned long)act, (unsigned long)ctm);
	}
	errmsg("book4-splash: splash up %lums after card appeared "
	       "(%s %lu, fb %lu, draw %lu, modeset %lu)",
	       (unsigned long)(now_ms() - t_card),
	       cached_mode ? "cache" : "probe", (unsigned long)ms_pick, (unsigned long)ms_fb,
	       (unsigned long)ms_draw, (unsigned long)(now_ms() - t_phase));

	if (fb_create(dp.fd, &dp.fb[1], dp.mode.hdisplay, dp.mode.vdisplay) < 0) {
		errmsg("book4-splash: fb_create[1]: %s (no animation)",
		       strerror(errno));
		dp.fb[1].map = NULL;
	}

	deadline = now_ms() + (shutdown_mode ? (uint64_t)1 << 40 : SESSION_WAIT_MS);

	for (;;) {
		uint64_t t = now_ms();

		if (want_quit)
			break;

		/*
		 * Defence in depth: if the release unit never fires, drop
		 * master anyway. Holding it means the compositor cannot take
		 * the panel at all -- a black screen caused by the splash.
		 */
		if (shutdown_mode)
			want_release = 0;
		if (!shutdown_mode && !released && t - t0 >= AUTO_RELEASE_MS && !want_release) {
			errmsg("book4-splash: no release signal after %dms, "
			       "dropping master anyway", AUTO_RELEASE_MS);
			want_release = 1;
		}

		if (want_release && !released) {
			drmDropMaster(dp.fd);
			released = 1;
			/* re-arm: the hold must be measured from the handover,
			 * not from when the splash first went up */
			deadline = now_ms() + SESSION_WAIT_MS;
			t_release = now_ms();
			t = t_release;	/* the fade above took time; do not test a stale t below */
			errmsg("book4-splash: master dropped at +%lums, holding frame",
			       (unsigned long)(t - t0));
		}

		if (released) {
			/*
			 * Do NOT close the fd. While it stays open the CRTC
			 * keeps scanning out our buffer; closing it would let
			 * drm_lastclose() restore the blank fbcon framebuffer.
			 */
			/*
			 * The socket appears when the compositor BINDS it,
			 * which is before its first DRM commit. We must not
			 * exit before that commit: drm_file_free() calls
			 * drm_fb_release() unconditionally, and removing a FB
			 * that is still on the primary plane blanks it --
			 * keeping open_count above zero does not save us.
			 */
			if (wayland_socket_exists()) {
				sleep_ms(HANDOVER_GRACE_MS);
				break;
			}
			if (t >= deadline)
				break;
			sleep_ms(30);
			continue;
		}

		if (t >= deadline)
			break;
		sleep_ms(FRAME_MS);
	}

	backlight_restore();
	errmsg("book4-splash: exiting at +%lums",
	       (unsigned long)(now_ms() - t0));
	return 0;
}
