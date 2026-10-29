/*
 * site_curve.c -- no phantom hits on curved surfaces for point traces
 * (qlfix_curveTrace), one site in the ENGINE (qzeroded.x64: non-PIE,
 * fixed addresses, base 0).
 *
 * CM_TracePointThroughPatchCollide (0x419b00) is the point-trace path for
 * curved surfaces: every hitscan pellet, the lightning beam, the rail.
 * For each plane of a patch it computes d1/d2 (start/end distance) and
 *
 *   frontFacing[i]  = d1 > 0
 *   intersection[i] = d1 == d2 ? 99999 : d1 / (d1 - d2), 99999 if <= 0
 *
 * A start EXACTLY on a plane (d1 == 0) is called back-facing and never
 * crossing, whichever way the ray goes.  For a facet's border plane that
 * means "inside this border for the whole ray" even when the ray leaves it
 * at t = 0, so the facet's infinite surface plane counts as hit far outside
 * the facet: every pellet stops in mid-air.  Shotgun origins are
 * integer-snapped (G_TempEntity; the server pattern and the client's
 * rendering both trace from that origin) and patch borders often sit on
 * integer map coordinates, so a snapped muzzle on such a plane whiffs the
 * whole blast (seen live: with the tent origin on y = 256 every ray of the
 * cone stops in open air; y = 256.001 passes).  QL's other hitscan weapons
 * start from an unsnapped muzzle, which is why the shotgun is the one that
 * shows it in play.
 *
 *   419c2c  comiss xmm0,xmm6            ; d1 ? 0 (xmm6 = 0)         <-- site
 *   419c2f  seta   cl                   ; frontFacing = d1 > 0
 *   419c32  comiss xmm0,xmm1            ; d1 ? d2
 *   419c35  mov    [rsp+rdx+0x10],ecx   ; frontFacing[i] (ecx zeroed at 419bea)
 *   419c39  jne    419b70               ; d1 != d2 -> intersection
 *
 * The cave decides a start on the plane by where the ray goes: frontFacing
 * = d1 > 0 || (d1 == 0 && d2 > 0).  With intersection[i] still 99999 that
 * reads "on the side it moves to for all t > 0", which is what the border
 * test needs; non-integer starts are unaffected.  Side effects: none --
 * cl is the site's own output, the flags are recomputed at 419c32, xmm1 =
 * d2 and xmm6 = 0 are only read.
 */
#include <string.h>
#include "qlfix.h"

#define SITE    0x419c2c
#define RESUME  0x419c32

static const uint8_t pre[]  = { 0xf3,0x0f,0x58,0xc2 };                       /* addss xmm0,xmm2 */
static const uint8_t orig[] = { 0x0f,0x2f,0xc6, 0x0f,0x97,0xc1 };            /* comiss xmm0,xmm6 ; seta cl */
static const uint8_t post[] = { 0x0f,0x2f,0xc1, 0x89,0x4c,0x14,0x10 };       /* comiss xmm0,xmm1 ; mov [rsp+rdx+0x10],ecx */

static size_t build(uint8_t *out, uintptr_t out_addr, uintptr_t base)
{
	static const uint8_t body[] = {
		0x0f,0x2f,0xc6,          /* comiss xmm0,xmm6   ; d1 ? 0 */
		0x0f,0x97,0xc1,          /* seta   cl          ; d1 > 0 */
		0x75,0x06,               /* jne    +6          ; d1 != 0: done */
		0x0f,0x2f,0xce,          /* comiss xmm1,xmm6   ; d2 ? 0 */
		0x0f,0x97,0xc1,          /* seta   cl          ; d2 > 0 */
	};
	int32_t rel;

	memcpy(out, body, sizeof body);
	if (!qlfix_rel32(out_addr + sizeof body, base + RESUME, &rel)) return 0;
	out[sizeof body] = 0xe9;
	memcpy(out + sizeof body + 1, &rel, 4);
	return sizeof body + 5;
}

const qlfix_site_t qlfix_site_curve_trace = {
	.name         = "curve-trace",
	.cvar         = "qlfix_curveTrace",
	.cvar_default = "1",
	.offset       = SITE,
	.len          = sizeof orig,
	.original     = orig,
	.pre          = pre,   .pre_len  = sizeof pre,
	.post         = post,  .post_len = sizeof post,
	.build_cave   = build,
	.cave_max     = 32,
};
