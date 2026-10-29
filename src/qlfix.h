/*
 * qlfix.h -- runtime byte patches for Quake Live's closed qagamex64.so.
 *
 * A site is one contiguous run of instructions in the module that is
 * replaced by `jmp rel32` to a code cave (plus nop padding); the cave does
 * the corrected work and jumps back.  Sites are applied on every load of
 * the module (the engine dlopen()s it on every map change and map_restart)
 * after fingerprinting the original bytes, so a different qagame build is
 * left untouched.  Engine sites work the same way against the fixed-address
 * qzeroded.x64 (base 0); they are applied at module load too, once engine
 * access is armed (ql_arm.h).
 */
#ifndef QLFIX_H
#define QLFIX_H

#include <stddef.h>
#include <stdint.h>

#include "qagame_syms.h"            /* the module's offsets: functions, GOT slots, struct layouts */
#include "qlfix_log.h"
#define QLFIX_CAVE_SIZE     0x2000      /* the module cave: two pages, nine caves and room for the rebuilds a kept
                                         * module accumulates */

typedef struct qlfix_site_s {
	const char    *name;
	const char    *cvar;          /* the switch: its value 0 = original bytes, else patched; NULL = always patched */
	const char    *cvar_default;
	uint32_t       offset;        /* module offset of the first replaced byte */
	uint32_t       len;           /* bytes replaced (>= 5) */
	const uint8_t *original;      /* the len bytes expected there */
	const uint8_t *pre;           /* context bytes expected just before */
	uint32_t       pre_len;
	const uint8_t *post;          /* context bytes expected just after */
	uint32_t       post_len;
	/* Assemble the cave at out (which will live at out_addr) for a module at
	 * base.  Returns bytes written, 0 if a rel32 would not reach. */
	size_t       (*build_cave)(uint8_t *out, uintptr_t out_addr, uintptr_t base);
	size_t         cave_max;      /* upper bound on build_cave's output */
} qlfix_site_t;

extern const qlfix_site_t qlfix_site_footstep;
extern const qlfix_site_t qlfix_site_splash;              /* site_splash.c: A */
extern const qlfix_site_t qlfix_site_splash_surface;      /* B */
extern const qlfix_site_t qlfix_site_missile_stuck;       /* D (before C in G_RunMissile) */
extern const qlfix_site_t qlfix_site_missile_attribute;   /* C */
extern const qlfix_site_t qlfix_site_shotgun_origin;      /* site_shotgun.c */
extern const qlfix_site_t qlfix_site_fps;                 /* site_fps.c: ClientThink, every command, auto-kick */
extern const qlfix_site_t qlfix_site_footstep_emit;       /* site_endframe.c: ClientEndFrame, the footstep delivery */
extern const qlfix_site_t qlfix_site_dead_holdable;       /* site_holdable.c: ClientEvents, no holdable for a dead player */
/* engine sites: offset is an absolute address in qzeroded.x64 (base 0) */
extern const qlfix_site_t qlfix_site_curve_trace;         /* site_curve.c */

/* rel32 from the byte after a 5-byte jmp/call at `at` to `target`.
 * Returns 0 and leaves *out untouched if out of range. */
int qlfix_rel32(uintptr_t at, uintptr_t target, int32_t *out);

void qlfix_engine_command(const char *line);   /* queue a console command for the engine's next frame */
void qlfix_engine_print_all(const char *text);  /* a `print` server command to every client, as qagame's PrintMsg sends */

/* ---- runtime: the sites' targets and the loader's hooks ---- */

/* footstep_tick.c */
int  qlfix_footstep_tick(int *ps, float bobmove, int msec, const uint8_t *pm, int footstep);   /* the footstep site's target */
void qlfix_footstep_endframe(uint8_t *ent);          /* site_endframe.c's target: emit the owed footstep */
void qlfix_footstep_module(uintptr_t base);          /* on_module_loaded, every load: where the game's globals live; 0 = none */
void qlfix_footstep_new_map(void);                   /* footstep_status.c: a map change, every slot starts clean (not a map_restart) */
/* fps_kick.c */
struct cvar_s;
void qlfix_fps_tick(int *client, const int *ent, const uint8_t *level);   /* the fps site's target: every command of every client */
void qlfix_fps_kick_config(const struct cvar_s *limit, const struct cvar_s *margin, const struct cvar_s *window,
                                const struct cvar_s *strikes);
void qlfix_fps_kick_switch(int on);                   /* qlfix_fpsKick's value at this module load */
const char *qlfix_fps_kick_describe(char *buf, size_t n);   /* "auto-kick: fps limit L, strikes above T ..." for the console */
int  qlfix_fps_kick_guard(int slot);           /* qlfix_kick: 0 = slot changed hands since the auto-kick was queued */
void qlfix_fps_kick_forget(int slot);          /* qlfix_kick gave up before the guard: drop the queued kick's record */
/* splash_attribute.c */
void qlfix_splash_config(uintptr_t base);            /* on_module_loaded, every load: where g_entities lives; 0 = none */
/* what the missile-stuck site saves from G_RunMissile's moving trace, for
 * missile-attribute a few instructions later; defined in site_splash.c,
 * read in splash_attribute.c */
typedef struct { int32_t entityNum; int32_t pad; const void *missile; } qlfix_splash_stuck_t;
extern qlfix_splash_stuck_t qlfix_splash_stuck;
void qlfix_splash_attribute(uint8_t *missile, int32_t *trace);   /* the missile-attribute site's target */
/* site_shotgun.c */
void *qlfix_shotgun_nudge(uint8_t *tent);            /* the shotgun site's target; returns tent */

/* ---- qlfix_status: one report per feature (footstep_status.c, splash_attribute.c, site_shotgun.c) ---- */
void qlfix_footstep_status(void);
void qlfix_splash_status(void);
void qlfix_shotgun_status(void);

/* ---- inspection (from a debugger): nothing at run time calls these.
 *      The per-slot ones return -1 for an unknown slot (last_emit 0); the
 *      last two are counters. ---- */
int  qlfix_footstep_state(int cn);                   /* 0 STOCK, 1 FORCED */
int  qlfix_footstep_driver(int cn);                  /* 0 stock, 1 locked on the shadow clock */
int  qlfix_footstep_owed(int cn);                    /* 1 = a footstep waits for ClientEndFrame */
long qlfix_footstep_emitted(int cn);                /* footsteps this slot has emitted */
int  qlfix_footstep_last_emit(int cn, int *commandTime);   /* the last emitted event (0 = none) and when it was owed */
int  qlfix_fps_last(int cn);                         /* msec of the last command measured in play (capped at 200), -1 none */
long qlfix_splash_attributions(void);                /* since load */
long qlfix_shotgun_nudged(void);                     /* since load */

#endif
