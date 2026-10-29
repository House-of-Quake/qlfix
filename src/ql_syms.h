/*
 * qlfix -- the qzeroded.x64 addresses qlfix uses.
 *
 * Target: build "1069 linux-x64 Jun  3 2016 20:53:50" (non-PIE ET_EXEC, so
 * these are the addresses the process uses).  tools/genprologues.py
 * fingerprints every function here into ql_prologues.inc and engine_arm()
 * checks them at runtime through ql_arm.c.
 *
 * The engine *site* (site_curve.c) keeps its own address and fingerprint:
 * it is patched, not called.
 */
#ifndef QL_SYMS_H
#define QL_SYMS_H

#include "ql_types.h"

/* ---- functions ---- */
#define A_Cvar_Get                      0x426bc0  /* (name, value, flags) -> cvar_t* */
#define A_Com_Printf                    0x421170  /* (fmt, ...)                      */
#define A_Cmd_AddCommand                0x4209c0  /* (name, void(*)(void))           */
/* auto-kick (qlfix_fpsKick) */
#define A_Cbuf_AddText                  0x420310  /* (text): run at the top of the next frame */
#define A_SV_SendServerCommand          0x43db90  /* (client_t* or NULL = everyone, fmt, ...) */
#define A_SV_DropClient                 0x439390  /* (client_t*, reason)             */
#define A_Cmd_Argc                      0x420660  /* () -> int                       */
#define A_Cmd_Argv                      0x420670  /* (int) -> const char* ("" if out of range) */

/* ---- data ---- */
#define A_svs_clients                   0x1472290  /* client_t*                  */
#define A_cv_com_sv_running             0x864bb0   /* cvar_t**                   */
#define A_cv_sv_maxclients              0x1576b38  /* cvar_t**                   */
/* Two words of server_t (0x15202e0; serverId at +8, sv.time at +0x56850
 * right before the sv_maxclients pointer).  sv.time is what
 * SV_SendClientSnapshot loads at 0x43f9b1 to write as the snapshot's
 * serverTime, so it is the clock every snapshot carries, and level.time
 * equals it; SV_SpawnServer clears it on a map change; a level restart
 * (map_restart, the end of warmup) lets it run on.  serverId bumps on both.
 */
#define A_sv_serverId                   0x15202e8  /* int                        */
#define A_sv_time                       0x1576b30  /* int, ms                    */

#define A_build_string                  0x50a628
#define QL_BUILD_STRING                 "1069 linux-x64 Jun  3 2016 20:53:50"

/* ---- calls ---- */
typedef cvar_t   *(*ql_Cvar_Get_t)(const char *, const char *, int);
typedef void      (*ql_Com_Printf_t)(const char *, ...);
typedef void      (*ql_Cmd_AddCommand_t)(const char *, void (*)(void));
typedef void      (*ql_Cbuf_AddText_t)(const char *);
typedef void      (*ql_SV_SendServerCommand_t)(void *, const char *, ...);
typedef void      (*ql_SV_DropClient_t)(void *, const char *);
typedef int       (*ql_Cmd_Argc_t)(void);
typedef const char *(*ql_Cmd_Argv_t)(int);

#define QL_FN(name, addr)  ((ql_##name##_t)(uintptr_t)(addr))

#define QL_Cvar_Get                 QL_FN(Cvar_Get,                 A_Cvar_Get)
#define QL_Com_Printf               QL_FN(Com_Printf,               A_Com_Printf)
#define QL_Cmd_AddCommand           QL_FN(Cmd_AddCommand,           A_Cmd_AddCommand)
#define QL_Cbuf_AddText             QL_FN(Cbuf_AddText,             A_Cbuf_AddText)
#define QL_SV_SendServerCommand     QL_FN(SV_SendServerCommand,     A_SV_SendServerCommand)
#define QL_SV_DropClient            QL_FN(SV_DropClient,            A_SV_DropClient)
#define QL_Cmd_Argc                 QL_FN(Cmd_Argc,                 A_Cmd_Argc)
#define QL_Cmd_Argv                 QL_FN(Cmd_Argv,                 A_Cmd_Argv)

/* ---- access ---- */
#define QL_RPTR(a)  (*(void *volatile *)(uintptr_t)(a))
#define QL_R32(a)   (*(volatile int *)(uintptr_t)(a))

#endif
