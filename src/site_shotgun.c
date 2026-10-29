/*
 * site_shotgun.c -- shotgun pattern origin off the integer grid
 * (qlfix_shotgunOrigin), one site in qagame.
 *
 * weapon_supershotgun_fire (0x99c20): G_TempEntity(muzzle, EV_SHOTGUN)
 * snaps the muzzle to integers, and both the server pattern (0x99e7b: the
 * pellet start is &tent->s.pos.trBase) and every client's rendering of the
 * event trace the pellets from that snapped origin.  An integer origin on
 * a curved surface's border plane whiffs the blast (see site_curve.c).
 * The engine site fixes the server's trace; the clients run their own
 * copy of the engine code, which nobody can patch, so the origin they are
 * sent must not sit on the plane in the first place.
 *
 *   99c3c  sub  rsp,0xa8                ; 6 pushes + 0xa8: rsp 16-aligned here
 *   99c43  call G_TempEntity (0x980a0)  ; rdi = muzzle, esi = EV_SHOTGUN   <-- site
 *   99c48  movss xmm2,[4096.0]
 *   99c50  mov  rbx,rax                 ; tent
 *   99c70  lea  r15,[rbx+0x18]          ; s.pos.trBase: the pattern's start
 *   99c74  ... s.origin2 = muzzle + forward*4096, snapped
 *
 * The cave makes the displaced call itself, then enters the trampoline with
 * rdi = rax (the tent).  weapon_supershotgun_fire calls G_TempEntity at the
 * site and traces the pellets after it, so it is no leaf and keeps nothing
 * in the red zone below rsp.  The helper adds NUDGE to s.pos.trBase and
 * returns the tent, so rax still holds it at 0x99c48 (the trampoline
 * restores every other register).  A non-integer coordinate goes over the
 * wire as a full float, so every client starts its trace from the same
 * off-grid point.  Aim is all but unchanged: the pattern direction is
 * normalize(origin2 - trBase), 0.58 u over 4096 u.
 *
 * Why (27, 59, 35)/128 and not 1/64 on every axis.  The clients build their
 * patch planes with x87 code and get distances that differ from the
 * server's in the low bits, so only a check run through the client's own
 * collision code says where a client's trace starts exactly on a plane.
 * 1/64 per axis clears the integer-coordinate planes but is blind to the
 * 45-degree planes whose normal's two components differ in sign (the
 * offset's parts cancel) and to axial planes whose distance has a
 * fractional part of 1/64: it leaves 203M on-plane origins within 64 u of a
 * curve over the 328 stock and workshop maps.  27, 59 and 35 are fractions
 * over 128 that no axial curve plane of any of those maps has on that axis,
 * no two of them cancel under the 1:1, 1:2 or 1:3 slopes and no signed sum
 * of all three vanishes, so no plane class is blind; an exact per-plane
 * check finds no on-plane origin within 64 u of any curve on any map, and
 * none within 512 u on the common duel and team maps bar four isolated
 * rounding coincidences, none of which whiffs from any aim.  Each value is
 * exact in float for |coordinate| < 131072.
 */
#include <string.h>
#include "qlfix.h"
#include "tramp.h"

#define SITE          0x99c43
#define RESUME        0x99c48
static const float NUDGE[3] = { 27.0f / 128, 59.0f / 128, 35.0f / 128 };

static const uint8_t pre[]  = { 0x48,0x81,0xec,0xa8,0x00,0x00,0x00 };        /* sub rsp,0xa8 */
static const uint8_t orig[] = { 0xe8,0x58,0xe4,0xff,0xff };                  /* call 980a0 */
static const uint8_t post[] = { 0xf3,0x0f,0x10,0x15,0xbc,0x53,0x01,0x00 };   /* movss xmm2,[rip+0x153bc] */

static long nudged;

void *qlfix_shotgun_nudge(uint8_t *tent)
{
	float *o = (float *)(tent + QA_ES_pos_trBase);
	o[0] += NUDGE[0]; o[1] += NUDGE[1]; o[2] += NUDGE[2];
	nudged++;
	return tent;
}

long qlfix_shotgun_nudged(void) { return nudged; }

void qlfix_shotgun_status(void)
{
	qlfix_row("shotgun: %ld pattern origins moved off the integer grid since load", nudged);
}

static size_t args(uint8_t *out, uintptr_t addr, void *ctx)
{
	static const uint8_t a[] = { 0x48,0x89,0xc7 };    /* mov rdi,rax */
	memcpy(out, a, sizeof a);
	return sizeof a;
}

static size_t build(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	qlfix_tramp_t t = {
		.target    = (void *)qlfix_shotgun_nudge,
		.resume    = base + RESUME,
		.rsp_mod16 = 0,
		.args      = args,
		.epilogue  = NULL,
		.ctx       = NULL,
	};
	int32_t rel;
	size_t  k;

	if (!qlfix_rel32(out_addr, base + QA_G_TempEntity, &rel)) return 0;
	out[0] = 0xe8;                                    /* the displaced call, same rsp */
	memcpy(out + 1, &rel, 4);
	k = qlfix_tramp_emit(out + 5, out_addr + 5, &t);
	return k ? 5 + k : 0;
}

const qlfix_site_t qlfix_site_shotgun_origin = {
	.name         = "shotgun-origin",
	.cvar         = "qlfix_shotgunOrigin",
	.cvar_default = "1",
	.offset       = SITE,
	.len          = sizeof orig,
	.original     = orig,
	.pre          = pre,   .pre_len  = sizeof pre,
	.post         = post,  .post_len = sizeof post,
	.build_cave   = build,
	.cave_max     = QLFIX_TRAMP_MAX,
};
