/*
 * site_splash.c -- no direct hit for everyone in the splash radius
 * (qlfix_splash), four sites.
 *
 * G_RadiusDamage (0x739b0) and its damage-through-thin-surfaces copy
 * (0x73d80) contain QL's workaround for a missile that is startsolid in a
 * player (g_playerCylinders on, the player moved onto a resting missile
 * between traces, and the startsolid re-trace names nobody): when the
 * impact trace has fraction == 0 the loop hands out the missile's FULL
 * direct damage with the direct MOD -- to every entity in the radius but
 * the missile's owner, because it never checks which entity the missile is
 * inside: three players at box distances 0, 48 and 81 all take 100 (rocket
 * splash maximum is 84).
 *
 * The direct hit belongs to G_MissileImpact, which deals it to the trace's
 * entityNum and passes that entity to G_RadiusDamage as `ignore`; sites D
 * and C name, in a stuck missile's trace, the entity the engine's move
 * found it stuck in, when that entity can take the hit (see below).  So
 * sites A and B take the branch out: every entity in the radius gets the
 * normal falloff, fraction 0 or not.
 *
 *   73c5d  xorps xmm6,xmm6
 *   73c60  comiss xmm6,[rax+8]           ; trace->fraction
 *   73c64  je 73af8                      ; fraction == 0 -> direct hit   <-- site A
 *   73c6a  ... normal falloff
 *   73af8  cmp r13,rdx ; je 73c6a        ; ent == parent -> falloff
 *   73b01  G_Damage(ent, missile, parent, dir, origin, missile->damage, 0, missile->MOD)
 *
 *   7402a  xorps xmm3,xmm3
 *   7402d  comiss xmm3,[rax+8]
 *   74031  jne 74150                     ; fraction != 0 -> falloff      <-- site B
 *   74037  cmp r14,rdx ; je 74150 ; G_Damage(... direct ...)
 *   74150  normal falloff
 *
 * Each cave is one jmp to the falloff, whatever the box distance.  The
 * direct branch is not kept even for the entity at box distance 0
 * ([rsp+0x20] in 0x739b0, [rsp+0x28] in 0x73d80): that distance is to
 * r.absmin/absmax, the box padded by 1, so the branch would give 100 to a
 * player touching the stuck entity, or standing within 1 unit of a brush
 * the missile is stuck in, neither of them hit.  With sites D and C naming
 * what the engine's move found the missile stuck in, the branch has nobody
 * left it is right for.
 *
 * Sites D and C fix the attribution so the stuck entity gets a proper
 * direct hit (knockback, EV_MISSILE_HIT with the victim, `ignore` in the
 * radius call) instead of a world impact at the missile's position: D
 * (below) saves the entity the move found the missile stuck in, C names
 * it.  G_RunMissile (0x86350) re-traces origin-to-origin when the trace is
 * startsolid and forces fraction = 0:
 *
 *   8644d  call [r11+0x100]              ; trap_Trace, origin -> origin
 *   86454  mov dword [rsp+0x88],0        ; trace.fraction = 0            <-- site C
 *   8645f  mov rax,[rbx] ; mov rdi,r14 ; call [rax+0x138]   ; trap_LinkEntity(missile)
 *   864c7  jmp 86454                     ; the capsule re-trace (g_playerCylinders on) joins here
 *
 * The trace_t is at [rsp+0x80] (entityNum at +0x34 = [rsp+0xb4]), the
 * missile in r14, rsp 16-byte aligned (6 pushes + 0x1c8), rax dead at
 * 0x8645f.  G_RunMissile calls trap_LinkEntity next, so it is no leaf and
 * keeps nothing in the red zone below rsp.  The cave reproduces the
 * displaced store and then calls qlfix_splash_attribute(missile, trace)
 * through the trampoline; inside it rsp has moved by 8 + 72 + 256 = 336, so
 * the trace is [rsp+0x1d0].  Side effects: the displaced store is
 * reproduced; no flags or scratch registers are read at 0x8645f (rax
 * reloaded, rdi rewritten).
 *
 * The helper attributes only when the re-trace did not start inside a
 * brush (trace.contents) and the game's own CanDamage (0x734e0) passes
 * for the chosen entity, so the direct hit it produces never crosses a
 * wall or a patch surface; see splash_attribute.c.
 *
 * Without D and C, A alone would stop the spill but leave the stuck
 * entity splash only; B covers the through-surface copy.
 *
 * B is dormant under default cvars.  The copy runs only for a world impact
 * with g_forceDmgThroughSurface on (or a flagged surface) whose normal
 * passes g_dmgThroughSurfaceAngularThreshold (default 0.5: a hit on the
 * underside of a thin floor).  The startsolid re-trace has no surface and
 * a zero normal, so it fails that test and goes to the normal
 * G_RadiusDamage, site A's; a real surface hit has fraction != 0 and never
 * takes the branch B patches.  B guards the copy in case an admin lowers
 * the threshold to 0 or below with the feature on.  As far as the bytes
 * go, loading is all or nothing all the same: the four share qlfix_splash,
 * and the loader restores a cvar's whole group when one of its sites has
 * unexpected bytes (a site that fails after that check, with a full cave
 * or a failed cave build, is only reported; qlfix.c).
 */
