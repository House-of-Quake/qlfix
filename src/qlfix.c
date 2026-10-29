/*
 * qlfix.c -- LD_PRELOAD loader for the qagame byte patches.
 *
 * The engine dlopen()s baseq3/qagamex64.so on every map change and
 * map_restart, dlclose()ing the previous one first.  In the live server
 * that really unmaps it and every load is a fresh patch; a module glibc
 * keeps mapped is handled too.  qlfix interposes dlopen: after a
 * successful open of that module it finds the base via the exported
 * dllEntry, fingerprints each site, and patches it -- or restores it when
 * its cvar is 0.  Nothing else is hooked; other preloads (minqlx) are not
 * touched.  dlopen(RTLD_NOLOAD) probes are passed through untouched.  The
 * code cave is two pages mapped below the module within rel32 reach,
 * writable only while sites are being (re)built and read-execute
 * otherwise.
 *
 * Engine access (cvars, console output, the qlfix_status command) is
 * optional: it is armed once by checking that this process is qzeroded.x64
 * build 1069 (build string and function prologues, see ql_syms.h and
 * ql_arm.c).  Without it -- e.g. loaded outside the server -- the qagame
 * sites use their defaults, the engine site is not applied, and notices go
 * to stderr / QLFIX_LOG.
 *
 * Environment:
 *   QLFIX_DISABLE=1     pass every dlopen straight through, patch nothing
 *   QLFIX_LOG=<path>    append log lines to this file as well (unset, "",
 *                       "0", "off", "none" = no file; see ql_log.h)
 *   QLFIX_TEST_CVARS    "name=value,name=value": cvar values to use while
 *                       engine access is off (a dry run outside the
 *                       server); ignored once it is armed
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "qlfix.h"
#include "qlfix_version.h"
#include "ql_arm.h"
#include "ql_syms.h"

static const qlfix_site_t *const sites[] = {
	&qlfix_site_footstep,
	&qlfix_site_splash,
	&qlfix_site_splash_surface,
	&qlfix_site_missile_stuck,
	&qlfix_site_missile_attribute,
	&qlfix_site_shotgun_origin,
	&qlfix_site_fps,
	&qlfix_site_footstep_emit,
	&qlfix_site_dead_holdable,
};
#define NSITES (sizeof sites / sizeof sites[0])

/* sites in the engine itself: absolute addresses, base 0 */
static const qlfix_site_t *const engine_sites[] = {
	&qlfix_site_curve_trace,
};
#define NENGINE (sizeof engine_sites / sizeof engine_sites[0])
#define NSITES_MAX (NSITES > NENGINE ? NSITES : NENGINE)

/* ---- engine ---- */

static ql_Cvar_Get_t   cvar_get;
static cvar_t         *cvar_maxfps, *cvar_maxfpsMargin, *cvar_maxfpsWindow, *cvar_maxfpsStrikes;   /* the switch, qlfix_fpsKick, is the fps site's cvar */
static int             kick_armed;      /* Cbuf_AddText, SV_SendServerCommand, SV_DropClient, Cmd_Argc/Argv usable */
static cvar_t         *site_cvars[NSITES];
static cvar_t         *engine_site_cvars[NENGINE];
static int             engine_tried, engine_armed;

/* sv.time: the server's game clock, the time its snapshots carry.  Only
 * read once the build string has been verified (engine_arm). */
static int sv_time(void) { return QL_R32(A_sv_time); }

static const char *fn_state_name(int st)
{
	return st == QL_FN_DETOURED ? "detoured (minqlx)" : ql_arm_state_name(st);
}

/* A function we only call may start with minqlx's detour; calling through it
 * is what the engine's own callers do (ql_arm.h). */
static int fn_usable(int st)
{
	return st == QL_FN_CLEAN || st == QL_FN_DETOURED;
}

/* Queue a console command; the engine runs its buffer at the top of the
 * next frame, outside any client's think -- the only safe place to drop a
 * client from code that runs inside one. */
void qlfix_engine_command(const char *line)
{
	char buf[512];
	if (!kick_armed) { qlfix_notice("would run: %s", line); return; }
	snprintf(buf, sizeof buf, "%s\n", line);
	QL_Cbuf_AddText(buf);
}

/* A message to every client, the way qagame's PrintMsg (0x941e0) sends its
 * own: `print "<text>"` through SV_SendServerCommand (0x43db90).  Safe
 * inside a client's think: the command is only queued. */
