/*
 * site_footstep.c -- adaptive footstep cadence (qlfix_footsteps).
 *
 * PM_Footsteps advances the bob cycle with
 *
 *     bobCycle = (int)( old + bobmove * pml.msec ) & 255;
 *
 * and plays a footstep each time it crosses 64 or 192.  The float sum is
 * truncated every command, so the advance per command is floor(bobmove *
 * msec): running (0.4) gives 3 per 8 ms at 125 fps, 1 per 4 ms at 250 fps
 * -- and 0 at frame times of 1 or 2 ms.  A client at 500 fps and above (the
 * "250x" com_maxfps exploit plus an external limiter) makes no footsteps at
 * all, and one between 333 and 500 fewer the more 2 ms frames it mixes in.
 *
 * The policy and the classifier live in footstep_tick.c.
 *
 * Site (qagamex64.so build md5 bfd976810770188800d40519392ced49), inside
 * the inlined PM_Footsteps tail of PmoveSingle.  Entered at 0x58560 from
 * the four bobmove loads with the constant in xmm2; nothing jumps into the
 * middle of it:
 *
 *   58560  mov  rax,[r12]                 ; pm->ps            (r12 = pm)
 *   58564  cvtsi2ss xmm1,[rip+pml.msec]   ; pml.msec at 0x2d49a8   <-- site
 *   5856c  mulss xmm1,xmm2
 *   58570  mov  ecx,[rax+8]               ; old = ps->bobCycle
 *   58573  cvtsi2ss xmm0,ecx
 *   58577  add  ecx,0x40                  ; old + 64
 *   5857a  addss xmm0,xmm1
 *   5857e  cvttss2si edx,xmm0
 *   58582  and  edx,0xff
 *   58588  mov  [rax+8],edx               ; new bobCycle          <-- end
 *   5858b  mov  rdx,[r12]                 ; resume: crossing test needs ecx = old+64
 *
 * The cave is a register-saving trampoline (tramp.c) calling
 * qlfix_footstep_tick(ps, bobmove, pml.msec, pm, footstep), which writes
 * the new bobCycle (a forced slot's locked value, which crosses nothing)
 * and returns old+64; the epilogue moves that into ecx, so the crossing
 * test that follows applies stock's rule to what was written.  footstep is
 * PM_Footsteps' own flag (r13d), pm (r12) is read for pm->noFootsteps and
 * pm->waterlevel (the gate PM_Footsteps applies after the store).
 * PmoveSingle calls its PM_* helpers after the site, so it is no leaf and
 * keeps nothing in the red zone below rsp.
 *
 * Side effects of the replaced bytes that the cave does NOT reproduce: the
 * original leaves rax = ps, edx = new bobCycle, xmm0 = old+bobmove*msec
 * and xmm1 = bobmove*msec, and the flags from `and edx,0xff`; the
 * trampoline leaves rax = the helper's return value, restores the pre-site
 * rdx/xmm0/xmm1, and its `add rsp` clobbers the flags.  From 0x5858b every
 * path first reloads rdx (`mov rdx,[r12]`) and eax, and recomputes the
 * flags it branches on (`mov eax,[rdx+8]; add eax,0x40;
 * xor eax,ecx; test al,0x80`), and the only later read of xmm0/xmm1 in
 * PmoveSingle, `comiss xmm0,xmm1` at 0x588d3, sits in a block reached
 * only from 0x5852e (before the site, the not-moving path) and from
 * 0x58eee/0x58f02, never from here.
 */
#include <string.h>
#include "qlfix.h"
#include "tramp.h"

#define OFF_SITE      0x58564
#define OFF_RESUME    0x5858b

static const uint8_t pre[] = { 0x49,0x8b,0x04,0x24 };                 /* mov rax,[r12] */
static const uint8_t original[] = {
	0xf3,0x0f,0x2a,0x0d,0x3c,0xc4,0x27,0x00,   /* cvtsi2ss xmm1,[rip+0x27c43c] */
	0xf3,0x0f,0x59,0xca,                        /* mulss xmm1,xmm2 */
	0x8b,0x48,0x08,                             /* mov ecx,[rax+8] */
	0xf3,0x0f,0x2a,0xc1,                        /* cvtsi2ss xmm0,ecx */
	0x83,0xc1,0x40,                             /* add ecx,0x40 */
	0xf3,0x0f,0x58,0xc1,                        /* addss xmm0,xmm1 */
	0xf3,0x0f,0x2c,0xd0,                        /* cvttss2si edx,xmm0 */
	0x81,0xe2,0xff,0x00,0x00,0x00,              /* and edx,0xff */
	0x89,0x50,0x08,                             /* mov [rax+8],edx */
};
static const uint8_t post[] = { 0x49,0x8b,0x14,0x24 };                /* mov rdx,[r12] */

/* rdi = ps (rax), xmm0 = bobmove (xmm2), rdx = pm (r12), ecx = footstep
 * (r13d), esi = pml.msec.  pm is the THIRD integer argument of
 * qlfix_footstep_tick(ps, bobmove, msec, pm, footstep): the float takes no
 * integer register, so it travels in rdx, not rcx, and footstep in rcx.
 * r13d is PM_Footsteps' `footstep` on all four ways in: the ducked,
 * walking and slow branches zero it (0x5854a, 0x58785, 0x58ae6), the
 * running one zeroes it and sets it to stats[STAT_PERSISTANT_POWERUP] !=
 * PW_SCOUT (0x58c20, 0x58c29); the dry footstep test reads it at
 * 0x58910. */
static size_t args(uint8_t *out, uintptr_t addr, void *ctx)
{
	static const uint8_t a[] = {
		0x44,0x89,0xe9,             /* mov ecx,r13d */
		0x48,0x89,0xc7,             /* mov rdi,rax */
		0x4c,0x89,0xe2,             /* mov rdx,r12 */
		0x0f,0x28,0xc2,             /* movaps xmm0,xmm2 */
		0x8b,0x35,                  /* mov esi,[rip+rel32] */
	};
	uintptr_t base = *(uintptr_t *)ctx;
	int32_t   rel;

	memcpy(out, a, sizeof a);
	/* rel32 field sits at out + sizeof a (14); next ip = out + 18 */
	if (!qlfix_rel32(addr + sizeof a - 1, base + QA_pml_msec, &rel)) return 0;
	memcpy(out + sizeof a, &rel, 4);
	return sizeof a + 4;
}

/* ecx = old + 64 (the helper's return value) */
static size_t epilogue(uint8_t *out, uintptr_t addr, void *ctx)
{
	out[0] = 0x89; out[1] = 0xc1;   /* mov ecx,eax */
	return 2;
}

static size_t build_cave(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	qlfix_tramp_t t = {
		.target    = (void *)qlfix_footstep_tick,
		.resume    = base + OFF_RESUME,
		.rsp_mod16 = 0,
		.args      = args,
		.epilogue  = epilogue,
		.ctx       = &base,
	};
	return qlfix_tramp_emit(out, out_addr, &t);
}

const qlfix_site_t qlfix_site_footstep = {
	.name         = "footstep",
	.cvar         = "qlfix_footsteps",
	.cvar_default = "1",
	.offset       = OFF_SITE,
	.len          = sizeof original,
	.original     = original,
	.pre          = pre,    .pre_len  = sizeof pre,
	.post         = post,   .post_len = sizeof post,
	.build_cave   = build_cave,
	.cave_max     = QLFIX_TRAMP_MAX,
};
