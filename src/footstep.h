/*
 * footstep.h -- what the three footstep files share: the per-slot record,
 * the windows and thresholds, and the helpers each file needs from another.
 * Internal to qlfix (every symbol is hidden); the public entry points are
 * in qlfix.h.
 *
 *   footstep_tick.c    the classifier and the shadow-clock delivery: the
 *                      bob site's target and ClientEndFrame's
 *   fps_kick.c         auto-kick: the per-command fps ring from the
 *                      ClientThink site, windows, strikes, the kick guard
 *   footstep_status.c  sessions and reporting: session reset, names,
 *                      level.clients access, the qlfix_status table
 */
#ifndef QLFIX_FOOTSTEP_H
#define QLFIX_FOOTSTEP_H

#include <stdint.h>
#include "qlfix.h"
#include "ql_types.h"
#include "qagame_syms.h"

/* Inlining is decided here, not left to the compiler's size heuristics,
 * which cannot tell a per-command path from a per-window one: FS_HOT marks
 * what runs on every command and is always inlined into the tick, FS_COLD
 * what runs on an event (a change, a window, a line of log) and stays out
 * of line so the hot code does not carry its bytes.  The compiled ticks
 * make no call on the per-command path: everything they call sits on an
 * event path. */
#define FS_HOT   static inline __attribute__((always_inline))
#define FS_COLD  __attribute__((noinline, cold))

#define WINDOW      512          /* samples kept per slot (the slow tier's window) */
#define FAST_W      64           /* the fast tier's window (also the log line's) */
#define MAX_SLOTS   64
#define MSEC_MAX    200          /* PmoveSingle clamps pml.msec to [1,200] */
#define LOG_INTERVAL 60000       /* ms of commandTime between console lines for one slot's classifier changes
                                  * (the changes between them are trace lines; the console line carries the count) */
#define LOST_FAST   16           /* lost commands among the last 64 the fast tier needs (real 250 fps limiters: 5 at most) */
#define LOST_SLOW   64           /* ... among the last 512 the slow tier needs: real 250 fps limiters lose 20-37,
                                  * the quiet caps 96 and up */
#define LOST_CLEAR  48           /* ... below which a FORCED slot clears: under LOST_SLOW, so a stream hovering at the gate
                                  * does not flip on every crossing; still above every real 250 fps limiter */
#define FAST_TRIP   205          /* 64-cmd cadence below this forces at once */
#define SLOW_TRIP   258          /* slow tier: 512-cmd cadence below this forces */
#define CLEAR       275          /* slow tier: 512-cmd cadence above this clears */
#define FORCED_RATE 270          /* a FORCED slot's shadow clock, in units/s times the gait's 4 ms advance: what a real
                                  * 250 fps client measures (its 4/5 ms jitter, ~474 ms between running footsteps);
                                  * between SLOW_TRIP and CLEAR, so forced is never quieter than 250 fps */
#define REST_ON     56           /* a locked cycle rests mid-half once the last FAST_W commands advanced it this much or
                                  * less (under 1 a command: a 64-command prediction from mid-half cannot reach a
                                  * boundary, 64 units on) ... */
#define REST_OFF    64           /* ... and goes back to the boundary once they advance it this much: from mid-half a
                                  * prediction over those commands could reach a boundary (real 363-380 caps running:
                                  * 98 % of 64-command windows at or under REST_ON, the rest hitches) */
#define NEAR_INTERVAL 30000      /* ms between "near the line" samples for one slot: a sampling rate for tuning data
                                  * (file only), unrelated to LOG_INTERVAL; 40 samples in a 20-minute match */
#define NEAR_CADENCE  10         /* STOCK slot within this above SLOW_TRIP (or below it) ... */
#define NEAR_LOST     6          /* ... and within this below LOST_SLOW (or above): logged; a FORCED slot kept only
                                  * by the hysteresis (LOST_CLEAR to LOST_SLOW lost) is logged at the same rate */


enum { ST_STOCK = 0, ST_FORCED = 1 };

/* Who makes a slot's footsteps (pend.drv).  STOCK: stock's crossings, the
 * cycle stock's.  SHADOW: the shadow clock, the cycle locked from pend.bob
 * (on it or mid-half; it also carries a slot that has left FORCED until the
 * release, footstep_tick.c's DELIVERY).  Owned by footstep_tick.c. */