void qlfix_engine_print_all(const char *text)
{
	char buf[1024];
	size_t i;
	if (!kick_armed) { qlfix_notice("would print: %s", text); return; }
	snprintf(buf, sizeof buf, "print \"%s\n\"", text);
	for (i = sizeof "print \"" - 1; buf[i] && buf[i + 1]; i++)   /* the text, not the closing quote */
		if (buf[i] == '"') buf[i] = '\'';
	QL_SV_SendServerCommand(NULL, "%s", buf);
}

/* The site cvars are latched: a set made mid-map is held as "latched"
 * (the engine says it "will be changed upon restarting") and lands at the
 * next registration, which register_site_cvars() makes on every module
 * load just before the sites are applied, so the value and the patched
 * bytes never disagree.  The settings (qlfix_maxfps and the other
 * qlfix_maxfps*) patch nothing and are read live. */
#define SITE_CVAR_FLAGS (CVAR_ARCHIVE | CVAR_LATCH)

static void register_site_cvars(void)
{
	size_t i;
	for (i = 0; i < NSITES; i++)
		if (sites[i]->cvar)
			site_cvars[i] = cvar_get(sites[i]->cvar, sites[i]->cvar_default, SITE_CVAR_FLAGS);
	for (i = 0; i < NENGINE; i++)
		if (engine_sites[i]->cvar)
			engine_site_cvars[i] = cvar_get(engine_sites[i]->cvar, engine_sites[i]->cvar_default, SITE_CVAR_FLAGS);
}

/* atoi, kept out of libc: glibc 2.38's headers turn atoi into a call to
 * __isoc23_strtol, which servers on an older glibc do not have.  Base 10,
 * leading space and a sign, like atoi. */
static int parse_int(const char *p)
{
	long v = 0;
	int  neg = 0;
	while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
	if (*p == '+' || *p == '-') neg = *p++ == '-';
	while (*p >= '0' && *p <= '9' && v < 1L << 32) v = v * 10 + (*p++ - '0');
	return (int)(neg ? -v : v);
}

/* qlfix_kick <slot> <reason...>: SV_DropClient with a free-text reason
 * (the stock clientkick hard-codes "was kicked"; kick <name> resolves by
 * name).  The same checks clientkick makes: server running, slot in
 * range, client in use, not the loopback host. */
static void cmd_kick(void)
{
	ql_Cmd_Argc_t argc = QL_Cmd_Argc;
	ql_Cmd_Argv_t argv = QL_Cmd_Argv;
	const cvar_t *running = QL_RPTR(A_cv_com_sv_running);
	const cvar_t *maxcl   = QL_RPTR(A_cv_sv_maxclients);
	uint8_t      *clients = QL_RPTR(A_svs_clients);
	char          reason[128];
	int           n, slot, i;

	if (!kick_armed) { qlfix_notice("qlfix_kick: engine functions not armed"); return; }
	n = argc();
	if (n < 3) { qlfix_notice("usage: qlfix_kick <slot> <reason>"); return; }
	if (!running || !running->integer) { qlfix_notice("qlfix_kick: server is not running"); return; }
	slot = parse_int(argv(1));
	if (slot < 0 || !maxcl || slot >= maxcl->integer || !clients) { qlfix_notice("qlfix_kick: bad slot %s", argv(1)); return; }
	if (*(int *)(clients + (size_t)slot * QL_CLIENT_SIZE + QL_CLIENT_OFF_STATE) == CS_FREE) { qlfix_fps_kick_forget(slot); qlfix_notice("qlfix_kick: slot %d is not in use", slot); return; }
	if (*(int *)(clients + (size_t)slot * QL_CLIENT_SIZE + QL_CLIENT_OFF_NETADRTYPE) == NA_LOOPBACK) { qlfix_fps_kick_forget(slot); qlfix_notice("qlfix_kick: cannot kick the host player"); return; }
	reason[0] = 0;
	for (i = 2; i < n; i++) {
		size_t len = strlen(reason);
		snprintf(reason + len, sizeof reason - len, "%s%s", i > 2 ? " " : "", argv(i));
	}
	if (!qlfix_fps_kick_guard(slot)) return;
	QL_SV_DropClient(clients + (size_t)slot * QL_CLIENT_SIZE, reason);
}

