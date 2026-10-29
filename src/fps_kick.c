/*
 * fps_kick.c -- auto-kick (qlfix_fpsKick, limit qlfix_maxfps): its own
 * feature, sharing the slot table and the sessions with the footstep files.
 *
 * AUTO-KICK (qlfix_fpsKick, off by default) is fed by qlfix_fps_tick from
 * its own site in ClientThink_real (site_fps.c) that sees every command of
 * every client, airborne and idle included, not just the ground commands
 * the footstep site sees; it measures only clients in play.  The two rings
 * are separate: the classifier must judge footstep-producing commands only
 * (fed from every command, a client running slow in the air and fast on the
 * ground would look loud while silent on the ground), and the limit must
 * judge every command in play.  A window opens at a slot's first command in
 * play and closes once it holds qlfix_maxfpsWindow seconds of play (the
 * summed msec of its commands, so a spell out of play pauses it) and at
 * least 512 commands.  It is a strike when its fastest 512-command stretch
 * is over the trip (a window mean would let a short burst hide inside a
 * capped window) OR a burst fired inside it: BURST_SHORT of the ring's last
 * 64 commands at 2 ms or shorter, a stretch of ~445+ fps frames that no
 * real cap produces and that is too short for the 512-command stretch to
 * see.  CLEAN_RUN clean windows in a row clear the count (a single one
 * would let a toggler bursting every other window escape).
 *
 * The window after the FINAL warning is judged by its last 512 commands
 * rather than its peak: the warning is queued at window close and the next
 * window is already running, so a player who caps after the warning must
 * not be removed; the following window uses the peak rule again.  Warning
 * and kick are QUEUED (a print server command, Cbuf_AddText): this code
 * runs inside the client's own ClientThink, where dropping it would free
 * the entity we are in.
 */
#include <stdio.h>
#include "footstep.h"

#define CLEAN_RUN     3          /* consecutive clean windows that clear the fps strikes */
#define BURST_SHORT  48          /* the burst: this many of the last 64 commands <= 2 ms, ~445 fps frames; the real 380
                                  * cap's worst 64-command window has 30 (363 and 366: 26, 350: 21, 333: 16, 250: 5) */

static const cvar_t *kick_limit, *kick_margin, *kick_window, *kick_strikes;
static int           kick_on;    /* qlfix_fpsKick as of this module load (latched, like the site) */

void qlfix_fps_kick_config(const cvar_t *limit, const cvar_t *margin, const cvar_t *window, const cvar_t *strikes)
{
	kick_limit = limit; kick_margin = margin; kick_window = window; kick_strikes = strikes;
}

void qlfix_fps_kick_switch(int on) { kick_on = on; }

/* the limit in force: qlfix_maxfps, read live; 0 while the kick is off or paused */
int fs_kick_limit(void) { return kick_on && kick_limit ? kick_limit->integer : 0; }

/* The margin is measurement noise, not policy, and no single percentage
 * fits every limit: an honest cap's fastest 512-command stretch runs a
 * few percent over its nominal rate (clock nudges), and the common caps
 * above 300 sit within a few percent of each other.  qlfix_maxfpsMargin
 * -1 (the default) is auto: 10 % up to a 300 limit, 2 % above. */
static int kick_margin_auto(void) { return !kick_margin || kick_margin->integer < 0; }
static int kick_margin_pct(int limit)
{
	if (kick_margin_auto()) return limit > 300 ? 2 : 10;
	return kick_margin->integer;
}
int fs_kick_trip(int limit) { return (limit * (100 + kick_margin_pct(limit)) + 50) / 100; }   /* rounded: 333 + 2 % = 340 */

/* One line for the console: what the current cvars amount to. */
const char *qlfix_fps_kick_describe(char *buf, size_t n)
{
	int limit   = fs_kick_limit();
	int window  = kick_window  ? kick_window->integer  : 5;
	int strikes = kick_strikes ? kick_strikes->integer : 3;
	if (!kick_on)   { snprintf(buf, n, "auto-kick off (qlfix_fpsKick 0)"); return buf; }
	if (limit <= 0) { snprintf(buf, n, "auto-kick paused (qlfix_maxfps 0)"); return buf; }
	snprintf(buf, n, "auto-kick: fps limit %d, strikes above %d (margin %d percent%s), %d s windows, kick on strike %d",
	         limit, fs_kick_trip(limit), kick_margin_pct(limit), kick_margin_auto() ? " auto" : "",
	         window < 1 ? 1 : window, strikes < 1 ? 1 : strikes);
	return buf;
}

