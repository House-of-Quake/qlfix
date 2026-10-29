/*
 * qagame_syms.h -- the qagamex64.so offsets qlfix reads or calls.
 *
 * Target: the Steam build, 872930 bytes, md5 bfd976810770188800d40519392ced49.
 *
 * The module is position independent, so everything here is an offset from
 * its base, which the loader finds through the exported dllEntry on every
 * load.  Functions are called by offset, GOT slots are read for the globals
 * they point at, struct offsets come from the module's own loads; where a
 * derivation is given, it is beside the define or in its group's heading.
 * The engine's addresses are in ql_syms.h.
 *
 * The patched SITES are not here: each site file keeps its own offsets and
 * the original bytes it fingerprints, as site_curve.c does for the engine.
 */
#ifndef QAGAME_SYMS_H
#define QAGAME_SYMS_H

/* ---- the module ---- */
#define QA_MODULE_NAME   "qagamex64.so"
#define QA_MODULE_SIZE   872930
#define QA_DLLENTRY      0x7f180   /* exported; the loader finds the base from it with dlsym */

/* ---- functions called ---- */
#define QA_G_TempEntity  0x980a0   /* gentity_t *(vec3_t origin, int event): eType = ET_EVENTS + event,
                                    * eventTime = level.time, freeAfterEvent, snapped origin, linked */
#define QA_PM_FootstepForSurface 0x553a0   /* int (void): the footstep event for pml.groundTrace.surfaceFlags
                                            * (0 on SURF_NOSTEPS); a leaf, no stack */
#define QA_CanDamage     0x734e0   /* qboolean (gentity_t *targ, vec3_t origin): the MASK_SOLID midpoint and
                                    * corner traces G_RadiusDamage applies */

/* ---- GOT slots: each holds the address of a game global ---- */
#define QA_GOT_level         0x2ce9d8   /* level_locals_t *level (ClientConnect 0x623af loads it from here) */
#define QA_GOT_sv_maxclients 0x2cee28   /* the game's sv_maxclients vmCvar_t (cvar table entry 0x2d1b70 -> 0x855d60);
                                         * every client loop in the game is bounded by it (0x5e0ca `cmp %ebp,0xc(%r15)`) */
#define QA_GOT_g_entities    0x2ce5d0   /* gentity_t *g_entities (the load at 0x868a3 in G_RunMissile) */

/* ---- data ---- */
#define QA_pml_msec          0x2d49a8   /* pml.msec, the int PmoveSingle clamps to [1,200] */

/* ---- level_locals_t ---- */
#define QA_LEVEL_clients     0x00       /* gclient_t *clients, stride QA_GCLIENT_SIZE
                                         * (ClientConnect 0x623af: `imul $0xbf8; add (%rax)`) */
#define QA_LEVEL_intermissiontime 0x1da0 /* ClientThink_real 0x59e7d `mov 0x1da0(%r12),%esi; test; je`: the
                                         * intermission think (buttons toggle ready) when set */

/* ---- gclient_t: ps is the first member (ClientBegin 0x642a2, rbx = client:
 *      `mov %eax,0x4(%rbx)` is ps.pm_type, `mov 0x68(%rbx)` ps.eFlags) ---- */
#define QA_GCLIENT_SIZE           0xbf8
#define QA_GCLIENT_pers_connected 0x250  /* `movl $2,0x250(%rbx)` = CON_CONNECTED */
#define QA_GCLIENT_pers_cmd_serverTime 0x254 /* ClientThink_real 0x59e37 `mov 0x254(%rbx),%eax`, clamped back at 0x59e4c */
#define QA_GCLIENT_pers_netname   0x27c  /* char[36]: ClientUserinfoChanged 0x61b7c `lea 0x27c(%rbp)` -> ClientCleanName */
#define QA_NETNAME_LEN            36
#define QA_GCLIENT_pers_steamId   0x2c0  /* u64: ClientConnect 0x62437 `mov %rax,0x2c0(%r15)` */
#define QA_GCLIENT_pers_enterTime 0x2f4  /* `mov level.time,0x2f4(%rbx)` in ClientBegin */
#define QA_GCLIENT_sess_sessionTeam 0x348 /* the client's own team (a follower's ps carries the followed one's):
                                          * `cmpl $0x3,0x348(%rbx)` at 0x5ab37 = TEAM_SPECTATOR */
#define QA_CON_CONNECTED          2

/* ---- vmCvar_t ---- */
#define QA_VMCVAR_integer    0xc

/* ---- pmove_t (pm, in r12 at the footstep site) ---- */
#define QA_PM_noFootsteps    0x2c
#define QA_PM_waterlevel     0xd4

/* ---- gentity_t, stride 0x420 (G_Find 0x979f0); offsets from G_RunMissile's
 *      own loads at 0x86518-0x868c3 ---- */
#define QA_GENT_SIZE         0x420
#define QA_GENT_r_linked     0x1d8
#define QA_GENT_r_currentOrigin 0x220
#define QA_GENT_number       0x00       /* ent->s.number: entityState_t is gentity_t's first member, number its first field */
#define QA_GENT_client       0x240
#define QA_GENT_inuse        0x248
#define QA_GENT_takedamage   0x39c

/* ---- entityState_t: SendPendingPredictableEvents 0x59ad7-0x59aff sets eFlags
 *      |= 0x10 and otherEntityNum = clientNum; BG_PlayerStateToEntityState
 *      0x5443e stores ps.clientNum at +0xb0 ---- */
#define QA_ES_eFlags         0x08
#define QA_ES_pos_trBase     0x18
#define QA_ES_otherEntityNum 0x94
#define QA_ES_clientNum      0xb0
#define QA_EF_PLAYER_EVENT   0x10

/* ---- trace_t, as ints ---- */
#define QA_TRACE_contents    (0x30 / 4)
#define QA_TRACE_entityNum   (0x34 / 4)

/* ---- game constants ---- */
#define QA_EV_FOOTSTEP       1
#define QA_EV_FOOTSPLASH     3          /* PM_Footsteps 0x59598: waterlevel 1 at a crossing, whatever the gait */
#define QA_EV_SWIM           5          /* PM_Footsteps 0x58b03: waterlevel 2; 3 (head under) plays nothing */
#define QA_CONTENTS_SOLID    1
#define QA_ENTITYNUM_WORLD   1022
#define QA_ENTITYNUM_NONE    1023

#endif
