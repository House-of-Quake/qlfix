/*
 * footstep_status.c -- sessions and reporting, the third footstep file.
 *
 * A session is keyed by pers.enterTime and the player by pers.steamId (see
 * fs_slot_begin); both ticks call fs_slot_session on every command, which
 * starts a new session when enterTime changes.  qlfix_status only reports:
 * it never changes a slot.
 * The rest is what qlfix_status prints and how names and level.clients are
 * read.
 */
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "footstep.h"

static uintptr_t  level_slot;          /* address of the GOT slot holding &level, 0 = unknown */
static uintptr_t  maxclients_slot;     /* address of the GOT slot holding &sv_maxclients, 0 = unknown */

void fs_level_module(uintptr_t base)
{
	level_slot      = base ? base + QA_GOT_level : 0;
	maxclients_slot = base ? base + QA_GOT_sv_maxclients : 0;
}

static int max_clients(void)
{
	const uint8_t *cv;
	int n;
	if (!maxclients_slot) return 0;
	cv = *(const uint8_t *const *)maxclients_slot;
	if (!cv) return 0;
	n = *(const int *)(cv + QA_VMCVAR_integer);
	return n < 0 ? 0 : n > MAX_SLOTS ? MAX_SLOTS : n;
}

/* level.clients[cn] read as ints (ps first); NULL when the module, level or
 * clients pointer is unknown (before the first load). */
static const int *client_ints(int cn)
{
	const uint8_t *level, *clients;
	if (!level_slot) return NULL;
	level = *(const uint8_t *const *)level_slot;
	if (!level) return NULL;
	clients = *(const uint8_t *const *)(level + QA_LEVEL_clients);
	if (!clients) return NULL;
	return (const int *)(clients + (size_t)cn * QA_GCLIENT_SIZE);
}

/* An int field of level.clients[cn]; 0 when unknown (client_ints). */
int fs_client_field(int cn, int off, int *out)
{
	const int *c = client_ints(cn);
	if (!c) return 0;
	*out = c[off / 4];
	return 1;
}

/* pers.netname of level.clients[cn], sanitised to printable ASCII; "?" when
 * the module is unknown. */
FS_COLD const char *fs_client_name(int cn, char out[QA_NETNAME_LEN + 1])
{
	const uint8_t *level, *clients, *p;
	int i;
	out[0] = '?'; out[1] = 0;
	if (!level_slot) return out;
	level = *(const uint8_t *const *)level_slot;
	if (!level) return out;
	clients = *(const uint8_t *const *)(level + QA_LEVEL_clients);
	if (!clients) return out;
	p = clients + (size_t)cn * QA_GCLIENT_SIZE + QA_GCLIENT_pers_netname;
	for (i = 0; i < QA_NETNAME_LEN && p[i]; i++) out[i] = (p[i] < 0x20 || p[i] >= 0x7f) ? '?' : (char)p[i];
	out[i] = 0;
	if (i == 0) { out[0] = '?'; out[1] = 0; }
	return out;
}

/* Two kinds of state live in a slot.  The player's: everything the
 * auto-kick keeps (its ring, the open window, strikes, clean windows, a
 * final warning, a queued kick) and the footstep classifier with its ring
 * -- how this player's client runs, which a team change or a reconnect does
 * not change.  The session's: the cycle's lock and shadow clock, which
 * ClientSpawn's fresh playerstate starts over.  A queued kick also outlives
 * a reset by another player: the slot may change hands before the engine
 * runs the command, and qlfix_kick's guard needs the record to refuse the
 * newcomer. */
static void slot_reset(slot_t *s)
{
	int pending = s->kick.pending, pendingEt = s->kick.pendingEnterTime;
	memset(s, 0, sizeof *s);
	s->kick.pending      = pending;
	s->kick.pendingEnterTime = pendingEt;
}

/* The session's state only; the player's stays, the auto-kick's open window
 * included: across `team b` or `put` it simply runs on; across `team s` it
 * waits, and carries on once they are back in play; across a reconnect into
 * the slot the time offline arrives as one step, capped at MSEC_MAX like
 * any long frame, which can only lower the rate measured.  kicked is
 * cleared: a kicked player back in the slot is judged again (strikes at the
 * maximum, so the first window over the limit kicks). */
static void session_reset(slot_t *s)
{
	memset(&s->pend, 0, sizeof s->pend);
	s->kick.kicked = 0;
}

/* pers.enterTime changed, or the slot is new (never used, or cleared by a
 * new map).  ClientBegin sets it on a connect and a map change, and also on
 * a team change (`team s`, `team r`) and the map_restart that ends warmup.
 * The player is the Steam ID (pers.steamId, read by every ClientConnect,
 * map_restart's included): the same one keeps the player's state and starts
 * a new session; another, or none (a bot), starts clean.  The record is
 * kept per slot, never looked up across slots: two slots can hold one Steam
 * ID at once (a dropped player reconnecting while the old connection times
 * out), and the new slot then starts clean.  A new map loads the module
 * afresh and clears every slot. */
FS_COLD int fs_slot_begin(slot_t *s, const int *ps)
{
	unsigned long long id;
	memcpy(&id, ps + QA_GCLIENT_pers_steamId / 4, sizeof id);
	if (id && id == s->steamId) {
		session_reset(s);
		if (s->kick.pending) s->kick.pendingEnterTime = ps[QA_GCLIENT_pers_enterTime / 4];
		return 1;
	}
	slot_reset(s);
	s->steamId = id;
	return 0;
}

/* A new map: every player's state goes (a queued kick's record stays, as in
 * slot_reset).  The module reloads on a map_restart too, which keeps them
 * while engine access is on; without it every load counts as a new map. */