/* One window of play has completed for the slot: is it over the limit?
 * SUSTAINED is the window's peak, or its last 512 commands in the window
 * after the final warning (see the header).  BURST is BURST_SHORT of the
 * ring's last 64 commands <= 2 ms somewhere in the window, ~445+ fps frames
 * that no real cap produces (the 380 cap peaks at 30 of 64).  A burst's own
 * 64-command fps is not a frame rate the player would recognise (48 frames
 * of 2 ms with 16 of 8 ms average 286 fps and still make the count), so a
 * burst strike is worded by the short frames seen, and only counts while
 * 500 fps is itself over the trip. */
FS_COLD static void kick_check(int cn, slot_t *s, long elapsed, long kMs, long kCmds, int tail)
{
	int  limit = fs_kick_limit();
	int  strikes, trip, mean, peak, sustained, burst, fps, grace;
	char name[QA_NETNAME_LEN + 1], line[256];

	if (limit <= 0 || s->kick.kicked || kMs <= 0) return;
	grace = s->kick.grace; s->kick.grace = 0;
	strikes = kick_strikes ? kick_strikes->integer : 3;
	if (strikes < 1) strikes = 1;
	trip = fs_kick_trip(limit);
	mean = (int)(1000 * kCmds / kMs);
	peak = s->kick.peakMs ? (int)(1000L * WINDOW / s->kick.peakMs) : mean;
	burst = s->kick.burstShort >= BURST_SHORT && trip < 500;
	/* the window after the final warning kicks only if the player is STILL
	 * over at its end: its last 512 commands, or a burst */
	sustained = grace ? tail : peak;
	s->kick.fps = burst && s->kick.burstFps > peak ? s->kick.burstFps : peak;   /* status: the window's peak, or a counted burst's 64-command fps if higher */
	if (sustained <= trip && !burst) {
		if (grace && peak > trip) {
			qlfix_notice("slot %d %s capped within the window after the last warning (peak %d, last 512 commands %d fps): no strike",
			             cn, fs_client_name(cn, name), peak, tail);
			s->kick.cleanRun = 0;                      /* neither a strike nor a clean window */
			return;
		}
		/* a clean window does not wipe the count on its own: a toggler who
		 * bursts every other window would never accumulate.  CLEAN_RUN clean
		 * windows in a row (at least 15 s of play by default) do. */
		if (++s->kick.cleanRun >= CLEAN_RUN && s->kick.strikes) {
			qlfix_notice("slot %d %s fps strikes cleared after %d clean windows", cn, fs_client_name(cn, name), s->kick.cleanRun);
			s->kick.strikes = 0;
		}
		return;
	}
	s->kick.cleanRun = 0;
	s->kick.strikes++;
	fps = sustained > trip ? sustained : 0;       /* 0: a burst-only strike, no frame rate to quote */
	fs_client_name(cn, name);
	{
		char b[128] = "";
		if (s->kick.burstShort)
			snprintf(b, sizeof b, "; burst: %d of 64 commands <= 2 ms (~%d fps over the 64)%s",
			         s->kick.burstShort, s->kick.burstFps, burst ? "" : ", not counted (trip >= 500)");
		qlfix_notice("slot %d %s over the fps limit (%s): peak %d fps over 512 commands (window mean %d over %ld s, tail %d; limit %d, trip %d)%s, strike %d of %d%s",
		             cn, name, fps ? (grace ? "tail" : "sustained") : "burst", peak, mean, elapsed / 1000, tail, limit, trip, b,
		             s->kick.strikes, strikes, s->kick.strikes >= strikes ? "; kicking" : "");
	}
	if (s->kick.strikes < strikes) {
		/* A print to everyone, the way the game makes announcements. */
		if (fps)
			snprintf(line, sizeof line,
			         "^1%s^7 is at %d fps, over this server's limit of %d (strike %d of %d).",
			         name, fps, limit, s->kick.strikes, strikes);
		else
			snprintf(line, sizeof line,
			         "^1%s^7 spiked over 500 fps, this server's limit is %d (strike %d of %d).",
			         name, limit, s->kick.strikes, strikes);
		qlfix_engine_print_all(line);
		if (s->kick.strikes == strikes - 1) s->kick.grace = 1;   /* the last warning: the next window is judged by its tail */
	} else {
		s->kick.kicked = 1;
		s->kick.pending = 1;                       /* qlfix_kick checks the session still matches */
		s->kick.pendingEnterTime = s->enterTime;
		if (fps)
			snprintf(line, sizeof line, "qlfix_kick %d was kicked: frame rate %d fps is over this server's limit of %d",
			         cn, fps, limit);
		else
			snprintf(line, sizeof line, "qlfix_kick %d was kicked: spiked over 500 fps, this server's limit is %d",
			         cn, limit);
		qlfix_engine_command(line);
	}
}

