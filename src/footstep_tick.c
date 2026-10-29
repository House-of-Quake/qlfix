/*
 * footstep_tick.c -- per-command bob cycle update with framerate policy
 * (the classifier and the shadow-clock delivery; footstep.h lists the
 * files this is split across).
 *
 * Called from the site_footstep trampoline for every command that reaches
 * PM_Footsteps' advance (player on the ground and moving).  Two regimes per
 * client slot:
 *
 *   STOCK   bobCycle = (int)((float)old + bobmove * (float)msec) & 255,
 *           the original expression in the original precision (built with
 *           -ffp-contract=off so it is bit-identical to qagame's).
 *   FORCED  bobCycle held still (LOCKED, DELIVERY below), so neither the
 *           server nor the client's prediction plays a footstep from it,
 *           and the footsteps come from the SHADOW CLOCK at FORCED_RATE,
 *           each owed as the player's temp entity that ClientEndFrame
 *           emits.
 *
 * Classification asks: is the client's cycle, where it makes a sound,
 * slower than a 250 fps client's, and is that because of commands shorter
 * than 4 ms?  Over the slot's last commands, as running sums over a window:
 *
 *   cadence    the cycle's speed on the commands that sound at a crossing
 *              (running with footsteps on, dry; any gait wading or
 *              waist-deep), against what a steady 4 ms client would cover
 *              over the same time at the same gaits, scaled to a running
 *              250 fps client's 250 units a second: 1000 * advance / (msec
 *              * the gait's 4 ms advance), in sums.  Dry running it is the
 *              stock running cadence, floor(0.4*msec) per command.
 *              Commands that make no sound (walking dry, the head under)
 *              are not measured: walking dry leaves the cadence as it was.
 *   lost       the commands shorter than 4 ms that advance the cycle less
 *              than their share of a 4 ms command at their gait: running
 *              only <= 2 ms (3 ms still gives 1), walking 3 ms too
 *              (floor(0.3*3) = 0), crouching 1 and 3 ms (2 ms gives 1, a
 *              4 ms crouch's rate).  They are what makes a client above
 *              250 fps quiet; a client without them can be quiet only by
 *              being a 250 fps client or slower, which the policy protects.
 *              Counted whether they sound or not, so a forced client
 *              walking dry for a while stays forced.
 *
 *   fast tier   64 commands: at least LOST_FAST lost and cadence <
 *               FAST_TRIP -> FORCED.  A 500+ fps client is caught within
 *               64 commands.
 *   slow tier  512 commands: at least LOST_SLOW lost and cadence <
 *               SLOW_TRIP -> FORCED.
 *   clear      512 commands: fewer than LOST_CLEAR lost, or cadence >
 *               CLEAR -> STOCK.  LOST_CLEAR sits under LOST_SLOW, as CLEAR
 *               sits over SLOW_TRIP, so a stream hovering at the gate does
 *               not flip on every crossing; it is still above every real
 *               250 fps limiter, so a slot with few lost commands can
 *               neither trip nor stay FORCED.
 *
 * A tier judges cadence only over a window whose sounding commands cover a
 * quarter of its time (an eighth crouching, which counts double).  The line
 * is drawn so that nobody produces fewer footsteps than a real 250 fps
 * client.  Running, stock cadence is not monotonic in fps: it rises up to
 * 333 (every 3 ms command advances 1) and falls beyond it as 2 ms commands
 * take over, so measured caps up to ~350 are louder than a real 250 fps
 * client (~270, FORCED_RATE) and 363 is the first common cap that is
 * quieter; walking, where 3 ms is lost too, every cap above 250 is quieter,
 * and in water that is heard.  The lost count is set from the population
 * (real 250 fps limiters lose 20-37 per 512, all of them clock nudges) and
 * the constants from real client streams.
 *
 * A state holds for a dwell after a change (512 commands after the slow
 * tier forces, 64 after any other change).  Per slot there is at most one
 * console change line per LOG_INTERVAL (further changes go to the file as
 * trace lines as they happen, and are counted into the next console line)
 * and at most one "near the line" (STOCK, within NEAR_CADENCE and NEAR_LOST
 * of the slow tier) or "held" (FORCED only by the hysteresis: LOST_CLEAR to
 * LOST_SLOW lost) trace line per NEAR_INTERVAL: the data for tuning.  A
 * session is keyed by pers.enterTime, which ClientBegin sets on reconnect,
 * slot reuse, team join, map_restart and map change alike; commandTime is
 * not a session signal (ClientSpawn sets it to level.time - 100, so every
 * respawn moves it backwards).  A slot's first command of a new session
 * ends the old one (fs_slot_session); the classifier's state stays when the
 * Steam ID is the same, until a map change (or any reload while engine
 * access is off).  A new player starts STOCK with an empty window, so a
 * legitimate client is never touched.
 *
 * DELIVERY.  Stock's footstep is the side effect of bobCycle crossing 64 or
 * 192 (`((old+64) ^ (new+64)) & 128`), added to the two-slot
 * predictable-event ring the client's prediction is matched against.  The
 * ring must hold the same entries on both sides: a server-only entry, or a
 * predicted one the server never adds, renumbers the client's in-flight
 * shot or jump, which cgame then plays again (rewriting a forced player's
 * bobCycle on the server alone does exactly that, and the doubles are
 * audible in play).  And bobCycle must only move forward: spectators
 * following the player, demo playback and cg_nopredict blend it between
 * snapshots on that assumption (a cycle swung back and forth makes their
 * view bob and gun sway jitter on every step).  So a forced slot's bobCycle
 * is LOCKED: held still mid-half (64 units from either boundary, where
 * cgame's view bob and weapon sway are at rest) while the client's last 64
 * commands moved the cycle under 64 units, so that the client, predicting
 * at most 64 commands (CMD_BACKUP) forward from it by stock arithmetic,
 * cannot reach a boundary; on a boundary, a whole 128 units short of the
 * next one, once they could (unless it averages 2 units a command or more:
 * 5 ms frames or longer running, 4 ms crouching, a stream the classifier
 * never forces; a client whose frame rate falls toward 125 fps, 3 units a
 * command running, while it is still forced can, given some 340 ms of round
 * trip, or 170 ms from mid-half before the move to the boundary).  A single
 * frame that carries stock's cycle over a boundary from the lock (from
 * mid-half, 160 ms or more running, 128 ms crouching) does so on both sides
 * and is stock's crossing.  Neither side adds a footstep, the ring is the
 * same, and a still cycle blends correctly.
 *
 * Entering, the cycle moves once, inside its half, to the lock: mid-half or
 * the boundary it last passed (the tick says why).  Leaving, the cycle is
 * released to the lock's boundary plus the shadow's phase on the command
 * after its footstep: stock's next crossing then falls where the shadow's
 * next footstep would, a period on, and a client predicting from before the
 * release is behind by one command's advance only when the lock was on the
 * boundary; from mid-half the cycle steps back up to 64 units inside its
 * half, and a client predicting from before it is ahead by as much, which
 * crosses nothing either (its last 64 commands moved the cycle under
 * REST_OFF).  The cost: a forced player's view bob and weapon sway stand
 * still, at rest as a stock 500-1000 fps player's (on a boundary, at full
 * sway, only while the commands move the cycle a unit or more each).
 *
 * The SHADOW CLOCK makes the footsteps everyone hears: a phase advancing
 * msec*rate*g/PEND_Q units per command (g a 250 fps client's units per 4 ms
 * command at this gait), started from the cycle's position, so the first
 * footstep falls where stock's next crossing would (or a period after the
 * last footstep, if that is sooner: entry_phase), and restarted on a stop
 * or a respawn; each 128 units owe what stock's crossing would add: the
 * surface's footstep, picked by the game's PM_FootstepForSurface, if
 * PM_Footsteps' own footstep flag holds (running and no Scout, r13d at the
 * site) with footsteps on and no water, a splash wading (waterlevel 1) or
 * waist-deep (2, EV_SWIM) whatever the gait, nothing with the head under.
 * The command marks the slot (owe_footstep, the event picked while
 * pml.groundTrace is this player's) and ClientEndFrame (site_endframe.c,
 * qlfix_footstep_endframe) emits it as the player's own temp entity:
 * G_TempEntity(ps->origin, event) with QA_EF_PLAYER_EVENT, s.otherEntityNum
 * and s.clientNum set to the player -- what SendPendingPredictableEvents
 * builds for a ring event that did not fit the entity, except that we do
 * NOT exclude the player (no SVF_NOTSINGLECLIENT).  Emitting at the end of
 * the frame, just before BG_PlayerStateToEntityState copies the same
 * ps->origin into the player's entity, puts the footstep exactly where that
 * entity is in that snapshot.  cgame maps a player-event entity onto the
 * player: other players, spectators following the player and the player all
 * hear it, and the entity's event slot is never used, so shots, jumps and
 * pain keep their path and their muzzle flashes.  The soft-landing footstep
 * (PM_CrashLand) does not depend on the cycle and stays stock's.  Slots
 * neither FORCED nor waiting for their release are stock bit for bit.
 */
