/*
 * ql_arm -- see ql_arm.h.
 */
#include "ql_arm.h"
#include "ql_syms.h"            /* A_build_string, QL_BUILD_STRING */

#include <string.h>

#include "ql_prologues.inc"     /* GENERATED from ql_syms.h + the binary */

#define NPROLOGUES (sizeof(ql_prologues) / sizeof(ql_prologues[0]))

extern const char *__progname;

int ql_arm_is_engine_process(void)
{
	return __progname && strcmp(__progname, "qzeroded.x64") == 0;
}

int ql_arm_build_matches(void)
{
	return strncmp((const char *)(uintptr_t)A_build_string, QL_BUILD_STRING,
	               sizeof(QL_BUILD_STRING) - 1) == 0;
}

int ql_arm_is_abs_jmp(const uint8_t *b)
{
	return b[0] == 0xff && b[1] == 0x25
	    && b[2] == 0x00 && b[3] == 0x00 && b[4] == 0x00 && b[5] == 0x00;
}

size_t ql_arm_count(void)
{
	return NPROLOGUES;
}

const ql_prologue_t *ql_arm_entry(size_t i)
{
	return i < NPROLOGUES ? &ql_prologues[i] : NULL;
}

int ql_arm_entry_state(const ql_prologue_t *p)
{
	const uint8_t *cur = (const uint8_t *)p->addr;

	if (memcmp(cur, p->bytes, sizeof(p->bytes)) == 0)
		return QL_FN_CLEAN;
	if (ql_arm_is_abs_jmp(cur))
		return QL_FN_DETOURED;
	return QL_FN_UNRECOGNISED;
}

int ql_arm_fn_state(uintptr_t addr)
{
	size_t i;

	for (i = 0; i < NPROLOGUES; i++)
		if (ql_prologues[i].addr == addr)
			return ql_arm_entry_state(&ql_prologues[i]);
	return QL_FN_NOT_IN_TABLE;
}

const char *ql_arm_state_name(int state)
{
	switch (state) {
	case QL_FN_CLEAN:        return "clean";
	case QL_FN_DETOURED:     return "detoured";
	case QL_FN_UNRECOGNISED: return "unrecognised";
	default:                 return "not in table";
	}
}
