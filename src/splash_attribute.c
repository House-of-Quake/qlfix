/*
 * splash_attribute.c -- attribute a startsolid missile trace to what the
 * missile is stuck in (sites D and C of qlfix_splash, see site_splash.c).
 *
 * G_RunMissile moves a missile with one trace; when that move starts stuck
 * in something, it re-traces from the missile's origin to itself to find
 * the entity the missile is stuck in, and forces fraction 0.  With
 * g_playerCylinders on, the move tests a player as a capsule (a cylinder of
 * radius sv_cylinderScale x 15 over the box's height and a sphere of radius
 * 10.5 on its top face, CM_TraceCapsuleThroughCapsule 0x41ef30), but the
 * re-trace tests two spheres inside the box (CM_TestCapsuleInCapsule
 * 0x41d940).  So for a missile stuck in the cylinder's waist, in its reach
 * past the side faces, or in the sphere above the head, the re-trace names
 * nobody, and the impact code takes the missile for a world hit: a grenade
 * froze where it was, a rocket burst on "nothing".  The move itself had it
 * right: its trace.entityNum names the entity it found the missile stuck
 * in, and the re-trace overwrites it.
 *
 * Site D, where a stuck move leaves for the re-trace, saves that number and
 * the missile in qlfix_splash_stuck.  Site C, right after the re-trace, calls
 * qlfix_splash_attribute with the missile and the trace_t the impact code
 * will read: when the re-trace names no entity (QA_ENTITYNUM_WORLD/NONE) and
 * the move named a linked, damageable entity, that entity becomes the
 * trace's entityNum, and G_MissileImpact treats it as a normal direct hit.
 * No geometry of our own: the answer is the engine's.  It covers bodies as
 * well as players: CopyToBodyQue (0x61350) copies the player's r.svFlags,
 * SVF_CAPSULE included, so a body is traced the same way.  It never names
 * the missile's owner, nor the owner's body: the entity clip (0x440970)
 * skips the pass entity (the owner) and every entity whose r.ownerNum is
 * the pass entity, and a body's r.ownerNum is its player's number, so in
 * stock QL a player's own missiles pass through that player's body.
 *
 * (Searching for the player by geometry instead falls short: the padded
 * abs box misses the head sphere and the cylinder's reach past the side
 * faces, about one stuck missile in eight, and both the box and the
 * capsule need a guess when two candidates hold the missile.)
 *
 * The gentity_t and trace_t offsets and the g_entities GOT slot are in
 * qagame_syms.h; the loader hands us the module base per load.
 *
 * Walls.  The direct hit G_MissileImpact deals to the attributed entity
 * runs without CanDamage (as for any direct hit), so attribution must not
 * cross geometry.  Two guards:
 *  - trace.contents: SV_Trace (0x440bc0) returns straight from the world
 *    trace when it is startsolid, and the world position test (0x41d800)
 *    stores contents = brush->contents; the entity clip (0x440970) copies
 *    a sub-trace's contents only when its fraction beats the world's, and
 *    the capsule tests never set contents at all.  So on this zero-length
 *    re-trace QA_CONTENTS_SOLID means "inside a brush" and nothing else does.
 *  - CanDamage (qagame 0x734e0, the MASK_SOLID midpoint + corner traces
 *    G_RadiusDamage applies right before site A) must pass for the chosen
 *    entity.  This also covers zero-thickness patch surfaces, which no
 *    point can be "inside".  At most one CanDamage call per startsolid
 *    re-trace.
 */
#include <stdint.h>
#include "qlfix.h"
#include "ql_types.h"


typedef int (*candamage_t)(const uint8_t *targ, const float *origin);

static uintptr_t   gentities_slot;    /* address of the GOT slot holding g_entities */
static candamage_t candamage;         /* the game's own line-of-sight test */
static long        attributions;      /* since load */
static long        calls, in_solid, no_los, on_bodies, not_damageable;

void qlfix_splash_config(uintptr_t base)
{
	gentities_slot = base ? base + QA_GOT_g_entities : 0;
	candamage      = base ? (candamage_t)(base + QA_CanDamage) : NULL;
	attributions = calls = in_solid = no_los = on_bodies = not_damageable = 0;
	qlfix_splash_stuck.missile = NULL;
}

static int I(const uint8_t *e, int off) { return *(const int *)(e + off); }

void qlfix_splash_attribute(uint8_t *missile, int32_t *tr)
{
	const uint8_t *ents, *e;
	int            num = tr[QA_TRACE_entityNum], stuck;

	calls++;
	/* the move this re-trace follows: site D ran a few instructions earlier
	 * in the same G_RunMissile call, on this missile */
	stuck = qlfix_splash_stuck.missile == missile ? qlfix_splash_stuck.entityNum : QA_ENTITYNUM_NONE;
	qlfix_splash_stuck.missile = NULL;
	if (num != QA_ENTITYNUM_WORLD && num != QA_ENTITYNUM_NONE) return;
	if (tr[QA_TRACE_contents] & QA_CONTENTS_SOLID) { in_solid++; return; }   /* inside a brush */
	if (stuck < 0 || stuck >= QA_ENTITYNUM_WORLD) return;                  /* the move named nothing */
	if (!gentities_slot || !candamage) return;
	ents = *(const uint8_t **)gentities_slot;
	if (!ents) return;

	e = ents + (size_t)stuck * QA_GENT_SIZE;
	if (e == missile || !I(e, QA_GENT_inuse) || !I(e, QA_GENT_r_linked) || !I(e, QA_GENT_takedamage)) {
		not_damageable++;
		return;
	}
	if (!candamage(e, (const float *)(missile + QA_GENT_r_currentOrigin))) { no_los++; return; }   /* something in the way */
	tr[QA_TRACE_entityNum] = stuck;
	attributions++;
	if (*(void *const *)(e + QA_GENT_client) == NULL) on_bodies++;
}

long qlfix_splash_attributions(void) { return attributions; }

void qlfix_splash_status(void)
{
	qlfix_row("splash: %ld startsolid re-traces seen, %ld attributed (%ld of them not to a player), "
	          "%ld inside a brush, %ld blocked by CanDamage, %ld stuck in something that takes no damage, since load",
	          calls, attributions, on_bodies, in_solid, no_los, not_damageable);
}
