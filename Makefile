# qlfix -- LD_PRELOAD byte patches for Quake Live's qagamex64.so and qzeroded.x64
#
# No dependency beyond libc/libdl: it is preloaded into a 2016 binary that
# ships its own libstdc++, and dragging in anything else risks symbol clashes.
#
# -MMD -MP: without header dependencies, editing ql_types.h / ql_syms.h leaves
# stale objects linked in, and the symptom is a live test that disagrees with
# the source in front of you.
#
# -ffile-prefix-map: the debug info and any __FILE__ name paths relative to
# this directory, so the build directory (a home path) never ends up inside
# qlfix.so and the same sources build the same bytes wherever they sit.
#
# -ffp-contract=off: footstep_tick.c reproduces qagame's float expression
# (mulss/addss/cvttss2si); a fused multiply-add would round differently,
# and a STOCK player's bobCycle would no longer be bit-identical to
# qagame's.
#
# ALIGN: every function and loop starts on a 64-byte line, so an edit
# elsewhere cannot shift qlfix_footstep_tick or qlfix_fps_tick across a
# fetch boundary and change its cost.  Function padding is never executed;
# loop padding sometimes runs as a few nops.

ALIGN   := -falign-functions=64 -falign-loops=64
CFLAGS  := -O2 -g -fPIC -Wall -Wextra -Wno-unused-parameter \
           -fvisibility=hidden -std=gnu11 -MMD -MP -ffp-contract=off -mfpmath=sse \
           $(ALIGN) -ffile-prefix-map=$(CURDIR)=.
# --no-undefined: a shared object links happily with dangling symbols and only
# fails at dlopen/preload time, which shows up as the server dying on startup
# with "symbol lookup error".  Make it a link error instead.
LDFLAGS := -shared -Wl,--no-undefined -Wl,-z,noexecstack,-z,relro,-z,now
LIBS    := -ldl

FOOTSTEP_SRC := src/footstep_tick.c src/fps_kick.c src/footstep_status.c
SRC := src/qlfix.c src/ql_arm.c src/ql_log.c src/tramp.c src/site_footstep.c $(FOOTSTEP_SRC) \
       src/site_splash.c src/splash_attribute.c src/site_curve.c src/site_shotgun.c src/site_fps.c src/site_endframe.c src/site_holdable.c
OBJ := $(SRC:.c=.o)

# The server binary, only needed to regenerate the prologue table:
#   make QLDS=/path/to/qlds prologues   (or QZERODED=<qzeroded.x64>)
QZERODED ?= $(if $(QLDS),$(QLDS)/qzeroded.x64)

all: qlfix.so

# Link to a temporary name and rename: the server loads qlfix.so by absolute
# path from this directory, and a rename never touches the inode a running
# server has mapped, whatever the linker does with an existing output file.
qlfix.so: $(OBJ)
	$(CC) $(LDFLAGS) -o $@.tmp $(OBJ) $(LIBS)
	mv -f $@.tmp $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/ql_arm.o: src/ql_prologues.inc

# Regenerate the prologue fingerprints from the target binary, only when the
# address table in ql_syms.h changes.  An explicit target, not a prerequisite
# of the committed file: a fresh clone gives the sources arbitrary mtimes, and
# must build without the binary.
prologues:
	@test -n "$(QZERODED)" || { echo "set QLDS=<server install dir> or QZERODED=<qzeroded.x64>"; exit 1; }
	python3 tools/genprologues.py $(QZERODED) > src/ql_prologues.inc

# The only exported symbol must be dlopen; undefined ones only from glibc
# (libc/libdl, versioned @GLIBC_*) or the toolchain's weak hooks, and none
# newer than GLIBC_MAX, the oldest glibc it must load on: a newer build
# host's headers can raise the requirement unseen.
GLIBC_MAX ?= 2.36
check: qlfix.so
	@nm -D --defined-only qlfix.so | awk '$$2 ~ /[TW]/ {print $$3}' | sort > .exports
	@if [ "$$(cat .exports)" != "dlopen" ]; then echo "unexpected exports:"; cat .exports; rm -f .exports; exit 1; fi
	@nm -D --undefined-only qlfix.so | awk '{print $$NF}' | sort > .undef
	@grep -Ev '@GLIBC_[0-9.]+$$|^(__gmon_start__|_ITM_deregisterTMCloneTable|_ITM_registerTMCloneTable|__cxa_finalize(@.*)?)$$' .undef > .bad || true
	@if [ -s .bad ]; then echo "undefined symbols not from glibc:"; cat .bad; rm -f .exports .undef .bad; exit 1; fi
	@v=$$(sed -n 's/.*@GLIBC_//p' .undef | sort -uV | tail -1); \
	 if [ "$$(printf '%s\n%s\n' "$$v" "$(GLIBC_MAX)" | sort -V | tail -1)" != "$(GLIBC_MAX)" ]; then \
		echo "needs glibc $$v, above GLIBC_MAX $(GLIBC_MAX):"; grep "@GLIBC_$$v$$" .undef; rm -f .exports .undef .bad; exit 1; fi; \
	 echo "glibc: needs $$v (max $(GLIBC_MAX))"
	@echo "exports: dlopen; undefined: $$(sed 's/@.*//' .undef | tr '\n' ' ')"
	@rm -f .exports .undef .bad

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) qlfix.so qlfix.so.tmp

-include $(OBJ:.o=.d)
.PHONY: all check clean prologues