#include <string.h>
#include "qlfix.h"
#include "tramp.h"

/* ---- site A: G_RadiusDamage ---- */
#define A_SITE     0x73c64
#define A_FALLOFF  0x73c6a

static const uint8_t a_pre[]  = { 0x0f,0x2f,0x70,0x08 };                       /* comiss xmm6,[rax+8] */
static const uint8_t a_orig[] = { 0x0f,0x84,0x8e,0xfe,0xff,0xff };             /* je 73af8 */
static const uint8_t a_post[] = { 0xf3,0x0f,0x2a,0x93,0xac,0x03,0x00,0x00 };   /* cvtsi2ss xmm2,[rbx+0x3ac] */

/* ---- site B: through-surface copy ---- */
#define B_SITE     0x74031
#define B_FALLOFF  0x74150

static const uint8_t b_pre[]  = { 0x0f,0x2f,0x58,0x08 };                       /* comiss xmm3,[rax+8] */
static const uint8_t b_orig[] = { 0x0f,0x85,0x19,0x01,0x00,0x00 };             /* jne 74150 */
static const uint8_t b_post[] = { 0x49,0x39,0xd6,0x0f,0x84,0x10,0x01,0x00,0x00 }; /* cmp r14,rdx ; je 74150 */

static size_t jmp(uint8_t *out, uintptr_t addr, uintptr_t target)
{
	int32_t rel;
	if (!qlfix_rel32(addr, target, &rel)) return 0;
	out[0] = 0xe9;
	memcpy(out + 1, &rel, 4);
	return 5;
}

/*   jmp  falloff            ; never the direct branch */
static size_t build_a(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	return jmp(out, out_addr, base + A_FALLOFF);
}

static size_t build_b(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	return jmp(out, out_addr, base + B_FALLOFF);
}

const qlfix_site_t qlfix_site_splash = {
	.name         = "splash",
	.cvar         = "qlfix_splash",
	.cvar_default = "1",
	.offset       = A_SITE,
	.len          = sizeof a_orig,
	.original     = a_orig,
	.pre          = a_pre,   .pre_len  = sizeof a_pre,
	.post         = a_post,  .post_len = sizeof a_post,
	.build_cave   = build_a,
	.cave_max     = 32,
};

const qlfix_site_t qlfix_site_splash_surface = {
	.name         = "splash-surface",
	.cvar         = "qlfix_splash",
	.cvar_default = "1",
	.offset       = B_SITE,
	.len          = sizeof b_orig,
	.original     = b_orig,
	.pre          = b_pre,   .pre_len  = sizeof b_pre,
	.post         = b_post,  .post_len = sizeof b_post,
	.build_cave   = build_b,
	.cave_max     = 32,
};

/* ---- site D: G_RunMissile, the moving trace's stuck entity ----
 *
 *   863fb  call [r11+0x108]              ; trap_TraceCapsule: the move (864b0: trap_Trace)
 *   86406  mov edi,[rsp+0x84] ; test ; jne 86420   ; trace.startsolid
 *   86411  mov esi,[rsp+0x80] ; test ; je 864d0    ; trace.allsolid
 *   86420  mov rcx,[rip+0x248799]        ; g_playerCylinders, for the re-trace  <-- site D
 *   86427  mov edx,[r14+0x290]           ; ... re-trace, then site C at 86454
 *
 * Both moves (capsule and box) join at 86406, and 86420 is where a stuck
 * one leaves for the re-trace, which overwrites the trace.  The move's
 * trace.entityNum ([rsp+0xb4]) still names what it was stuck in: the
 * entity clip (0x440970) stores the stuck entity's number and keeps the
 * first one (a stuck test has fraction 0, and only a smaller fraction
 * replaces the entry).  The cave saves it and the missile (r14) in
 * qlfix_splash_stuck, then makes the displaced load from the GOT slot
 * (0x2cebc0) by absolute address and jumps back, so rcx ends holding the
 * displaced load's value, as in the original.  edx is written at 86427
 * before any read; flags are not read before 86446 sets them again. */