/* Called by qlfix_kick before dropping: 1 = go ahead, 0 = the slot has
 * changed hands since the auto-kick was queued (the target's disconnect
 * and a newcomer's connect both landed in the frame between queueing and
 * execution), so the command would hit the wrong player.  Manual kicks
 * (no pending record) always pass. */
int qlfix_fps_kick_guard(int slot)
{
	slot_t *s;
	int     et;
	if (slot < 0 || slot >= MAX_SLOTS) return 1;
	s = &fs_slots[slot];
	if (!s->kick.pending) return 1;
	s->kick.pending = 0;
	if (fs_client_field(slot, QA_GCLIENT_pers_enterTime, &et) && et != s->kick.pendingEnterTime) {
		qlfix_notice("slot %d: changed hands since the kick was queued (enterTime %d -> %d), not kicking",
		             slot, s->kick.pendingEnterTime, et);
		return 0;
	}
	return 1;
}

/* Called by qlfix_kick when it gives up before the guard (slot free, or
 * the host): the queued auto-kick is spent, so its record must not
 * refuse the next manual kick on this slot. */
void qlfix_fps_kick_forget(int slot)
{
	if (slot >= 0 && slot < MAX_SLOTS) fs_slots[slot].kick.pending = 0;
}

/* Close the slot's strike window: the next command opens a new one. */
FS_HOT void window_reset(slot_t *s)
{
	s->kick.start = 0; s->kick.ms = 0; s->kick.cmds = 0; s->kick.peakMs = 0; s->kick.burstShort = 0;
}

/* The open window ends: judged if it holds enough commands to have a peak,
 * else dropped.  A window holds commands in play only, and its length is
 * their time (kick.ms), not the clock's. */
FS_COLD static void window_close(int cn, slot_t *s)
{
	if (s->kick.cmds >= WINDOW) kick_check(cn, s, s->kick.ms, s->kick.ms, s->kick.cmds, (int)(1000L * WINDOW / s->kick.ms512));
	window_reset(s);
}

/* Every command of every client, from the ClientThink_real site
 * (site_fps.c): client = the gclient (ps first), ent = its gentity, level =
 * &level.  Feeds the auto-kick only.  Nothing is read from the playerstate:
 * a spectator following someone carries a copy of the followed player's
 * (qagame 0x5bd68 copies 0x250 bytes, then sets pm_type SPECTATOR and
 * PMF_FOLLOW), clientNum and commandTime included.  The slot is the
 * entity's number; a command's time is its serverTime as the game clamped
 * it (pers.cmd.serverTime), its msec the step from the last counted one --
 * the game's own msec for a player, but for after a spawn (ClientSpawn sets
 * ps.commandTime 100 ms back), and the only true one for a follower.  A
 * command that does not advance it is not counted.  The classifier keeps
 * its own ring of ground commands at the footstep site on purpose: fed from
 * here, a client running slow in the air and fast on the ground would look
 * loud while every footstep-producing command was silent. */
