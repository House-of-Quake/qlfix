/*
 * tramp.c -- builds the register-saving trampoline a site's cave jumps
 * through into C (tramp.h has the contract).
 */
#include <string.h>
#include "qlfix.h"
#include "tramp.h"

static size_t put(uint8_t *out, size_t n, const void *b, size_t len)
{
	memcpy(out + n, b, len);
	return n + len;
}

/* movaps [rsp+disp32], xmmN  /  movaps xmmN, [rsp+disp32] */
static size_t xmm_spill(uint8_t *out, size_t n, int reg, int load)
{
	uint8_t b[9];
	size_t  k = 0;
	int32_t disp = reg * 16;
	if (reg >= 8) b[k++] = 0x44;                 /* REX.R */
	b[k++] = 0x0f;
	b[k++] = load ? 0x28 : 0x29;
	b[k++] = (uint8_t)(0x84 | ((reg & 7) << 3)); /* mod=10, rm=100 (SIB) */
	b[k++] = 0x24;                               /* SIB: base rsp */
	memcpy(b + k, &disp, 4); k += 4;
	return put(out, n, b, k);
}

/*
 * The frame qlfix_tramp_emit builds below the site's rsp (rsp_mod16 == 0):
 *
 *   site rsp
 *     -8    pad, so that the nine pushes leave rsp 16-aligned for the call
 *    -80    push rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11 (72 bytes)
 *   -336    sub rsp,256: xmm0..xmm15 at +0, +16, ... +240
 *   = rsp at the call (args() runs here)
 *
 * With rsp_mod16 == 8 the pad is omitted and the frame is 328 bytes.  The
 * site's args() runs with this frame in place, so a site that reads the
 * interrupted stack adds the frame size: site_splash.c's c_args reads
 * [rsp+0x1d0] for a trace at [rsp+0x80] (0x80 + 336).  Changing the frame
 * means changing that site.
 */
size_t qlfix_tramp_emit(uint8_t *out, uintptr_t out_addr, const qlfix_tramp_t *t)
{
	static const uint8_t pushes[] = {
		0x50, 0x51, 0x52, 0x56, 0x57,                 /* push rax,rcx,rdx,rsi,rdi */
		0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,   /* push r8,r9,r10,r11 */
	};
	static const uint8_t pops[] = {
		0x41,0x5b, 0x41,0x5a, 0x41,0x59, 0x41,0x58,   /* pop r11,r10,r9,r8 */
		0x5f, 0x5e, 0x5a, 0x59,                       /* pop rdi,rsi,rdx,rcx */
	};
	static const uint8_t sub256[] = { 0x48,0x81,0xec,0x00,0x01,0x00,0x00 };
	static const uint8_t add256[] = { 0x48,0x81,0xc4,0x00,0x01,0x00,0x00 };
	static const uint8_t sub8[]   = { 0x48,0x83,0xec,0x08 };
	static const uint8_t add8[]   = { 0x48,0x83,0xc4,0x08 };
	static const uint8_t add16[]  = { 0x48,0x83,0xc4,0x10 };
	static const uint8_t movrax[] = { 0x48,0xb8 };
	static const uint8_t callrax[] = { 0xff,0xd0 };
	size_t   n = 0, k;
	int      i, pad = (t->rsp_mod16 == 0);   /* 9 pushes = 72 -> need 8 more to realign */
	uint64_t tgt = (uint64_t)(uintptr_t)t->target;
	int32_t  rel;

	if (pad) n = put(out, n, sub8, sizeof sub8);
	n = put(out, n, pushes, sizeof pushes);
	n = put(out, n, sub256, sizeof sub256);
	for (i = 0; i < 16; i++) n = xmm_spill(out, n, i, 0);

	k = t->args(out + n, out_addr + n, t->ctx);
	if (!k) return 0;
	n += k;

	n = put(out, n, movrax, sizeof movrax);
	n = put(out, n, &tgt, 8);
	n = put(out, n, callrax, sizeof callrax);

	for (i = 0; i < 16; i++) n = xmm_spill(out, n, i, 1);
	n = put(out, n, add256, sizeof add256);
	n = put(out, n, pops, sizeof pops);
	if (t->preserve_rax) {
		out[n++] = 0x58;                                                  /* pop rax: the site's value, not the C return */
		if (pad) n = put(out, n, add8, sizeof add8);
	} else {
		n = put(out, n, pad ? add16 : add8, pad ? sizeof add16 : sizeof add8);  /* drop rax slot (+ pad) */
	}

	if (t->epilogue) {
		k = t->epilogue(out + n, out_addr + n, t->ctx);
		if (!k) return 0;
		n += k;
	}
	out[n] = 0xe9;
	if (!qlfix_rel32(out_addr + n, t->resume, &rel)) return 0;
	memcpy(out + n + 1, &rel, 4);
	return n + 5;
}