enum { DRV_STOCK = 0, DRV_SHADOW };

/* The log's view of a slot's transitions (classifier state):
 * every one is a file line, the console gets at most one per LOG_INTERVAL
 * per slot carrying the count since the last.  footstep_tick.c's
 * log_transition() owns it. */
typedef struct {
	int count;                   /* transitions of this player in the slot (kept across its sessions) */
	int unlogged;                /* since the last console line */
	int lastLogTime;             /* commandTime of that line */
} fold_t;

/* One client slot.  Three owners, each a sub-struct (the classifier's
 * ground ring a fourth), the per-command scalars first and the 512-byte
 * rings last so that a ground command touches the top, cls and pend scalars
 * in a few adjacent cache lines and two entries of each ring (the one
 * written and the one leaving the fast window); the kick's scalars sit with
 * its ring.  The groups are 64-byte aligned, so slot_t (2.5 KB with the
 * alignment) is a whole number of cache lines and every slot's groups start
 * on a line boundary whatever the slot index. */
typedef struct {
	/* session: both ticks, status */
	unsigned long long steamId;  /* pers.steamId of the session's player */
	int  seen;                   /* slot has been used since reset */
	int  enterTime;              /* pers.enterTime of the session this slot tracks */

	/* cls: the footstep classifier over the ground ring (footstep_tick.c) */
	struct {
		long ms64, ms512;        /* msec over the last FAST_W and WINDOW commands */
		long sa64, sa512;        /* ... the cycle's advance over those of them that sound (running dry, any gait
		                          * wading or waist-deep) */
		long sd64, sd512;        /* ... a steady 4 ms player's over the same time at the same gaits, in quarter
		                          * units */
		int  ln64, ln512;        /* ... how many of the window's commands, sounding or not, are LOST: shorter than
		                          * 4 ms and short of their share of a 4 ms command's advance */
		long sinceChange;        /* commands since the last change (dwell) */
		int  n, pos;             /* samples held (<= WINDOW), next write; the ring itself is `ring` below */
		int  state;              /* ST_STOCK / ST_FORCED */
		int  dwell;              /* commands the current state must hold (512 after a slow-tier force, else 64) */
		int  lastNearTime;       /* commandTime of the last "near the line" or "held" sample */
		fold_t fold;             /* classification changes and their console fold */
	} __attribute__((aligned(64))) cls;

	/* pend: the shadow clock and the owed footstep (footstep_tick.c, the end-frame helper) */
	struct {
		long pos;                /* the shadow's phase since its last footstep, 0..128 units, in 1/PEND_Q units */
		long emitted;            /* footsteps emitted */
		int  drv;                /* who makes the footsteps: DRV_STOCK, DRV_SHADOW (footstep_tick.c) */
		int  bob;                /* the boundary the cycle is locked from (64 or 192) */
		int  rest;               /* the lock is written mid-half, bob + 64, where the view bob and the weapon sway
		                          * are at rest; else on bob.  Any value but the one written, seen on the next
		                          * command, was set by someone else (a respawn's zeroed playerstate, standing
		                          * still, a landing), and the slot starts over from it */
		int  advSum, advPos;     /* the cycle's stock advance over the last FAST_W commands, every gait, and `adv`'s
		                          * next write */
		int  lastTime;           /* commandTime of the slot's last ground command (a gap: a stop, a respawn, the air) */
		int  footTime, footKnown; /* commandTime of the last footstep, stock's or the shadow's, if ground commands
		                          * ran unbroken since */
		int  stepped;            /* the shadow's last command made its footstep (the release waits for one) */
		int  owed, owedEvent, owedTime;   /* a footstep for ClientEndFrame to emit (at most one) */
		int  lastEmitEvent, lastEmitTime; /* the last footstep emitted and the commandTime it was owed at (inspection) */
		unsigned char adv[FAST_W];       /* per command, its stock advance (capped at 255) */
	} __attribute__((aligned(64))) pend;

	/* kick: auto-kick over every command in play (fps_kick.c) */
	struct {
		long ms512;              /* sum of the ring */
		long ms, cmds;           /* current window: ms and commands */
		int  n, pos;             /* samples held, next write, in `ring` below */
		int  short64;            /* <= 2 ms commands among the last FAST_W */
		int  start;              /* serverTime the window opened at (0 = none open) */
		int  lastCmd;            /* serverTime of the last command that advanced it (0 = none yet): each command's msec */
		int  peakMs;             /* shortest 512-command span (ms) in the current window: its peak fps */
		int  burstShort, burstFps;   /* this window's worst burst: its short count of 64 (0 = none) and the fps over those 64 */
		int  strikes, kicked;    /* windows over the trip (cleared after CLEAN_RUN clean ones); kick issued */
		int  cleanRun;           /* consecutive clean windows so far */
		int  grace;              /* the next window follows the final warning: judged by its tail */
		int  fps;                /* fps of the last judged window (status): its peak, or a counted burst's
		                          * 64-command fps if higher */
		int  pending;            /* a kick command is queued for this slot ... */
		int  pendingEnterTime;   /* ... for the session with this pers.enterTime */
		unsigned char ring[WINDOW];   /* msec of the last WINDOW commands in play */
	} __attribute__((aligned(64))) kick;

	/* the ground ring: one entry per command on the ground and moving */
	struct {
		unsigned char msec[WINDOW];  /* msec per command, clamped 1..200 */
		unsigned char snd[WINDOW];   /* bit 0: the command's gait and medium sound at a crossing; bit 1: it is lost */
		unsigned char act[WINDOW];   /* the cycle's advance if the command sounds (0..127), | 0x80 crouching (2 units a 4 ms command) */
	} __attribute__((aligned(64))) ring;
} slot_t;