static void cmd_status(void)
{
	/* the whole report prints without the "qlfix: " prefix (the log file
	 * keeps it); the first line says whose report it is */
	qlfix_row("qlfix v" QLFIX_VERSION ", %zu site(s) + %zu engine site(s)", NSITES, NENGINE);
	qlfix_footstep_status();
	qlfix_splash_status();
	qlfix_shotgun_status();
}

static int test_cvar(const char *name, const char *dflt);

static void engine_arm(void)
{
	int st_cvar, st_printf, st_addcmd, st_cbuf, st_send, st_drop, st_argc, st_argv;

	if (engine_tried) return;
	engine_tried = 1;
	{
		/* the auto-kick's settings until (unless) the engine's cvars replace
		 * them below: their defaults, or QLFIX_TEST_CVARS */
		static cvar_t tc[4];
		tc[0].integer = test_cvar("qlfix_maxfps", "250");
		tc[1].integer = test_cvar("qlfix_maxfpsMargin", "-1");
		tc[2].integer = test_cvar("qlfix_maxfpsWindow", "5");
		tc[3].integer = test_cvar("qlfix_maxfpsStrikes", "3");
		qlfix_fps_kick_config(&tc[0], &tc[1], &tc[2], &tc[3]);
	}

	if (!ql_arm_is_engine_process()) {
		qlfix_notice("not running inside qzeroded.x64: engine access off, sites use defaults");
		return;
	}
	/* From here on the fixed addresses are mapped: it is the engine.  The
	 * assumption is that any process named qzeroded.x64 is id's Linux
	 * dedicated server, of which one build exists: the probe reads
	 * A_build_string unconditionally, and the notice prints 40 bytes from
	 * it, so a differently laid out binary under that name would fault here
	 * rather than log a mismatch. */
	if (!ql_arm_build_matches()) {
		qlfix_notice("build string mismatch at 0x%x (\"%.40s\"): engine access off",
		             A_build_string, (const char *)A_build_string);
		return;
	}
	st_cvar   = ql_arm_fn_state(A_Cvar_Get);
	st_printf = ql_arm_fn_state(A_Com_Printf);
	st_addcmd = ql_arm_fn_state(A_Cmd_AddCommand);
	if (!fn_usable(st_cvar) || !fn_usable(st_printf) || !fn_usable(st_addcmd)) {
		qlfix_notice("Cvar_Get %s, Com_Printf %s, Cmd_AddCommand %s: engine access off",
		             fn_state_name(st_cvar), fn_state_name(st_printf), fn_state_name(st_addcmd));
		return;
	}

	cvar_get   = QL_Cvar_Get;
	ql_log_set_console(QL_Com_Printf);
	ql_log_set_clock(sv_time);        /* the file prefix carries sv.time from here on */
	engine_armed = 1;

	/* auto-kick needs five more engine functions to warn and kick; without
	 * them its site still patches and counts strikes, the warnings and
	 * kicks are only logged ("would print", "would run"), qlfix_kick is not
	 * registered, and the rest of engine access is unaffected (minqlx
	 * detours SV_SendServerCommand; fn_usable accepts that) */
	st_cbuf = ql_arm_fn_state(A_Cbuf_AddText);
	st_send = ql_arm_fn_state(A_SV_SendServerCommand);
	st_drop = ql_arm_fn_state(A_SV_DropClient);
	st_argc = ql_arm_fn_state(A_Cmd_Argc);
	st_argv = ql_arm_fn_state(A_Cmd_Argv);
	kick_armed = fn_usable(st_cbuf) && fn_usable(st_send) && fn_usable(st_drop) &&
	             fn_usable(st_argc) && fn_usable(st_argv);
	if (!kick_armed)
		qlfix_notice("Cbuf_AddText %s, SV_SendServerCommand %s, SV_DropClient %s, Cmd_Argc %s, Cmd_Argv %s: auto-kick cannot warn or kick",
		             fn_state_name(st_cbuf), fn_state_name(st_send), fn_state_name(st_drop),
		             fn_state_name(st_argc), fn_state_name(st_argv));

	/* CVAR_ARCHIVE (and CVAR_LATCH for the site cvars), never
	 * CVAR_SERVERINFO: nothing of qlfix's is advertised in the serverinfo
	 * string; the console lines are the record of what was active. */
	cvar_maxfps        = cvar_get("qlfix_maxfps",        "250", CVAR_ARCHIVE);   /* the limit in fps; 0 pauses the kick */
	cvar_maxfpsMargin  = cvar_get("qlfix_maxfpsMargin",  "-1", CVAR_ARCHIVE);   /* % above the limit before it counts; -1 = auto (10 up to 300, 2 above) */
	cvar_maxfpsWindow  = cvar_get("qlfix_maxfpsWindow",  "5",  CVAR_ARCHIVE);   /* seconds of play per strike window */
	cvar_maxfpsStrikes = cvar_get("qlfix_maxfpsStrikes", "3",  CVAR_ARCHIVE);   /* the strike that kicks; the ones before it warn */
	register_site_cvars();
	qlfix_fps_kick_config(cvar_maxfps, cvar_maxfpsMargin, cvar_maxfpsWindow, cvar_maxfpsStrikes);
	QL_Cmd_AddCommand("qlfix_status", cmd_status);
	if (kick_armed) QL_Cmd_AddCommand("qlfix_kick", cmd_kick);

	qlfix_notice("v" QLFIX_VERSION " armed: build \"%s\", Cvar_Get %s, Com_Printf %s, "
	             "Cmd_AddCommand %s, %zu site(s)%s", QL_BUILD_STRING,
	             fn_state_name(st_cvar), fn_state_name(st_printf), fn_state_name(st_addcmd), NSITES + NENGINE,
	             kick_armed ? ", auto-kick available (qlfix_fpsKick)" : "");
}

