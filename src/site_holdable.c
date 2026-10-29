/*
 * site_holdable.c -- no holdable item used by a dead player
 * (qlfix_deadHoldable), one site in qagame.
 *
 * ClientEvents (inlined in ClientThink_real, 0x59df0) walks the player's
 * new predictable events in order.  A fatal fall and a holdable pressed in
 * the same command come out of one Pmove as EV_FALL_* then EV_USE_ITEM*, so
 * the fall's G_Damage runs player_die first (obituary, PM_DEAD, the die
 * handler swapped to body_die, the respawn timer set) and the use-item case
 * runs on the corpse after it:
 *
 *   - medkit: ent->health = max + 25.  ClientEndFrame copies that into
 *     the playerstate, the next ClientThink sees health > 0 and lets the
 *     corpse walk: non-solid (corpse contents), still scoring, and its next
 *     lethal hit goes to body_die, so no obituary, no frag for the killer,
 *     and the long-expired respawn timer respawns it at once.  Seen live:
 *     2 hp, a medium fall and the medkit on the landing command, then 13 s
 *     of play as a corpse.
 *   - teleporter: TeleportPlayer moves the corpse and its G_KillBox
 *     telefrags whoever stands on the spawn point.
 *   - invulnerability: spawn protection on a corpse; kamikaze and portal
 *     run their handlers likewise.
 *
 * The site skips every EV_USE_ITEM* event (21..36) while ent->health <= 0,
 * the way the next command would: a dead player's Pmove never reaches the
 * holdable code.  The holdable is still gone (Pmove cleared it), as it is
 * on any death.  Nothing else in the loop changes.
 *
 *   5a2f1  mov ecx,[rax+rdx*4]           ; event = ps.events[i & 1]
 *   5a2f4  lea edx,[rcx-0x7]             ;                               <-- site
 *   5a2f7  cmp edx,0x13
 *   5a2fa  ja 5a320                      ; outside EV_FALL_MEDIUM..26 -> next event
 *   5a2fc  movsxd rdx,[r14+rdx*4] ; add rdx,r14 ; jmp rdx
 *   5a320  add ebx,1 ; cmp ebx,[rax+0x6c] ; jl 5a2e8   ; the loop's continue
 *
 * rbp = ent (health at +0x398, the medkit case's own store), rax = client,
 * ecx = event.  The cave reproduces the lea, tests the event range and the
 * health, and either jumps to 5a320 (rax still the client, as every case
 * leaves it) or reproduces the cmp/ja and jumps back to 5a2fc.  It writes
 * only edx, as the original does, and leaves the flags of `cmp edx,0x13`
 * on the way back; 5a2fc reads no flags.  No jump lands inside the eight
 * replaced bytes.
 */
#include <string.h>
#include "qlfix.h"

#define SITE      0x5a2f4
#define DISPATCH  0x5a2fc
#define NEXT      0x5a320

static const uint8_t pre[]  = { 0x8b,0x0c,0x90 };                          /* mov ecx,[rax+rdx*4] */
static const uint8_t orig[] = { 0x8d,0x51,0xf9, 0x83,0xfa,0x13, 0x77,0x24 }; /* lea edx,[rcx-7] ; cmp edx,0x13 ; ja 5a320 */
static const uint8_t post[] = { 0x49,0x63,0x14,0x96, 0x4c,0x01,0xf2 };     /* movsxd rdx,[r14+rdx*4] ; add rdx,r14 */

/* jcc rel32 (0f cc) or jmp rel32 (cc = 0) to target; returns bytes or 0 */
static size_t jrel(uint8_t *out, uintptr_t addr, uint8_t cc, uintptr_t target)
{
	int32_t rel;
	size_t  op = cc ? 2 : 1;
	if (!qlfix_rel32(addr + op - 1, target, &rel)) return 0;
	if (cc) { out[0] = 0x0f; out[1] = cc; } else out[0] = 0xe9;
	memcpy(out + op, &rel, 4);
	return op + 4;
}

/*
 *   lea  edx,[rcx-0x7]
 *   cmp  ecx,21 ; jl orig            ; below EV_USE_ITEM0
 *   cmp  ecx,36 ; jg orig            ; above EV_USE_ITEM15
 *   cmp  dword [rbp+0x398],0
 *   jle  next                        ; dead: skip the event
 * orig:
 *   cmp  edx,0x13
 *   ja   next
 *   jmp  dispatch
 */
static size_t build(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	static const uint8_t head[] = {
		0x8d,0x51,0xf9,                               /* lea edx,[rcx-0x7] */
		0x83,0xf9,0x15, 0x7c,0x12,                    /* cmp ecx,21 ; jl orig */
		0x83,0xf9,0x24, 0x7f,0x0d,                    /* cmp ecx,36 ; jg orig */
		0x83,0xbd,0x98,0x03,0x00,0x00,0x00,           /* cmp dword [rbp+0x398],0 */
	};
	size_t n = sizeof head, k;

	memcpy(out, head, n);
	if (!(k = jrel(out + n, out_addr + n, 0x8e, base + NEXT))) return 0;   /* jle next */
	n += k;
	/* orig, at +0x1a: where the jl/jg above land */
	out[n++] = 0x83; out[n++] = 0xfa; out[n++] = 0x13;                      /* cmp edx,0x13 */
	if (!(k = jrel(out + n, out_addr + n, 0x87, base + NEXT))) return 0;   /* ja next */
	n += k;
	if (!(k = jrel(out + n, out_addr + n, 0, base + DISPATCH))) return 0;  /* jmp dispatch */
	return n + k;
}

const qlfix_site_t qlfix_site_dead_holdable = {
	.name         = "dead-holdable",
	.cvar         = "qlfix_deadHoldable",
	.cvar_default = "1",
	.offset       = SITE,
	.len          = sizeof orig,
	.original     = orig,
	.pre          = pre,   .pre_len  = sizeof pre,
	.post         = post,  .post_len = sizeof post,
	.build_cave   = build,
	.cave_max     = 48,
};