/* footstep_tick.c */
extern slot_t fs_slots[MAX_SLOTS];
extern long fs_emitted, fs_nospawn, fs_stale;   /* since load: footsteps emitted; G_TempEntity returned nothing; dropped, their session over */

/* fps_kick.c */
int  fs_kick_limit(void);                   /* qlfix_maxfps while qlfix_fpsKick is on, else 0 */
int  fs_kick_trip(int limit);               /* the fps a strike needs, limit plus margin */

/* footstep_status.c */
void fs_level_module(uintptr_t base);       /* where level and sv_maxclients live this load; 0 = none */
int  fs_client_field(int cn, int off, int *out);   /* an int of level.clients[cn]; 0 when unknown */
FS_COLD const char *fs_client_name(int cn, char out[QA_NETNAME_LEN + 1]);   /* log lines and status */
FS_COLD const char *fs_state_name(int st);
FS_COLD int  fs_slot_begin(slot_t *s, const int *ps);   /* pers.enterTime changed: 1 = the same player's new session, 0 = another player */

/* Shared by both ticks on every command: a new session in this slot
 * (reconnect, slot reuse, team join, map_restart: pers.enterTime changed)
 * ends the old one, keeping the player's state if the Steam ID is the same
 * (fs_slot_begin); a map change has already cleared every slot
 * (qlfix_footstep_new_map), as has any reload while engine access is off.
 * Inline, so the common case is a load and a compare in the tick; only the
 * change crosses into footstep_status.c.  ps is the gclient_t read as ints:
 * pers.enterTime lies 0x2f4 past the start of the playerState_t, which
 * works because ps is gclient_t's first member, so a caller must hand over
 * at least QA_GCLIENT_pers_enterTime + 4 bytes. */
FS_HOT void fs_slot_session(int cn, slot_t *s, const int *ps)
{
	int et = ps[QA_GCLIENT_pers_enterTime / 4];
	if (__builtin_expect(!s->seen || et != s->enterTime, 0)) fs_slot_begin(s, ps);
	s->enterTime = et;
	s->seen = 1;
}

/* Stock cadence (units/s) over the fast and slow windows; -1 while a window
 * has not filled. */
static inline int cad64(const slot_t *s)  { return s->cls.n >= FAST_W && s->cls.sd64  > 0 ? (int)(1000 * s->cls.sa64  / s->cls.sd64)  : -1; }
static inline int cad512(const slot_t *s) { return s->cls.n >= WINDOW && s->cls.sd512 > 0 ? (int)(1000 * s->cls.sa512 / s->cls.sd512) : -1; }
static inline int fps64(const slot_t *s)  { return s->cls.n >= FAST_W && s->cls.ms64  > 0 ? (int)(1000 * FAST_W / s->cls.ms64)  : -1; }

#endif
