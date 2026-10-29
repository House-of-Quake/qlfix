/*
 * Engine and game types for qzeroded.x64, build "1069 linux-x64 Jun  3 2016",
 * and its qagamex64.so.
 *
 * Derived from the binary.  What qlfix reads, and the constants beside
 * them that name the values; client_t is offsets, not a struct.
 */
#ifndef QL_TYPES_H
#define QL_TYPES_H

#include <stdint.h>
#include <stddef.h>

typedef int      qboolean;

/* ---- cvar_t ----
 * The prefix up to `integer`: SV_Frame reads com_sv_running->integer at
 * 0x4c, and SV_SetConfigstring sv_maxclients->integer at the same offset.
 */
typedef struct cvar_s {
	char     *name;
	char     *string;
	char     *resetString;
	char     *latchedString;
	char     *defaultString;
	char     *minimumString;
	char     *maximumString;
	int       flags;
	qboolean  modified;
	uint8_t   _pad0[4];
	int       modificationCount;
	float     value;
	int       integer;          /* 0x4c */
} cvar_t;

#define CVAR_ARCHIVE   0x0001
#define CVAR_LATCH     0x0020

/* ---- client_t access ----
 * The stride is the client walk in SV_SetConfigstring (`add
 * $0x26a78,%rbp`).
 */
#define QL_CLIENT_SIZE                  0x26a78
#define QL_CLIENT_OFF_STATE             0x00000
#define QL_CLIENT_OFF_NETADRTYPE        0x169f0  /* netchan.remoteAddress.type */

/* clientState_t */
#define CS_FREE       0

/* ---- playerState_t ---- QL's playerState_t carries weaponPrimary, fov,
 * location, jumpTime, crouchSlideTime, the signed movement chars,
 * freeze/thaw fields, localPersistant and round stats.  The offsets qlfix
 * relies on are below; clientNum is at 0x88.  Proven in qagamex64.so:
 * BG_PlayerStateToEntityState copies [ps+0x88] into s.number (0x54328) and
 * s.clientNum (0x5443e); PM_Footsteps keeps bobCycle at +8.
 * qlfix_footstep_tick indexes the state as an int array: field / 4. */
#define QL_PS_OFF_COMMANDTIME    0
#define QL_PS_OFF_BOBCYCLE       8
#define QL_PS_OFF_ORIGIN        20      /* vec3_t */
#define QL_PS_OFF_CLIENTNUM    136

/* team_t: the team value qlfix tests (sess.sessionTeam). */
#define TEAM_SPECTATOR 3

/* netadrtype_t */
#define NA_LOOPBACK   2   /* the host player */

#endif /* QL_TYPES_H */