#include <stdio.h>
#include <string.h>
#include "footstep.h"

#define PEND_Q        1000       /* the shadow clock's fixed point, 1000 per unit: a command advances msec*rate*g
                                  * of these (exact; g = a 250 fps client's units per command at this gait) */

slot_t fs_slots[MAX_SLOTS];

typedef uint8_t *(*tempentity_fn)(const float *origin, int event);
static tempentity_fn g_tempentity;      /* qagame's G_TempEntity for this load, NULL = none */
static int (*fs_surface)(void);         /* the game's PM_FootstepForSurface for this load, NULL = none */
long fs_emitted, fs_nospawn, fs_stale;  /* totals since load: emitted; G_TempEntity returned nothing; owed by a session that had ended */

void qlfix_footstep_module(uintptr_t base)
{
	fs_level_module(base);
	g_tempentity = base ? (tempentity_fn)(base + QA_G_TempEntity) : NULL;
	fs_surface   = base ? (int (*)(void))(base + QA_PM_FootstepForSurface) : NULL;
}

/* A transition of a slot's state, logged once.  Every one reaches the file:
 * as a console notice when one is due (the player's first at once, then
 * at most one per LOG_INTERVAL per slot, saying how many it stands for), or
 * as a trace line otherwise.  `body` is the line up to but not including
 * its closing parenthesis; `unit` names what is counted.  Cold: the dwell
 * allows one per 64 commands at most, and real clients change rarely, so
 * kept out of the tick's code. */
FS_COLD static void log_transition(fold_t *f, int t1, const char *body, const char *unit)
{
	f->count++;
	f->unlogged++;
	if (!(f->count == 1 || t1 - f->lastLogTime >= LOG_INTERVAL)) {
		qlfix_trace("%s)", body);
		return;
	}
	if (f->unlogged > 1) qlfix_notice("%s; %d %s since last line)", body, f->unlogged, unit);
	else                 qlfix_notice("%s)", body);
	f->lastLogTime = t1;
	f->unlogged = 0;
}

/* A cadence for a log line: "-" while its window has not filled. */
FS_COLD static const char *cad_str(int v, char out[12])
{
	if (v < 0) { out[0] = '-'; out[1] = 0; } else snprintf(out, 12, "%d", v);
	return out;
}

/* What PM_Footsteps adds at a crossing, for the footstep the shadow clock
 * owes: the surface's footstep on dry ground while its own flag says
 * footstep (dry), picked by the game's PM_FootstepForSurface (0 on
 * SURF_NOSTEPS: nothing owed), a splash wading (waterlevel 1) and EV_SWIM
 * waist-deep (2) whatever the gait, nothing with the head under.  Without
 * the module, a plain footstep.  Cold: a shadow footstep comes 2.1 times a
 * second running, 4.2 crouching, and the call stays off the per-command
 * path. */
FS_COLD static void owe_footstep(slot_t *s, int dry, int waterlevel, int t1)
{
	int ev = waterlevel == 0 ? (dry ? (fs_surface ? fs_surface() : QA_EV_FOOTSTEP) : 0)
	       : waterlevel == 1 ? QA_EV_FOOTSPLASH
	       : waterlevel == 2 ? QA_EV_SWIM : 0;
	if (ev) { s->pend.owed = 1; s->pend.owedEvent = ev; s->pend.owedTime = t1; }
}


/* The slot's command ring: this command's msec, whether it sounds and its
 * advance at its own gait go in, the running sums over the fast and slow
 * windows follow.  No division: the sums are compared by
 * cross-multiplication where they are used. */
FS_HOT void ring_push(slot_t *s, int msec, int stockAdv, float bobmove, int sound)
{
	int v = msec < 1 ? 1 : msec > MSEC_MAX ? MSEC_MAX : msec;
	int c = bobmove * 4.0f >= 2.0f;              /* crouching: a 4 ms command advances 2, the rest 1 */
	int a = sound ? (stockAdv > 0x7f ? 0x7f : stockAdv) : 0;
	int d = sound ? v << c : 0;                  /* a steady 4 ms player's advance over v ms, in quarter units */
	int b = (sound != 0) | (v < 4 && 4 * stockAdv < v << c) << 1;   /* sounds; lost: under 4 ms and short of its share of a 4 ms command's advance */
	if (s->cls.n >= FAST_W) {                        /* leaves the fast window */
		int k = (s->cls.pos + WINDOW - FAST_W) % WINDOW, sk = s->ring.snd[k];
		s->cls.ms64 -= s->ring.msec[k]; s->cls.ln64 -= sk >> 1;
		s->cls.sa64 -= s->ring.act[k] & 0x7f; s->cls.sd64 -= (sk & 1) ? s->ring.msec[k] << (s->ring.act[k] >> 7) : 0;
	}
	if (s->cls.n == WINDOW) {                        /* leaves the slow window (overwritten) */
		int k = s->cls.pos, sk = s->ring.snd[k];
		s->cls.ms512 -= s->ring.msec[k]; s->cls.ln512 -= sk >> 1;
		s->cls.sa512 -= s->ring.act[k] & 0x7f; s->cls.sd512 -= (sk & 1) ? s->ring.msec[k] << (s->ring.act[k] >> 7) : 0;
	}
	s->ring.msec[s->cls.pos] = (unsigned char)v;
	s->ring.act[s->cls.pos]  = (unsigned char)(a | c << 7);
	s->ring.snd[s->cls.pos]  = (unsigned char)b;
	s->cls.ms64 += v; s->cls.ln64 += b >> 1; s->cls.sa64 += a; s->cls.sd64 += d;
	s->cls.ms512 += v; s->cls.ln512 += b >> 1; s->cls.sa512 += a; s->cls.sd512 += d;
	s->cls.pos = (s->cls.pos + 1) % WINDOW;
	if (s->cls.n < WINDOW) s->cls.n++;
}

/* The two-tier classifier over the ring, then the dwell and the change or
 * near-the-line line.  Runs once the fast window has filled. */
FS_HOT void classify(slot_t *s, int cn, int t1)
{
	/* cadence tests as cross-multiplications: 1000*sa/sd < X  <=>  1000*sa < X*sd */
	int  have512 = s->cls.n >= WINDOW && 4 * s->cls.sd512 >= s->cls.ms512;
	int want = s->cls.state, dwell = s->cls.dwell ? s->cls.dwell : FAST_W;
	if (s->cls.state == ST_STOCK) {
		if (s->cls.ln64 >= LOST_FAST && 4 * s->cls.sd64 >= s->cls.ms64 && 1000L * s->cls.sa64 < (long)FAST_TRIP * s->cls.sd64) { want = ST_FORCED; dwell = FAST_W; }
		else if (have512 && s->cls.ln512 >= LOST_SLOW && 1000L * s->cls.sa512 < (long)SLOW_TRIP * s->cls.sd512) { want = ST_FORCED; dwell = WINDOW; }
	} else if (s->cls.n >= WINDOW && (s->cls.ln512 < LOST_CLEAR || (have512 && 1000L * s->cls.sa512 > (long)CLEAR * s->cls.sd512))) {
		/* loud again, or too few lost commands to be quiet but by a
		 * near-steady 4 ms stream: a 250 fps client, which the policy
		 * protects */
		want = ST_STOCK; dwell = FAST_W;
	}
	/* dwell: the current state must have held long enough since the last change */
	if (want != s->cls.state && s->cls.sinceChange >= (s->cls.dwell ? s->cls.dwell : FAST_W)) {
		s->cls.state = want;
		s->cls.dwell = dwell;
		s->cls.sinceChange = 0;
		{
			char name[QA_NETNAME_LEN + 1], c64[12], c512[12], body[256];
			cad_str(cad64(s), c64); cad_str(cad512(s), c512);
			snprintf(body, sizeof body, "slot %d %s now %s (cadence %s/s over 64 cmds, %s/s over 512; %d of 64 lost, %d of 512; ~%d fps",
			         cn, fs_client_name(cn, name), fs_state_name(want), c64, c512, s->cls.ln64, s->cls.ln512, fps64(s));
			log_transition(&s->cls.fold, t1, body, "changes");
		}
	} else if (s->cls.state == ST_STOCK && s->cls.n == WINDOW &&
	           have512 && 1000L * s->cls.sa512 < (long)(SLOW_TRIP + NEAR_CADENCE) * s->cls.sd512 && s->cls.ln512 >= LOST_SLOW - NEAR_LOST &&
	           (s->cls.lastNearTime == 0 || t1 - s->cls.lastNearTime >= NEAR_INTERVAL)) {
		/* not forced, but a small shift of either constant would: the
		 * data for tuning, to the file only, at most one line per
		 * NEAR_INTERVAL per slot */
		char name[QA_NETNAME_LEN + 1];
		qlfix_trace("slot %d %s near the line (512 cadence %d/s, %d of 512 lost)",
		            cn, fs_client_name(cn, name), cad512(s), s->cls.ln512);
		s->cls.lastNearTime = t1;
	} else if (s->cls.state == ST_FORCED && want == ST_FORCED && s->cls.n == WINDOW &&
	           s->cls.ln512 >= LOST_CLEAR && s->cls.ln512 < LOST_SLOW &&
	           (s->cls.lastNearTime == 0 || t1 - s->cls.lastNearTime >= NEAR_INTERVAL)) {
		/* forced only because LOST_CLEAR sits under LOST_SLOW: what the
		 * hysteresis holds, to the file only, at the same rate */
		char name[QA_NETNAME_LEN + 1];
		qlfix_trace("slot %d %s held forced (512 cadence %d/s, %d of 512 lost)",
		            cn, fs_client_name(cn, name), cad512(s), s->cls.ln512);
		s->cls.lastNearTime = t1;
	}
}

/* The shadow clock's advance for a command, in 1/PEND_Q units: msec*rate*g. */
static inline long shadow_advance(int msec, float bobmove)
{
	int g = (int)(bobmove * 4.0f);
	return (long)msec * FORCED_RATE * (g < 1 ? 1 : g);
}

/* The shadow clock's command: the phase advances, and each 128 units owe
 * the player's temp entity (DELIVERY). */
FS_HOT void shadow_step(slot_t *s, int msec, float bobmove, int dry, int waterlevel, int t1)
{
	const long period = 128L * PEND_Q;
	s->pend.pos += shadow_advance(msec, bobmove);
	s->pend.stepped = s->pend.pos >= period;
	if (s->pend.stepped) {
		s->pend.pos %= period;
		if (!s->pend.owed) owe_footstep(s, dry, waterlevel, t1);
		s->pend.footTime = t1; s->pend.footKnown = 1;
	}
}

/* Lock the cycle from `lock` (a boundary: 128 units short of the next one)
 * with the shadow's phase at `phase` units. */
static void lock_at(slot_t *s, int lock, int phase)
{
	s->pend.drv = DRV_SHADOW;
	s->pend.bob = lock;
	s->pend.pos = (long)phase * PEND_Q;
	s->pend.stepped = 0;
}

/* Where a locked cycle is written: mid-half (the boundary + 64, so 0 or
 * 128), where cgame's view bob and weapon sway (|sin| of bobCycle & 127
 * over a half turn: zero at 0 and 128) are at rest, while the client's
 * commands could not carry it to a boundary within the 64 it predicts; on
 * the boundary, 128 units from the next, once they could.  Both lie inside
 * the half, so moving between them crosses nothing.  A forced 500-1000 fps
 * client advances 0 a command: its weapon rests where stock's does at that
 * rate, and a landing's 0 is already its lock (no snap on stairs). */
static inline int locked_value(slot_t *s)
{
	if (s->pend.advSum <= REST_ON) s->pend.rest = 1;
	else if (s->pend.advSum >= REST_OFF) s->pend.rest = 0;
	return (s->pend.bob + (s->pend.rest ? 64 : 0)) & 255;
}

/* The shadow's phase entering at cycle position u: u, or further on if the
 * time since the last footstep (stock's crossing or the shadow's) says so
 * (frames shortening toward 2 ms lose commands and slow stock's cycle
 * before the classifier forces the slot; from u alone the first footstep
 * would come late), so the first footstep falls a shadow period after
 * stock's last at the latest. */
static int entry_phase(const slot_t *s, int u, int t1, float bobmove)
{
	long ph;
	if (!s->pend.footKnown) return u;
	ph = shadow_advance(t1 - s->pend.footTime, bobmove) / PEND_Q;
	return ph > 127 ? 127 : ph > u ? (int)ph : u;
}

/* The footstep site's target.  `footstep` is PM_Footsteps' own flag
 * (r13d at the site): running and no Scout, as the game decided it.  A
 * NULL pm reads as dry ground with footsteps on. */
int qlfix_footstep_tick(int *ps, float bobmove, int msec, const uint8_t *pm, int footstep)
{
	int     old = ps[QL_PS_OFF_BOBCYCLE / 4];
	int     cn  = ps[QL_PS_OFF_CLIENTNUM / 4];
	int     t1  = ps[QL_PS_OFF_COMMANDTIME / 4];   /* already advanced */
	slot_t *s   = (cn >= 0 && cn < MAX_SLOTS) ? &fs_slots[cn] : NULL;
	int     forced = 0;
	float   f = (float)old + bobmove * (float)msec;   /* the original expression */
	int     stockNew = (int)f & 255;
	int     waterlevel = pm ? *(const int *)(pm + QA_PM_waterlevel) : 0;
	int     dry = s && footstep && waterlevel == 0 && (!pm || *(const int *)(pm + QA_PM_noFootsteps) == 0);   /* PM_Footsteps' footstep gate */
	int     sound = waterlevel == 0 ? dry : waterlevel <= 2;   /* this gait and medium make a sound at a crossing (the
	                                                          * surface aside): what the classifier measures */

	if (s) {
		fs_slot_session(cn, s, ps);
		ring_push(s, msec, (stockNew - old) & 255, bobmove, sound);
		s->cls.sinceChange++;
		if (s->cls.n >= FAST_W) classify(s, cn, t1);
		forced = s->cls.state == ST_FORCED;   /* the shadow clock drives it, the cycle locked (DELIVERY) */
	}

	if (!s) { ps[QL_PS_OFF_BOBCYCLE / 4] = stockNew; return old + 64; }

	/* someone else moved a locked cycle (a respawn zeroes it, standing
	 * still and PM_CrashLand's landing reset it to 0; the lock is 0 only
	 * mid-half from 192, see below): start over from where it is.  The air
	 * pauses the shadow, and the landing's 0 restarts it 64 units on, as
	 * stock's cycle restarts from 0: the first footstep after a landing
	 * ~237 ms on, stock 250's 256.  A landing too soft to reach the reset
	 * (PM_CrashLand returns first under a delta of 1) leaves the cycle and
	 * the shadow's phase as they are, unless the lock is the mid-half 0,
	 * which after the gap reads as a landing's and restarts the shadow 64
	 * units on */
	{
		int gap = t1 - msec != s->pend.lastTime, a = (stockNew - old) & 255;
		/* the advance over the last FAST_W commands: what the client's
		 * prediction could add to a lock before it hears of it */
		s->pend.advSum += a - s->pend.adv[s->pend.advPos];
		s->pend.adv[s->pend.advPos] = (unsigned char)a;
		s->pend.advPos = (s->pend.advPos + 1) % FAST_W;
		if (gap) s->pend.footKnown = 0;
		/* a lock mid-half at 0 (192 + 64) reads the same as a stop's or a
		 * landing's 0: after a gap, a 0 is theirs */
		if (s->pend.drv == DRV_SHADOW && (old != ((s->pend.bob + (s->pend.rest ? 64 : 0)) & 255) || (gap && old == 0)))
			s->pend.drv = DRV_STOCK;
	}
	s->pend.lastTime = t1;

	if (__builtin_expect(s->pend.drv == DRV_SHADOW, 0)) {
		int p = (int)(s->pend.pos / PEND_Q);
		if (!forced && s->pend.stepped) {
			/* leaving: on the command after the shadow's footstep, release
			 * the cycle to its boundary plus the shadow's phase, a
			 * command's advance at most, so stock's next crossing comes
			 * where the shadow's next footstep would, a whole period on.
			 * From a mid-half lock that steps the cycle back up to 64 units
			 * inside its half, a one-off blend for spectators that crosses
			 * nothing; any other value would put stock's next footstep off
			 * the shadow's rhythm.  A client predicting from before the
			 * release, from the boundary, is behind by that one command's
			 * advance only: its crossing and the server's fall within a
			 * command of each other, and both more than 64 commands of a
			 * forced stream away (a release half a period on would leave
			 * such a client 64-72 units behind, the server's crossing one
			 * it never predicted) */
			ps[QL_PS_OFF_BOBCYCLE / 4] = (s->pend.bob + p) & 255;
			s->pend.drv = DRV_STOCK;
		} else if (((old + 64) ^ (stockNew + 64)) & 128) {
			/* one command carries stock's cycle over a boundary from the
			 * lock (from mid-half, a frame of 160 ms or more running,
			 * 128 ms crouching): the client's prediction does the same, so
			 * stock's crossing stands on both sides and the next command
			 * locks again from it */
			ps[QL_PS_OFF_BOBCYCLE / 4] = stockNew;
			s->pend.drv = DRV_STOCK;
		} else {
			ps[QL_PS_OFF_BOBCYCLE / 4] = locked_value(s);   /* locked: no crossing, on either side */
			shadow_step(s, msec, bobmove, dry, waterlevel, t1);
		}
	} else if (!forced) {
		ps[QL_PS_OFF_BOBCYCLE / 4] = stockNew;     /* stock, bit for bit */
		s->pend.drv = DRV_STOCK;
	} else {
		/* entering: the cycle is locked from the boundary it last passed,
		 * inside its half (one snapshot's blend for a spectator): mid-half
		 * or on it (locked_value), and from the boundary a whole 128
		 * units short of the next one: a client predicting 64 commands
		 * from it would need 2 units a command, which not even a 2 ms
		 * crouch (1) reaches.  The shadow's phase starts where the cycle
		 * was, so its first footstep falls where stock's next would.  A
		 * boundary is 64 or 192, never the 0 a stop, a respawn or a
		 * landing writes, so those show as someone else's value (or, when
		 * the lock is mid-half from 192, by the gap they come with) and
		 * restart the shadow as stock's cycle restarts.  A client
		 * predicting from a snapshot before the step-back, whose next
		 * commands carry the cycle over the boundary before it has the
		 * new one (3 ms frames and some lag past the middle of the half),
		 * plays a footstep the server does not: at most one doubled event
		 * per entry, accepted for one rule instead of a wait for stock's
		 * own crossing, which near 2 ms frames would be a second or more
		 * of silence */
		int u = (old + 64) & 127;
		int lock = (old - u) & 255;
		lock_at(s, lock, entry_phase(s, u, t1, bobmove));
		ps[QL_PS_OFF_BOBCYCLE / 4] = locked_value(s);
	}
	/* stock's crossing: its footstep, on both sides */
	if (((old + 64) ^ (ps[QL_PS_OFF_BOBCYCLE / 4] + 64)) & 128) { s->pend.footTime = t1; s->pend.footKnown = 1; }
	/* the site's crossing test: stock's rule on what was written */
	return old + 64;
}

/* site_endframe.c: ClientEndFrame, rbx = the gentity, just before its
 * BG_PlayerStateToEntityState.  Emits the footstep the slot owes as the
 * player's temp entity at the origin that call is about to copy.  Without
 * the module only the bookkeeping runs. */
void qlfix_footstep_endframe(uint8_t *ent)
{
	const uint8_t *client = *(const uint8_t *const *)(ent + QA_GENT_client);
	slot_t *s;
	int cn, ev;
	if (!client) return;
	cn = *(const int *)(client + QL_PS_OFF_CLIENTNUM);
	if (cn < 0 || cn >= MAX_SLOTS) return;
	s = &fs_slots[cn];
	if (!s->pend.owed) return;
	ev = s->pend.owedEvent;
	s->pend.owed = 0;
	/* owed by a session that has ended: the player went spectator in the
	 * frame of their last ground command (SpectatorClientEndFrame skipped
	 * this site), and ClientBegin has since given them a new
	 * pers.enterTime; the next tick would start a new session, which drops
	 * the owed footstep, but this end frame comes first and would put the
	 * old footstep at their spawn point */
	if (*(const int *)(client + QA_GCLIENT_pers_enterTime) != s->enterTime) { fs_stale++; return; }
	s->pend.lastEmitEvent = ev; s->pend.lastEmitTime = s->pend.owedTime;
	s->pend.emitted++; fs_emitted++;
	if (g_tempentity) {
		uint8_t *t = g_tempentity((const float *)(client + QL_PS_OFF_ORIGIN), ev);
		if (!t) { fs_nospawn++; return; }
		*(int *)(t + QA_ES_eFlags) |= QA_EF_PLAYER_EVENT;
		*(int *)(t + QA_ES_otherEntityNum) = cn;
		*(int *)(t + QA_ES_clientNum) = cn;
	}
}

int qlfix_footstep_owed(int cn)
{
	if (cn < 0 || cn >= MAX_SLOTS || !fs_slots[cn].seen) return -1;
	return fs_slots[cn].pend.owed;
}

long qlfix_footstep_emitted(int cn)
{
	if (cn < 0 || cn >= MAX_SLOTS || !fs_slots[cn].seen) return -1;
	return fs_slots[cn].pend.emitted;
}

int qlfix_footstep_last_emit(int cn, int *commandTime)
{
	if (cn < 0 || cn >= MAX_SLOTS || !fs_slots[cn].seen) return 0;
	if (commandTime) *commandTime = fs_slots[cn].pend.lastEmitTime;
	return fs_slots[cn].pend.lastEmitEvent;
}

int qlfix_footstep_driver(int cn)
{
	slot_t *s;
	if (cn < 0 || cn >= MAX_SLOTS || !fs_slots[cn].seen) return -1;
	s = &fs_slots[cn];
	return s->pend.drv == DRV_SHADOW ? 1 : 0;
}

int qlfix_footstep_state(int cn)
{
	if (cn < 0 || cn >= MAX_SLOTS || !fs_slots[cn].seen) return -1;
	return fs_slots[cn].cls.state;
}