/* A cvar's value from QLFIX_TEST_CVARS, else its default: for use while
 * engine access is off. */
static int test_cvar(const char *name, const char *dflt)
{
	const char *e = getenv("QLFIX_TEST_CVARS");
	size_t      nl = strlen(name);
	while (e && *e) {
		if (strncmp(e, name, nl) == 0 && e[nl] == '=')
			return parse_int(e + nl + 1);
		e = strchr(e, ',');
		if (e) e++;
	}
	return parse_int(dflt);
}

/* The site's cvar value (0 = off).  Without the engine the site's default
 * applies, unless QLFIX_TEST_CVARS overrides it. */
static int site_mode(const qlfix_site_t *s, const cvar_t *cv)
{
	if (!s->cvar) return 1;
	if (cv)       return cv->integer;
	return engine_armed ? parse_int(s->cvar_default) : test_cvar(s->cvar, s->cvar_default);
}

/* ---- cave ---- */

static void     *cave;
static size_t    cave_size;
static uintptr_t cave_base;      /* module base the current cave serves */

int qlfix_rel32(uintptr_t at, uintptr_t target, int32_t *out)
{
	intptr_t d = (intptr_t)target - (intptr_t)(at + 5);
	if (d > 0x7fffffffLL || d < -0x80000000LL) return 0;
	*out = (int32_t)d;
	return 1;
}

/* A cave of `size` bytes within rel32 reach of the module, at least 1 MiB
 * below it on a MiB boundary.  Mapped RW; apply_set() flips it RW/RX around
 * (re)building. */
static void *cave_alloc(uintptr_t base, size_t size)
{
	uintptr_t hint = (base & ~(uintptr_t)0xfffff) - 0x100000;
	int       tries;

	for (tries = 0; tries < 1024; tries++, hint -= 0x100000) {
		void *p;
		if (hint < 0x100000) break;
		p = mmap((void *)hint, size, PROT_READ | PROT_WRITE,
		         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == MAP_FAILED) continue;
		if ((uintptr_t)p != hint) {          /* old kernel ignored the flag */
			munmap(p, size);
			continue;
		}
		if (base - (uintptr_t)p < 0x7f000000u) return p;
		munmap(p, size);
		break;                               /* already too far: give up */
	}
	return NULL;
}

/* ---- patch ---- */

