/*
 * ql_arm -- is this the qzeroded.x64 our address table was written for?
 *
 * The table is src/ql_syms.h and the ql_prologues.inc that
 * tools/genprologues.py generates from it and the binary.
 *
 * Checks only; on a failure qlfix turns off engine access (Cvar_Get,
 * Com_Printf, Cmd_AddCommand) or, for the auto-kick's own functions, only
 * its warnings and kicks, which it then logs instead.
 */
#ifndef QL_ARM_H
#define QL_ARM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char *name;
	uintptr_t   addr;
	uint8_t     bytes[12];      /* the first 12 bytes when the table was generated */
} ql_prologue_t;

enum {
	QL_FN_CLEAN,                /* the recorded bytes */
	QL_FN_DETOURED,             /* another preload's 14-byte abs jmp (minqlx's Hook) */
	QL_FN_UNRECOGNISED,         /* neither: a different binary, or a hook we don't know */
	QL_FN_NOT_IN_TABLE          /* the address is not in ql_prologues.inc */
};

/* __progname is "qzeroded.x64".  Checked first: this .so may be preloaded into
 * any process the launch script runs, and a fixed address read in something
 * that is not qzeroded would fault. */
int ql_arm_is_engine_process(void);
/* The build string at A_build_string is QL_BUILD_STRING.  Only meaningful
 * (and only safe) after ql_arm_is_engine_process(). */
int ql_arm_build_matches(void);

/* FF 25 00 00 00 00 <8-byte target>: how minqlx's Hook() overwrites a
 * prologue.  For a function we only *call*, going through it is right: that is
 * what the engine's own callers do. */
int ql_arm_is_abs_jmp(const uint8_t *code);

size_t               ql_arm_count(void);
const ql_prologue_t *ql_arm_entry(size_t i);
int                  ql_arm_entry_state(const ql_prologue_t *p);
int                  ql_arm_fn_state(uintptr_t addr);     /* looks the address up */
const char          *ql_arm_state_name(int state);

#endif
