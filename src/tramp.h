/*
 * tramp.h -- a register-saving trampoline from a patched site into C.
 *
 * The cave saves every caller-saved register the interrupted code might
 * hold (rax, rcx, rdx, rsi, rdi, r8-r11, xmm0-xmm15), runs the site's
 * argument loader, calls the C target, restores everything EXCEPT rax
 * (the C return value stays in eax for the epilogue to use), runs the
 * site's epilogue, and jumps to the resume address.  A site whose resumed
 * code still reads the interrupted rax sets `preserve_rax`: the original
 * rax is then restored too and the C return value is discarded (the
 * target should return void).
 *
 * Alignment: the emitted code assumes rsp is 16-byte aligned at the site
 * (true at the footstep tail: PmoveSingle pushes five registers and
 * subtracts 0x40).  Sites with a different alignment must say so via
 * `rsp_mod16`.  The interrupted function must not be a leaf relying on
 * the red zone below rsp: none of the five functions patched through
 * this trampoline (PmoveSingle, G_RunMissile, weapon_supershotgun_fire,
 * ClientThink_real, ClientEndFrame) is such a leaf, as each site header
 * says.
 */
#ifndef QLFIX_TRAMP_H
#define QLFIX_TRAMP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	void     *target;                 /* C function to call */
	uintptr_t resume;                 /* absolute address to jump back to */
	int       rsp_mod16;              /* rsp alignment at the site (0 or 8) */
	int       preserve_rax;           /* restore rax as well (rax is live after the site) */
	/* Emit the argument-loading instructions at out (living at addr).
	 * Returns bytes written, 0 on failure.  Runs after the register save,
	 * so the original registers are still intact. */
	size_t  (*args)(uint8_t *out, uintptr_t addr, void *ctx);
	/* Emit the epilogue at out; runs after the restore, eax = return. */
	size_t  (*epilogue)(uint8_t *out, uintptr_t addr, void *ctx);
	void     *ctx;
} qlfix_tramp_t;

/* Returns bytes written into out, 0 if a rel32 would not reach. */
size_t qlfix_tramp_emit(uint8_t *out, uintptr_t out_addr, const qlfix_tramp_t *t);

#define QLFIX_TRAMP_MAX 512

#endif