static int write_code(uintptr_t addr, const void *bytes, size_t len)
{
	uintptr_t pg   = addr & ~(uintptr_t)0xfff;
	size_t    span = (addr + len + 0xfff) / 0x1000 * 0x1000 - pg;

	if (mprotect((void *)pg, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		qlfix_notice("mprotect RWX %#lx: %s", (unsigned long)pg, strerror(errno));
		return 0;
	}
	memcpy((void *)addr, bytes, len);
	if (mprotect((void *)pg, span, PROT_READ | PROT_EXEC) != 0)
		qlfix_notice("mprotect RX %#lx: %s", (unsigned long)pg, strerror(errno));
	__builtin___clear_cache((char *)addr, (char *)addr + len);
	return 1;
}

/* What is at a site right now. */
enum { SITE_ORIGINAL, SITE_OURS, SITE_FOREIGN };

static int site_state(const qlfix_site_t *s, uintptr_t base, uintptr_t cave_lo, uintptr_t cave_hi)
{
	const uint8_t *p = (const uint8_t *)(base + s->offset);
	int32_t rel;
	size_t  i;
	uintptr_t target;

	if (memcmp(p, s->original, s->len) == 0) return SITE_ORIGINAL;
	if (p[0] != 0xe9 || cave_lo == 0) return SITE_FOREIGN;
	for (i = 5; i < s->len; i++)
		if (p[i] != 0x90) return SITE_FOREIGN;
	memcpy(&rel, p + 1, 4);
	target = base + s->offset + 5 + rel;
	return (target >= cave_lo && target < cave_hi) ? SITE_OURS : SITE_FOREIGN;
}

static const char *site_context_check(const qlfix_site_t *s, uintptr_t base)
{
	const uint8_t *p = (const uint8_t *)(base + s->offset);
	if (s->len < 5) return "site too short for a jmp";
	if (s->pre  && memcmp(p - s->pre_len, s->pre, s->pre_len) != 0)   return "context before differs";
	if (s->post && memcmp(p + s->len, s->post, s->post_len) != 0)     return "context after differs";
	return NULL;
}

/*
 * Apply (or, with the cvar at 0, restore) every site.  A module the engine
 * dlclose()d but glibc kept mapped comes back with our jmp already in place
 * and the cave still live; that is SITE_OURS: nothing to do, and the cave
 * is never freed under a live jmp.
 */
static void apply_set(const qlfix_site_t *const *set, cvar_t *const *cvars, size_t nset,
                      uintptr_t base, uintptr_t cave_addr, size_t cave_sz)
{
	size_t      i, j, used = 0;
	uint8_t     jmp[64];
	int         states[NSITES_MAX], on[NSITES_MAX];
	const char *whys[NSITES_MAX], *partner[NSITES_MAX];

	/* Pre-pass: classify every site and reserve the cave space of every
	 * live one before anything is built.  A live server maps the module
	 * afresh each load, so `used` restarts at 0; only a module glibc kept
	 * mapped accumulates reservations across loads that toggle cvars
	 * (a few full rebuilds fit before "cave full"). */
	for (i = 0; i < nset; i++) {
		const qlfix_site_t *s = set[i];
		whys[i]   = site_context_check(s, base);
		states[i] = whys[i] ? SITE_FOREIGN : site_state(s, base, cave_addr, cave_addr + cave_sz);
		on[i]     = site_mode(s, cvars[i]) != 0;
		partner[i] = NULL;
		if (states[i] == SITE_OURS) {
			int32_t rel;
			uintptr_t c;
			memcpy(&rel, (const void *)(base + s->offset + 1), 4);
			c = base + s->offset + 5 + rel;
			if (c - cave_addr + s->cave_max > used) used = c - cave_addr + s->cave_max;
		}
	}
	/* Sites that share a cvar are one fix and patch all or nothing as far
	 * as the bytes go: a footstep site without its ClientEndFrame emitter
	 * would owe footsteps nobody delivers, a radius-loop splash site
	 * without the attribution sites would leave the entity a missile is
	 * stuck in splash only.  A member whose site bytes are foreign takes
	 * its whole group down to the original bytes; one that fails later
	 * (cave full, a build or rel32 failure) is only reported. */
	for (i = 0; i < nset; i++) {
		if (states[i] != SITE_FOREIGN || !on[i]) continue;
		for (j = 0; j < nset; j++)
			if (j != i && on[j] && !partner[j] && set[i]->cvar && set[j]->cvar && strcmp(set[j]->cvar, set[i]->cvar) == 0)
				partner[j] = set[i]->name;
	}

	if (mprotect((void *)cave_addr, cave_sz, PROT_READ | PROT_WRITE) != 0)
		qlfix_notice("mprotect cave RW: %s", strerror(errno));

	for (i = 0; i < nset; i++) {
		const qlfix_site_t *s = set[i];
		const char *why = whys[i];
		uintptr_t   c;
		size_t      n;
		int32_t     rel;
		int         state = states[i];

		if (state == SITE_FOREIGN) {
			qlfix_notice("  %s: skipped (%s at +%#x)", s->name,
			             why ? why : "site bytes differ", s->offset);
			continue;
		}
		if (partner[i]) {
			qlfix_notice("  %s: skipped (%s was skipped; the %s sites patch all or nothing)",
			             s->name, partner[i], s->cvar);
			if (state == SITE_OURS && write_code(base + s->offset, s->original, s->len))
				qlfix_notice("  %s: restored original bytes", s->name);
			continue;
		}

		if (state == SITE_OURS) {
			memcpy(&rel, (const void *)(base + s->offset + 1), 4);
			c = base + s->offset + 5 + rel;
			if (on[i]) {
				qlfix_notice("  %s: already patched (cave %#lx)",
				             s->name, (unsigned long)c);
			} else if (write_code(base + s->offset, s->original, s->len)) {
				qlfix_notice("  %s: restored original bytes (%s 0)", s->name, s->cvar);
			}
			continue;
		}

		/* SITE_ORIGINAL */
		if (!on[i]) {
			qlfix_notice("  %s: disabled (%s 0)", s->name, s->cvar);
			continue;
		}
		if (used + s->cave_max + 16 > cave_sz || s->len > sizeof jmp) {
			qlfix_notice("  %s: skipped (cave full)", s->name);
			continue;
		}
		c = (cave_addr + used + 15) & ~(uintptr_t)15;
		used = c - cave_addr;
		n = s->build_cave((uint8_t *)c, c, base);
		if (n == 0 || n > s->cave_max) {
			qlfix_notice("  %s: skipped (cave build failed, %zu)", s->name, n);
			continue;
		}
		if (!qlfix_rel32(base + s->offset, c, &rel)) {
			qlfix_notice("  %s: skipped (cave out of rel32 range)", s->name);
			continue;
		}
		jmp[0] = 0xe9;
		memcpy(jmp + 1, &rel, 4);
		memset(jmp + 5, 0x90, s->len - 5);
		if (!write_code(base + s->offset, jmp, s->len)) continue;
		used += n;
		qlfix_notice("  %s: patched (+%#x -> cave %#lx, %zu bytes)",
		             s->name, s->offset, (unsigned long)c, n);
	}

	__builtin___clear_cache((char *)cave_addr, (char *)cave_addr + cave_sz);
	if (mprotect((void *)cave_addr, cave_sz, PROT_READ | PROT_EXEC) != 0)
		qlfix_notice("mprotect cave RX: %s", strerror(errno));
}

static void apply_engine_sites(void);

static void on_module_loaded(const char *path, void *handle)
{
	struct stat st;
	void       *entry;
	uintptr_t   base;

	engine_arm();
	if (engine_armed)
		register_site_cvars();   /* a latched site switch lands here */
	apply_engine_sites();

	/* Whatever happens below, the helpers that read the module's globals
	 * (qlfix_status's level.clients, splash attribution's g_entities) must
	 * point at THIS module or at nothing: a stale address from a previous
	 * load is unmapped memory. */
	if (stat(path, &st) != 0) st.st_size = -1;
	if (st.st_size != QA_MODULE_SIZE) {
		qlfix_notice("%s: size %lld, expected %d: not patching", path,
		             (long long)st.st_size, QA_MODULE_SIZE);
		qlfix_footstep_module(0);
		qlfix_splash_config(0);
		return;
	}
	entry = dlsym(handle, "dllEntry");
	if (!entry) {
		qlfix_notice("%s: no dllEntry symbol: not patching", path);
		qlfix_footstep_module(0);
		qlfix_splash_config(0);
		return;
	}
	base = (uintptr_t)entry - QA_DLLENTRY;
	/* a map change clears sv (SV_ClearServer) before it loads the module,
	 * so sv.time is 0; a map_restart reloads it with sv.time running, and
	 * the players' strikes and classifier states carry on.  Without the
	 * engine every load counts as a new map. */
	if (!engine_armed || sv_time() == 0) qlfix_footstep_new_map();
	qlfix_footstep_module(base);
	qlfix_splash_config(base);

	/* A cave serving a different base belongs to a module that is gone (the
	 * same path cannot be mapped twice), so it can be released.  One serving
	 * this base is kept: the module may still carry jmps into it. */
	if (cave && cave_base != base) {
		munmap(cave, cave_size);
		cave = NULL;
	}
	if (!cave) {
		cave_size = QLFIX_CAVE_SIZE;
		cave = cave_alloc(base, cave_size);
		if (!cave) {
			qlfix_notice("%s: no cave page within reach of base %#lx: not patching",
			             path, (unsigned long)base);
			return;
		}
		cave_base = base;
	}
	/* serverId marks each reload (it bumps on a map change and on a level
	 * restart alike); sv.time in the prefix tells them apart: it drops to
	 * near 0 on a map change and runs on through a restart */
	if (engine_armed)
		qlfix_notice("%s base %#lx cave %#lx, serverId %d", QA_MODULE_NAME,
		             (unsigned long)base, (unsigned long)cave, QL_R32(A_sv_serverId));
	else
		qlfix_notice("%s base %#lx cave %#lx", QA_MODULE_NAME,
		             (unsigned long)base, (unsigned long)cave);
	apply_set(sites, site_cvars, NSITES, base, (uintptr_t)cave, cave_size);
	{
		/* what the auto-kick cvars amount to, once per load: the switch as
		 * latched for this map, the settings as they stand (they are read
		 * live: a change to the limit or margin shows in qlfix_status at
		 * once, and every change shows here at the next module load) */
		char   d[160];
		size_t i;
		for (i = 0; i < NSITES; i++)
			if (sites[i] == &qlfix_site_fps) qlfix_fps_kick_switch(site_mode(sites[i], site_cvars[i]) != 0);
		qlfix_notice("  %s", qlfix_fps_kick_describe(d, sizeof d));
	}
}

/*
 * Engine sites.  The executable is non-PIE, so base is 0 and the cave is
 * one page, mapped below its text (0x400000) once and kept.  Applied on
 * every module load like the module's own sites (a cvar change takes effect
 * at the next map change or map_restart), but only once engine_arm() has
 * armed engine access (the build string and the Cvar_Get, Com_Printf and
 * Cmd_AddCommand prologues); without it nothing here runs.
 */
static void *engine_cave;
static size_t engine_cave_size;

static void apply_engine_sites(void)
{
	if (!engine_armed) return;
	if (!engine_cave) {
		engine_cave_size = 0x1000;
		engine_cave = cave_alloc(0x400000, engine_cave_size);
		if (!engine_cave) {
			qlfix_notice("no cave page within reach of the engine: engine sites off");
			return;
		}
	}
	apply_set(engine_sites, engine_site_cvars, NENGINE, 0, (uintptr_t)engine_cave, engine_cave_size);
}

/* ---- dlopen ---- */

static int is_module_path(const char *path)
{
	const char *slash;
	if (!path) return 0;
	slash = strrchr(path, '/');
	return strcmp(slash ? slash + 1 : path, QA_MODULE_NAME) == 0;
}

typedef void *(*dlopen_t)(const char *, int);

__attribute__((visibility("default")))
void *dlopen(const char *filename, int flags)
{
	static dlopen_t real;
	static int      disabled = -1;
	void           *h;

	if (!real) {
		real = (dlopen_t)dlsym(RTLD_NEXT, "dlopen");
		if (!real) {
			fprintf(stderr, "qlfix: dlsym(RTLD_NEXT, dlopen) failed: %s\n", dlerror());
			abort();
		}
	}
	if (disabled < 0) {                  /* may run before our constructor */
		const char *e = getenv("QLFIX_DISABLE");
		disabled = e && *e && *e != '0';
	}

	/* glibc takes the caller of dlopen from the return address and
	 * searches that object's RPATH/RUNPATH and $ORIGIN for a bare name.
	 * Everything but the game module is passed on as a tail call (a jmp),
	 * so glibc still sees the real caller.  The module is opened by
	 * absolute path, where the caller does not matter, and is patched
	 * after the call returns. */
	if (disabled || (flags & RTLD_NOLOAD) || !is_module_path(filename))
		return real(filename, flags);
	h = real(filename, flags);
	if (h) on_module_loaded(filename, h);
	return h;
}

/* The log opens at load, in any process. */
__attribute__((constructor))
static void qlfix_init(void)
{
	qlfix_log_open();
}
