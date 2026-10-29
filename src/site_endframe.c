/*
 * site_endframe.c -- footstep delivery, the second site of qlfix_footsteps.
 *
 * footstep_tick.c marks the slot of a FORCED client (or one waiting for its
 * release) when its shadow clock owes a footstep.  This site emits it, at
 * the end of the server frame, as the player's own temp entity
 * (G_TempEntity with EF_PLAYER_EVENT, otherEntityNum and clientNum set to
 * the player, and no SVF_NOTSINGLECLIENT, so the player receives it too).
 * cgame maps a player-event entity onto the player and plays the footstep
 * on the player, for other players, for spectators following the player and
 * for the player too, without touching the predictable-event ring the
 * player's prediction is matched against, so nothing is ever replayed; the
 * entity's own event slot is never used, so the player's shots keep their
 * path and their muzzle flashes.
 *
 * Why here and not at the bob site: BG_PlayerStateToEntityState, the very
 * next call, copies this ps->origin (truncated as G_TempEntity truncates)
 * into the player's entity for this snapshot.  A temp entity made from
 * the same origin sits exactly where that entity is, which is where a stock
 * footstep riding the entity sounds from.  Made at the command instead, it
 * would sit where the player stepped, up to a frame of movement behind.
 *
 * Site (qagamex64.so md5 bfd97681...), in ClientEndFrame (0x5b6e0; rbx =
 * ent from `mov rbx,rdi`; pushes r13,r12,rbp,rbx and sub rsp,0x68, so rsp
 * is 0 mod 16 here; it calls BG_PlayerStateToEntityState next, so it is no
 * leaf and keeps nothing in the red zone below rsp):
 *
 *   5b91b  mov  rax,[rbx+0x240]           ; client
 *   5b922  mov  dword [rax+0x1c4],0       ; ps.loopSound = 0       <-- pre
 *   5b92c  mov  rdi,[rbx+0x240]           ; client                 <-- site (15 bytes, no
 *   5b933  mov  edx,1                     ;                            branches, no rip-
 *   5b938  mov  rsi,rbx                   ; ent                        relative operands)
 *   5b93b  call BG_PlayerStateToEntityState(ps, &ent->s, qtrue)    <-- resume / post
 *
 * 0x5b92c is a convergence point (fallthrough, and jmps from 0x5bbb9 and
 * 0x5bbd1); nothing jumps into the middle of the site.  rax holds client
 * on entry and is dead (the call overwrites it); rdi/rsi/rdx are set by the
 * displaced instructions, which the epilogue re-executes verbatim after the
 * register restore.  The helper is void; its eax is discarded.  Spectators
 * never reach this site (SpectatorClientEndFrame returns earlier), and a
 * slot that owes nothing costs a few loads.
 */
#include <string.h>
#include "qlfix.h"
#include "tramp.h"

#define OFF_SITE    0x5b92c
#define OFF_RESUME  0x5b93b

static const uint8_t pre[]      = { 0xc7,0x80,0xc4,0x01,0x00,0x00,0x00,0x00,0x00,0x00 };   /* mov dword [rax+0x1c4],0 */
static const uint8_t original[] = {
	0x48,0x8b,0xbb,0x40,0x02,0x00,0x00,       /* mov rdi,[rbx+0x240] */
	0xba,0x01,0x00,0x00,0x00,                 /* mov edx,1 */
	0x48,0x89,0xde,                           /* mov rsi,rbx */
};
static const uint8_t post[]     = { 0xe8,0xd0,0x89,0xff,0xff };                            /* call 54310 */

/* rdi = ent (rbx) */
static size_t args(uint8_t *out, uintptr_t addr, void *ctx)
{
	static const uint8_t a[] = { 0x48,0x89,0xdf };    /* mov rdi,rbx */
	memcpy(out, a, sizeof a);
	return sizeof a;
}

/* the displaced instructions, as they were */
static size_t epilogue(uint8_t *out, uintptr_t addr, void *ctx)
{
	memcpy(out, original, sizeof original);
	return sizeof original;
}

static size_t build_cave(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	qlfix_tramp_t t = {
		.target    = (void *)qlfix_footstep_endframe,
		.resume    = base + OFF_RESUME,
		.rsp_mod16 = 0,
		.args      = args,
		.epilogue  = epilogue,
		.ctx       = NULL,
	};
	return qlfix_tramp_emit(out, out_addr, &t);
}

const qlfix_site_t qlfix_site_footstep_emit = {
	.name         = "footstep-emit",
	.cvar         = "qlfix_footsteps",      /* one fix with site_footstep.c: both patch or neither */
	.cvar_default = "1",
	.offset       = OFF_SITE,
	.len          = sizeof original,
	.original     = original,
	.pre          = pre,    .pre_len  = sizeof pre,
	.post         = post,   .post_len = sizeof post,
	.build_cave   = build_cave,
	.cave_max     = QLFIX_TRAMP_MAX,
};
