/*
 * site_fps.c -- frame-rate measurement for the auto-kick, at ClientThink.
 *
 * ClientThink_real (qagamex64.so 0x59df0: rbp = ent from `mov rbp,rdi` at
 * 59df9, rbx = client from `mov rbx,[rdi+0x240]` at 59e04, r12 = &level
 * from 59e30) clamps ucmd->serverTime into eax, writing it back to
 * pers.cmd.serverTime, then measures the command against ps.commandTime:
 *
 *   59e54  lea  ecx,[rdx-0x3e8]       level.time - 1000
 *   59e5a  cmp  ecx,eax
 *   59e5c  jg   59ec8                 too old: clamped up there, which jumps
 *                                       back to 59e5e
 *   59e5e  mov  r13d,eax              <- site (6 bytes, no branches, no
 *   59e61  sub  r13d,[rbx]               rip-relative operands: displaced
 *                                        into the cave as they are)
 *   59e64  test r13d,r13d             <- resume
 *   59e67  jle  59ef8                 msec <= 0: dropped, unless following
 *
 * Every command of every client passes here once, before the drop and
 * before the follow branch, whose commands the game's msec does not measure
 * (a follower's playerstate is a copy of the followed player's).  The cave
 * runs the displaced pair, then calls qlfix_fps_tick(client, ent, level);
 * the tick takes each command's time from the client's own
 * pers.cmd.serverTime and its slot from the entity, never from the
 * playerstate.  r13d is callee-saved, so the C call leaves it for the
 * resumed test.  eax is NOT dead: the pause branch (59ed8: level+0x1dd8
 * set) stores it into ps.commandTime at 59eeb instead of running the move,
 * so the trampoline restores rax (preserve_rax).  rsp is 16-byte aligned at
 * the site (entry 8, six pushes, sub 0x178).  ClientThink_real calls Pmove
 * later, so it is no leaf and keeps nothing in the red zone below rsp.  The
 * site's cvar is qlfix_fpsKick, the auto-kick's switch: the site is applied
 * only while the kick is on (if its bytes or its cave fail, the kick is on
 * but receives nothing), and like every site cvar it is latched.  The
 * limit, qlfix_maxfps, is a separate cvar read live.
 */
#include <string.h>
#include "qlfix.h"
#include "tramp.h"

#define OFF_SITE    0x59e5e
#define OFF_RESUME  0x59e64

static const uint8_t pre[]      = {
	0x8d,0x8a,0x18,0xfc,0xff,0xff,            /* lea   ecx,[rdx-0x3e8] */
	0x39,0xc1,                                /* cmp   ecx,eax */
	0x7f,0x6a,                                /* jg    59ec8 */
};
static const uint8_t original[] = {
	0x41,0x89,0xc5,                           /* mov   r13d,eax */
	0x44,0x2b,0x2b,                           /* sub   r13d,[rbx] */
};
static const uint8_t post[]     = { 0x45,0x85,0xed, 0x0f,0x8e,0x8b,0x00,0x00,0x00 };   /* test r13d,r13d ; jle 59ef8 */

/* rdi = client (rbx), rsi = ent (rbp), rdx = &level (r12) */
static size_t args(uint8_t *out, uintptr_t addr, void *ctx)
{
	static const uint8_t a[] = {
		0x48,0x89,0xdf,                           /* mov rdi,rbx */
		0x48,0x89,0xee,                           /* mov rsi,rbp */
		0x4c,0x89,0xe2,                           /* mov rdx,r12 */
	};
	memcpy(out, a, sizeof a);
	return sizeof a;
}

static size_t build_cave(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	qlfix_tramp_t t = {
		.target    = (void *)qlfix_fps_tick,
		.resume    = base + OFF_RESUME,
		.rsp_mod16 = 0,
		.preserve_rax = 1,                        /* eax = ucmd->serverTime is read by the pause branch */
		.args      = args,
		.epilogue  = NULL,
		.ctx       = NULL,
	};
	size_t n = sizeof original, k;

	memcpy(out, original, n);                     /* the displaced pair first */
	k = qlfix_tramp_emit(out + n, out_addr + n, &t);
	return k ? n + k : 0;
}

const qlfix_site_t qlfix_site_fps = {
	.name         = "fps",
	.cvar         = "qlfix_fpsKick",
	.cvar_default = "0",
	.offset       = OFF_SITE,
	.len          = sizeof original,
	.original     = original,
	.pre          = pre,    .pre_len  = sizeof pre,
	.post         = post,   .post_len = sizeof post,
	.build_cave   = build_cave,
	.cave_max     = QLFIX_TRAMP_MAX,
};
