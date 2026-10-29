# qlfix

Server-side fixes for various Quake Live bugs, implemented as a small
library to be preloaded into the Linux dedicated server (x64 only).

Players are recommended to disable view bob and weapon sway if playing over
333 fps (cg_bob 0; cg_drawGun 2).

| Bug | What players see | Cvar | Default |
|---|---|---|---|
| [Silent footsteps](#silent-footsteps-above-250-fps) | A player above around 350 fps makes fewer or no footsteps | `qlfix_footsteps` | on |
| [Frame-rate limit evasion](#frame-rate-limit) | - | `qlfix_fpsKick` | off |
| [Everyone takes a direct hit](#everyone-takes-a-direct-hit) | One rocket does full direct-hit damage to everyone near it | `qlfix_splash` | on |
| [Shotgun blasts that hit air](#shotgun-blasts-that-hit-air) | The whole blast stops in mid-air near curved walls | `qlfix_curveTrace`, `qlfix_shotgunOrigin` | on |
| [The walking dead](#the-walking-dead) | A player killed by a fall uses the medkit and keeps playing as a corpse | `qlfix_deadHoldable` | on |

## Install

You need the Linux x64 dedicated server (`qzeroded.x64`, build 1069), `gcc`
and `make`.

```
git clone <this repository> qlfix
cd qlfix
make && make check
```

`make check` ensures the library exports nothing but `dlopen` and needs
nothing beyond glibc. Build it on the server, or on a machine whose glibc is
no newer than the server's.

Start the server with `qlfix.so` in `LD_PRELOAD`, by absolute path. If you
also run minqlx, put qlfix first:

```
LD_PRELOAD="/path/to/qlfix/qlfix.so /path/to/minqlx/minqlx.x64.so" ./qzeroded.x64 +set ...
```

The server console reports the status of each patched site at map load, for
example:

```
qlfix: v0.8 armed: build "1069 linux-x64 Jun  3 2016 20:53:50", Cvar_Get clean, Com_Printf detoured (minqlx), Cmd_AddCommand detoured (minqlx), 10 site(s), auto-kick available (qlfix_fpsKick)
qlfix:   curve-trace: patched (+0x419c2c -> cave 0x300000, 19 bytes)
qlfix: qagamex64.so base 0x7267f1c00000 cave 0x7267f1b00000, serverId 0
qlfix:   footstep: patched (+0x58564 -> cave 0x7267f1b00000, 356 bytes)
qlfix:   splash: patched (+0x73c64 -> cave 0x7267f1b00170, 5 bytes)
qlfix:   splash-surface: patched (+0x74031 -> cave 0x7267f1b00180, 5 bytes)
qlfix:   missile-stuck: patched (+0x86420 -> cave 0x7267f1b00190, 41 bytes)
qlfix:   missile-attribute: patched (+0x86454 -> cave 0x7267f1b001c0, 358 bytes)
qlfix:   shotgun-origin: patched (+0x99c43 -> cave 0x7267f1b00330, 344 bytes)
qlfix:   fps: patched (+0x59e5e -> cave 0x7267f1b00490, 352 bytes)
qlfix:   footstep-emit: patched (+0x5b92c -> cave 0x7267f1b005f0, 354 bytes)
qlfix:   dead-holdable: patched (+0x5a2f4 -> cave 0x7267f1b00760, 40 bytes)
qlfix:   auto-kick: fps limit 250, strikes above 275 (margin 10 percent auto), 5 s windows, kick on strike 3
```

Every fix except the frame-rate limit is on by default. Run `/qlfix_status`
in the server console (or through rcon) to see counters and player states.

## The fixes

### Silent footsteps above 250 fps

**The bug:** The game moves each player's footstep cycle forward by a whole
number of units per frame, rounding down, and plays a footstep every 128
units. Running at 250 fps (4 ms frames) that gives 1 unit a frame. At 500
fps or more (2 ms or 1 ms frames) it gives 0: no footsteps at all. The
client limits `com_maxfps` to 250, but a value such as `250x` turns the
limit off, and an external frame limiter at 500 or 1000 fps makes the player
silent.

This bug has a longer history than you may expect - capping at 250 makes
around 25% fewer footsteps than capping at 125. qlfix accepts this as how
the game has been played since 2013 when id increased the limit and so
leaves 250 caps untouched. By coincidence, 250 is the point when players
make the least footsteps between 125 fps all the way to around 360.

| cap | ideal cadence | measured cadence | compared with 250 fps |
|---|---|---|---|
| 125 | 375/s | 367/s | louder |
| 250 | 250/s | 270/s | — |
| 333 | 333/s | 293/s | louder |
| 363 | 274/s | 263/s | quieter |

(Cadence: footstep-cycle units per second, idealized and from real client
recordings; a footstep every 128 units.)

**The fix:** The server watches each player's recent frames. A player whose
footsteps come less often than a 250 fps player's, because of frames shorter
than 4 ms, is switched to **FORCED**: the server plays their footsteps
itself, at a steady 270 units a second (a footstep about every 474 ms while
running), which is what a real 250 fps client measures. Everyone else stays
**STOCK** and the server does not touch them. A forced player who goes back
to 250 fps returns to stock.

- Nobody makes fewer footsteps than a 250 fps player.
- Footsteps keep the floor's sound (metal, snow, wood), splashes in water,
  nothing under water, nothing walking or with the Scout on dry ground, just
  as stock.
- 250, 333 and 350 caps are never forced. 363 and above are forced after
  about 1.5 s of running; 500 fps and up within 64 frames. Caps around
  353–360 sit on the line and may switch now and then; either way they make
  about the same number of footsteps.

**Known limits:** Forced players have their bobCycle frozen by the server,
which can cause slight jitter in weapon sway and view bob. Players who find
this distracting should set `cg_bob 0` and `cg_drawGun 2`.

In rare cases, a forced player hears one of their own shots or jumps twice:
entering the forced state with 3 ms frames and high ping, or dropping
towards 125 fps while still forced with 340 ms or more of round trip time.

### Frame-rate limit

Off by default and not needed for footsteps: the footstep fix handles fast
players without removing anyone. Turn it on if your server has an fps rule:

```
set qlfix_fpsKick 1
set qlfix_maxfps 250
```

With these defaults, a player who keeps above 275 fps for a sustained
stretch (512 frames, 1–2 s) in 5 seconds of play gets a strike and a warning
to everyone:

```
alpha is at 341 fps, over this server's limit of 250 (strike 1 of 3).
```

Short bursts of high frame-rates also earn a strike.

The third strike kicks them:
`alpha was kicked: frame rate 341 fps is over this server's limit of 250`.
Three clean windows (15 seconds by default) clear the strikes, and a player
who caps after the second warning is not kicked.

Why 275: honest 250 fps clients measure 247–256 fps, 333 caps 317–331. The
default margin (`qlfix_maxfpsMargin -1`) is 10 % for limits up to 300 and
2 % above. `qlfix_maxfps 333` works (trip at 340 fps) but also kicks 350
and 363 caps; watch the strike lines and `qlfix_status` for a while before
relying on it.

### Everyone takes a direct hit

**The bug:** With `g_playerCylinders` on, a player can walk onto a missile
between traces, resulting in the missile being stuck inside them. The game
treats this as a direct hit, but never checks *who* it is inside. Every
player in the blast radius takes the full direct-hit damage and knockback,
when splash usually tops out at 84.

**The fix:** Only the player the missile is actually inside takes the direct
hit; everyone else takes normal splash damage. The stuck player gets a
proper direct hit (knockback, impact).

Other projectiles are also affected: the trick of balancing a grenade on an
AFK player's head no longer works, and a plasma bolt does 20 (a hit) instead
of 15 (a near miss). Missiles are not predicted by the client, so nothing
changes client-side.

### Shotgun blasts that hit air

**The bug:** Now and then a whole shotgun blast hits nothing: twenty sparks
hanging in the air, no damage. The shotgun's origin is rounded to whole
units, and so are the edges of curved surfaces. When the origin lands
exactly on the plane of a curve's edge, the collision code counts the ray as
inside the curve and stops it at once, in every direction of the spread.
Other hitscan weapons fire from an unrounded position, so in practice only
the shotgun shows it.

**The fix:** Two parts:

- `qlfix_curveTrace`: the server's collision code treats a ray starting
  exactly on an edge plane as being on the side it moves toward. This fixes
  every hit the server traces (shotgun, LG, rail, machine guns, gauntlet).
- `qlfix_shotgunOrigin`: clients run their own collision code, which a
  server cannot patch, to draw the sparks. With this enabled, the shotgun's
  origin is moved off whole units by a fraction, (27, 59, 35)/128. Checked
  against all stock maps and the HoQ map pools, no origin near a curve lands
  on a client's plane. The sparks move by at most 0.6 units, less than the
  rounding already scatters them.

### The walking dead

**The bug:** If a fatal fall and a holdable item happen in the same frame,
the game kills the player first and then uses the item on the corpse. With
the medkit the corpse gets max health + 25 and keeps playing: it moves,
fights, scores, gives no frag when it dies again, and respawns instantly
when dying for real.

**The fix:** A holdable is not used by a player at 0 health or below. The
item is lost, as on any death. The death, the kill feed and the scoreboard
stay as they were.

## Configuration

| Name | Default | |
|---|---|---|
| `qlfix_footsteps` | 1 | footstep fix |
| `qlfix_fpsKick` | 0 | frame-rate limit and kick |
| `qlfix_maxfps` | 250 | the limit in fps; 0 pauses the kick |
| `qlfix_maxfpsMargin` | -1 | percent above the limit before a window counts; -1 = 10 up to a limit of 300, 2 above |
| `qlfix_maxfpsWindow` | 5 | seconds of play per strike window |
| `qlfix_maxfpsStrikes` | 3 | number of strikes that results in a kick; three clean windows in a row clear them |
| `qlfix_splash` | 1 | splash fix |
| `qlfix_deadHoldable` | 1 | no holdable for a dead player |
| `qlfix_curveTrace` | 1 | curve collision fix on the server |
| `qlfix_shotgunOrigin` | 1 | shotgun origin moved off whole units |

The six switches (the first six rows) decide what is patched and take effect
at the next map load. A value set on the command line or in `server.cfg`
applies from the first map.

Console commands:

- `qlfix_status`: every playing client with their footstep state, recent
  footstep rate and fps (and strikes while the kick is on), plus totals:
  footsteps the server played, missiles reattributed, shotgun origins moved.

Environment:

| Name | |
|---|---|
| `QLFIX_LOG=<file>` | Append every qlfix line to this file, with date, time and server time. Rotate with logrotate's `copytruncate`. |
| `QLFIX_DISABLE=1` | Load but patch nothing. |

## Logs

The console (and so the server log) gets what an admin acts on: the patch
lines at each map load, fps strikes and kicks, and footstep state changes,
at most one line per player per minute:

```
qlfix: slot 3 alpha now FORCED (cadence 0/s over 64 cmds, -/s over 512; 64 of 64 lost, 64 of 512; ~1000 fps)
qlfix: slot 3 alpha now STOCK (cadence 268/s over 64 cmds, 266/s over 512; 2 of 64 lost, 21 of 512; ~247 fps; 4 changes since last line)
```

The `QLFIX_LOG` file gets every change unfolded, and every 30 s a sample of
players close to the line, so you can see who a change of threshold would
affect:

```
[2026-10-28 21:04:12.318 1734512] qlfix: slot 5 bravo near the line (512 cadence 261/s, 60 of 512 lost)
[2026-10-28 21:06:40.102 1882296] qlfix: slot 5 bravo held forced (512 cadence 249/s, 55 of 512 lost)
```

## How it works

Quake Live's game logic lives in `qagamex64.so`, which the server loads with
`dlopen` at every map change and `map_restart`. qlfix replaces `dlopen` and
nothing else. When the game module loads, qlfix checks it and patches it;
every other `dlopen` goes straight through, so other preloads such as minqlx
work as before.

Each fix is a **site**: a few instructions in the game (or, for
`curve-trace`, the server) replaced by a jump to a small piece of code. Most
sites jump to a trampoline that saves the registers and calls a C function,
so their logic lives in C (`src/`), not in hand-written machine code. The
simplest (`splash`, `splash-surface`, `missile-stuck`, `dead-holdable`,
`curve-trace`) need only a few instructions of their own.

On every load qlfix checks the module's size, the exact bytes it will
replace and the bytes around them. A different game build is left untouched
and the console says why. Engine functions (cvars, console output, commands)
are used only after checking the server's build string and the first bytes
of each function; functions another preload has hooked are recognised and
called through the hook. If engine access fails, the console line ends in
"engine access off" with the reason; the game's fixes then run with their
defaults, and `curve-trace`, which patches the server itself, is not
applied.

| Site | Where | Fix |
|---|---|---|
| `footstep` | game `+0x58564` | footstep cycle, end of `PmoveSingle` |
| `footstep-emit` | game `+0x5b92c` | plays forced footsteps, `ClientEndFrame` |
| `splash` | game `+0x73c64` | normal splash for everyone in the radius, `G_RadiusDamage` |
| `splash-surface` | game `+0x74031` | the same for damage through thin surfaces (inactive at default cvars) |
| `missile-stuck` | game `+0x86420` | saves the entity a stuck missile was found inside, `G_RunMissile` |
| `missile-attribute` | game `+0x86454` | names that entity in the missile's trace, so it takes the direct hit, `G_RunMissile` |
| `dead-holdable` | game `+0x5a2f4` | holdables skipped at 0 health, `ClientEvents` |
| `curve-trace` | engine `0x419c2c` | edge-plane test, `CM_TracePointThroughPatchCollide` |
| `shotgun-origin` | game `+0x99c43` | shotgun origin offset, `weapon_supershotgun_fire` |
| `fps` | game `+0x59e5e` | frame timing for the kick, `ClientThink_real` (only with `qlfix_fpsKick 1`) |

Sites that belong together are applied all or none: both footstep sites and
all four splash sites.

## License

Released under the MIT License (see LICENSE)