void qlfix_footstep_new_map(void)
{
	int i;
	for (i = 0; i < MAX_SLOTS; i++) slot_reset(&fs_slots[i]);
}

/* A name as a fixed-width table cell: `width` visible characters, colour
 * codes not counted (^ followed by a letter or digit; fs_client_name has
 * already reduced the name to printable ASCII), cut on a visible
 * character rather than inside a code, padded with spaces, and closed with
 * ^7 when it used colour so the rest of the row stays white.  out holds
 * QA_NETNAME_LEN + width + 3 bytes. */
#define NAME_CELL 15
static const char *name_cell(const char *name, int width, char *out)
{
	int i = 0, n = 0, vis = 0, coloured = 0;
	while (name[i] && vis < width) {
		if (name[i] == '^' && isalnum((unsigned char)name[i + 1])) {
			out[n++] = name[i++]; out[n++] = name[i++];
			coloured = 1;
			continue;
		}
		out[n++] = name[i++];
		vis++;
	}
	if (coloured) { out[n++] = '^'; out[n++] = '7'; }
	while (vis++ < width) out[n++] = ' ';
	out[n] = 0;
	return out;
}

FS_COLD const char *fs_state_name(int st) { return st == ST_FORCED ? "FORCED" : "STOCK"; }


void qlfix_footstep_status(void)
{
	int  i, n, any = 0;
	int  limit = fs_kick_limit();
	char head[128], fpshead[16] = "";

	/* rows are kept short: rcon output goes out in 1008-byte packets, one
	 * row per print */
	qlfix_row("footsteps: forced rate %d units/s running (%d ms per step)",
	          FORCED_RATE, 128000 / FORCED_RATE);
	qlfix_row("  slow tier trips below %d/s, clears above %d/s", SLOW_TRIP, CLEAR);
	qlfix_row("  %ld emitted since load (%ld not spawned, %ld stale)", fs_emitted, fs_nospawn, fs_stale);
	if (limit > 0) {
		snprintf(fpshead, sizeof fpshead, "  %4s %2s", "fps", "st");
		qlfix_row("  fps limit %d, strikes above %d", limit, fs_kick_trip(limit));
	}
	snprintf(head, sizeof head, "%2s  %-*s  %-6s  %4s %4s  %7s %4s%s",
	         "sl", NAME_CELL, "name", "state", "c512", "c64", "lost", "chg", fpshead);
	qlfix_row("%s", head);

	/* Every connected client who is playing -- on a team, or TEAM_FREE in
	 * FFA -- gets a row, including one who has sent no command this session
	 * (none on the ground and moving; with qlfix_fpsKick on, none at all):
	 * shown STOCK with no numbers.  Spectators never do (by the client's
	 * own team: a follower's copied playerstate carries the followed
	 * player's).  Without the game's client count only the fs_slots that
	 * have seen commands are walked. */
	n = level_slot ? max_clients() : 0;
	for (i = 0; i < MAX_SLOTS; i++) {
		slot_t *s = &fs_slots[i];
		int con = QA_CON_CONNECTED, et = s->enterTime, team = 0, known;
		char name[QA_NETNAME_LEN + 1], cell[QA_NETNAME_LEN + NAME_CELL + 3];

		if (i >= n && !s->seen) continue;
		known = fs_client_field(i, QA_GCLIENT_pers_connected, &con) && fs_client_field(i, QA_GCLIENT_pers_enterTime, &et)
		     && fs_client_field(i, QA_GCLIENT_sess_sessionTeam, &team);
		if (n && i >= n) con = 0;                 /* beyond the game's clients: gone */
		/* a report only: no slot is changed here.  A client who left keeps
		 * its slot's state until the slot's next command, which starts the
		 * new session (fs_slot_session), as for a slot status never looked
		 * at */
		if (con != QA_CON_CONNECTED) continue;
		/* spectators are not listed; a follower on a team (dead in Clan
		 * Arena) is: its slot is its own, measured by the fps site */
		if (known && team == TEAM_SPECTATOR) continue;
		any = 1;
		name_cell(fs_client_name(i, name), NAME_CELL, cell);
		if (!s->seen || et != s->enterTime) {
			/* nothing counted yet in this session: a slot never used, or a
			 * client who rejoined (ClientBegin gave a new enterTime) and has
			 * sent no command since.  The strikes shown are what its first
			 * command will keep (fs_slot_begin): the slot's, if the Steam
			 * ID is the same */
			char st[8] = "-";
			const int *c = client_ints(i);
			if (s->seen && c) {
				unsigned long long id;
				memcpy(&id, c + QA_GCLIENT_pers_steamId / 4, sizeof id);
				if (id && id == s->steamId) snprintf(st, sizeof st, "%d", s->kick.strikes);
			}
			qlfix_row("%2d  %s  %-6s  %4s %4s  %7s %4s%s%s", i, cell, fs_state_name(ST_STOCK),
			              "-", "-", "-", "-", limit > 0 ? "     -  " : "", limit > 0 ? st : "");
		} else {
			char c64[12] = "-", c512[12] = "-", fps[24] = "";
			if (cad64(s)  >= 0) snprintf(c64,  sizeof c64,  "%d", cad64(s));
			if (cad512(s) >= 0) snprintf(c512, sizeof c512, "%d", cad512(s));
			if (limit > 0)      snprintf(fps,  sizeof fps,  "  %4d %2d", s->kick.fps, s->kick.strikes);
			qlfix_row("%2d  %s  %-6s  %4s %4s  %3d/512 %4d%s", i, cell, fs_state_name(s->cls.state),
			              c512, c64, s->cls.ln512, s->cls.fold.count, fps);
		}
	}
	if (!any) qlfix_row("(no players)");
}