void qlfix_fps_tick(int *client, const int *ent, const uint8_t *level)
{
	int    *ps = client;
	int     cn = ent[QA_GENT_number / 4];
	int     st = ps[QA_GCLIENT_pers_cmd_serverTime / 4];
	slot_t *s;
	int     d, v;
	long    win;

	if (cn < 0 || cn >= MAX_SLOTS) return;
	s = &fs_slots[cn];
	fs_slot_session(cn, s, ps);        /* first: another player in the slot clears lastCmd */
	d = st - s->kick.lastCmd;
	if (!s->kick.lastCmd) { s->kick.lastCmd = st; return; }
	/* not past the last one: not counted */
	if (d < 1) return;
	s->kick.lastCmd = st;
	v  = d > MSEC_MAX ? MSEC_MAX : d;

	/* in play: on a team (or free in FFA) until the intermission, from the
	 * client's own fields -- alive, dead, frozen (Freeze Tag, Attack &
	 * Defend's rounds), dead and following a teammate (Clan Arena), in
	 * warmup and in a timeout.  Out of play nothing is measured: some
	 * players unlock their frame rate there.  A spectator's commands only
	 * advance lastCmd (so the first one back has its own msec); the open
	 * window waits and carries on when they are back in play, since its
	 * length is time in play: going spectator just before it closes only
	 * delays the judging.  The intermission judges the open window at once
	 * instead: the next map clears every slot, and a map's last seconds
	 * would go free.  A new session of the same player (ClientBegin: a team
	 * change, a reconnect into the slot) leaves the window as it is
	 * (fs_slot_begin) */
	if (level && *(const int *)(level + QA_LEVEL_intermissiontime)) {
		if (s->kick.start && fs_kick_limit() > 0) window_close(cn, s);
		return;
	}
	if (ps[QA_GCLIENT_sess_sessionTeam / 4] == TEAM_SPECTATOR) return;

	if (s->kick.n == WINDOW) s->kick.ms512 -= s->kick.ring[s->kick.pos];
	if (s->kick.n >= FAST_W) s->kick.short64 -= s->kick.ring[(s->kick.pos + WINDOW - FAST_W) % WINDOW] <= 2;   /* the sample written 64 commands ago leaves the 64-window */
	s->kick.ring[s->kick.pos] = (unsigned char)v;
	s->kick.ms512 += v;
	s->kick.short64 += v <= 2;
	s->kick.pos = (s->kick.pos + 1) % WINDOW;
	if (s->kick.n < WINDOW) s->kick.n++;

	if (fs_kick_limit() <= 0) {
		/* the kick is paused (qlfix_maxfps 0, live): drop an open window, or
		 * a limit set again mid-map would close it at once on a stale start
		 * and an old peak */
		if (s->kick.start) window_reset(s);
		return;
	}
	if (!s->kick.start) s->kick.start = st - v;   /* the window opens with this command */
	s->kick.ms += v; s->kick.cmds++;
	/* the shortest span of 512 commands is the window's peak fps -- tracked as
	 * the span so no division runs per command; only once all 512 lie inside
	 * this window, so a burst at the end of one window is not counted again
	 * from the ring's tail at the start of the next */
	if (s->kick.cmds >= WINDOW && (!s->kick.peakMs || s->kick.ms512 < s->kick.peakMs)) s->kick.peakMs = (int)s->kick.ms512;
	/* the burst: BURST_SHORT or more of the last 64 commands <= 2 ms.  Recorded
	 * once per worsening, its fps summed over the 64 then, not per command;
	 * only once all 64 lie inside this window, so a burst at the end of one
	 * window is not counted again from the ring's tail at the start of the
	 * next (the peak's rule, with its 512) */
	if (s->kick.cmds >= FAST_W && s->kick.short64 >= BURST_SHORT && s->kick.short64 > s->kick.burstShort) {
		long ms = 0;
		int  k;
		for (k = 1; k <= FAST_W; k++) ms += s->kick.ring[(s->kick.pos + WINDOW - k) % WINDOW];
		s->kick.burstShort = s->kick.short64;
		s->kick.burstFps   = ms > 0 ? (int)(1000L * FAST_W / ms) : 0;
	}
	if (s->kick.cmds < WINDOW) return;
	win = (kick_window ? kick_window->integer : 5) * 1000L;   /* 5: qlfix_maxfpsWindow's default */
	if (win < 1000) win = 1000;
	if (s->kick.ms >= win) window_close(cn, s);   /* qlfix_maxfpsWindow seconds of play */
}

int qlfix_fps_last(int cn)
{
	slot_t *s;
	if (cn < 0 || cn >= MAX_SLOTS || !fs_slots[cn].kick.n) return -1;
	s = &fs_slots[cn];
	return s->kick.ring[(s->kick.pos + WINDOW - 1) % WINDOW];
}