#define D_SITE     0x86420
#define D_RESUME   0x86427
#define D_GOT_CYL  0x2cebc0   /* the GOT slot the displaced load reads: g_playerCylinders' vmCvar_t * */

qlfix_splash_stuck_t qlfix_splash_stuck;     /* written by the cave, read by qlfix_splash_attribute */

static const uint8_t d_pre[]  = { 0x0f,0x84,0xb0,0x00,0x00,0x00 };              /* je 864d0 */
static const uint8_t d_orig[] = { 0x48,0x8b,0x0d,0x99,0x87,0x24,0x00 };         /* mov rcx,[rip+0x248799] */
static const uint8_t d_post[] = { 0x41,0x8b,0x96,0x90,0x02,0x00,0x00 };         /* mov edx,[r14+0x290] */

static size_t build_d(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	static const uint8_t save[] = {
		0x8b,0x94,0x24,0xb4,0x00,0x00,0x00,           /* mov edx,[rsp+0xb4]: the move's entityNum */
	};
	uint64_t stuck = (uintptr_t)&qlfix_splash_stuck, slot = base + D_GOT_CYL;
	size_t n = 0, k;

	memcpy(out, save, sizeof save); n += sizeof save;
	out[n++] = 0x48; out[n++] = 0xb9; memcpy(out + n, &stuck, 8); n += 8;   /* mov rcx,&qlfix_splash_stuck */
	out[n++] = 0x89; out[n++] = 0x11;                                       /* mov [rcx],edx */
	out[n++] = 0x4c; out[n++] = 0x89; out[n++] = 0x71; out[n++] = 0x08;     /* mov [rcx+8],r14 */
	out[n++] = 0x48; out[n++] = 0xb9; memcpy(out + n, &slot, 8); n += 8;    /* mov rcx,base+0x2cebc0 */
	out[n++] = 0x48; out[n++] = 0x8b; out[n++] = 0x09;                      /* mov rcx,[rcx]: the displaced load */
	if (!(k = jmp(out + n, out_addr + n, base + D_RESUME))) return 0;
	return n + k;
}

const qlfix_site_t qlfix_site_missile_stuck = {
	.name         = "missile-stuck",
	.cvar         = "qlfix_splash",
	.cvar_default = "1",
	.offset       = D_SITE,
	.len          = sizeof d_orig,
	.original     = d_orig,
	.pre          = d_pre,   .pre_len  = sizeof d_pre,
	.post         = d_post,  .post_len = sizeof d_post,
	.build_cave   = build_d,
	.cave_max     = 48,
};

/* ---- site C: G_RunMissile attribution ---- */
#define C_SITE     0x86454
#define C_RESUME   0x8645f

static const uint8_t c_pre[]  = { 0x41,0xff,0x93,0x00,0x01,0x00,0x00 };                 /* call [r11+0x100] */
static const uint8_t c_orig[] = { 0xc7,0x84,0x24,0x88,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }; /* mov dword [rsp+0x88],0 */
static const uint8_t c_post[] = { 0x48,0x8b,0x03,0x4c,0x89,0xf7 };                      /* mov rax,[rbx] ; mov rdi,r14 */

/* rdi = missile (r14), rsi = &trace ([rsp+0x80] before the trampoline's 336 bytes) */
static size_t c_args(uint8_t *out, uintptr_t addr, void *ctx)
{
	static const uint8_t a[] = {
		0x4c,0x89,0xf7,                               /* mov rdi,r14 */
		0x48,0x8d,0xb4,0x24,0xd0,0x01,0x00,0x00,      /* lea rsi,[rsp+0x1d0] */
	};
	memcpy(out, a, sizeof a);
	return sizeof a;
}

static size_t build_c(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	qlfix_tramp_t t = {
		.target    = (void *)qlfix_splash_attribute,
		.resume    = base + C_RESUME,
		.rsp_mod16 = 0,
		.args      = c_args,
		.epilogue  = NULL,
		.ctx       = NULL,
	};
	size_t n = sizeof c_orig, k;

	memcpy(out, c_orig, n);                           /* the displaced store, rsp untouched */
	k = qlfix_tramp_emit(out + n, out_addr + n, &t);
	return k ? n + k : 0;
}

const qlfix_site_t qlfix_site_missile_attribute = {
	.name         = "missile-attribute",
	.cvar         = "qlfix_splash",
	.cvar_default = "1",
	.offset       = C_SITE,
	.len          = sizeof c_orig,
	.original     = c_orig,
	.pre          = c_pre,   .pre_len  = sizeof c_pre,
	.post         = c_post,  .post_len = sizeof c_post,
	.build_cave   = build_c,
	.cave_max     = QLFIX_TRAMP_MAX,
};
