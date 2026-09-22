# MQ2ZWarp

Position warping, plus named waypoints and a couple of small location utilities.

## How the warp works

The teleport primitive is the client's own `CDisplay::MoveLocalPlayerToSafeCoords()`
— the routine that drops you on the zone's safe point when you `/succor` or get
rescued from the world geometry.

Writing `pLocalPlayer->Y/X/Z` directly moves the position field but leaves velocity,
collision state, the cached floor height and the view actor pointing at where you
used to be, which is why naive warps make you fall, clip or rubber-band. Instead
MQ2ZWarp:

1. saves `pZoneInfo->Safe{Y,X,Z}Loc`,
2. overwrites them with the destination,
3. calls `CDisplay::MoveLocalPlayerToSafeCoords()`, letting the client relocate you
   properly (player, controlled player, view actor, floor height, camera),
4. puts the real safe coords back,
5. sets `pLocalPlayer->Heading`.

### What this does *not* do

`MoveLocalPlayerToSafeCoords()` is the client's own relocate routine, and the client
instruments it. Two things happen that MQ2ZWarp does not currently suppress:

1. It calls the relocate notifier (`0x14030C8C0`) with **reason 3**, which queues a
   `{position, reason}` record onto the client's anomaly reporter (`0x1402DE160`).
2. It increments a teleport counter at **`0x140E03140`**, which the client packages
   into an opcode-`0x174` message and sends to the server every 525 seconds
   (`0x1401AA260`).

MQ2MMOBugs cancels both: it pre-decrements the counter so the `inc` nets to zero, and
it detours the reporter to rewrite reason 3 into reason 1 exactly once per warp
(reason 1 is position-deduplicated on a 1000 ms window, so it usually drops).

See "Parity gap" below.

### The offset

`CDisplay::MoveLocalPlayerToSafeCoords` is not currently wired up in eqlib (no
`CDisplay__MoveLocalPlayerToSafeCoords_x` define), so the address lives in
`plugins/common/includes/eqgame.private.h` and is fixed up at runtime with
`FixEQGameOffset()`, the same way the other private offsets in this tree work.

**This offset has to be re-verified every client patch.** To relocate it, scan
eqgame.exe for the function that reads `instEQZoneInfo + 0x1fc/0x200/0x204`
(`SafeYLoc`/`SafeXLoc`/`SafeZLoc`) and writes them to `pinstLocalPlayer + 0x74/0x78/0x7c`;
it also repeats the same writes against `pinstControlledPlayer` and updates the
actor hanging off `pinstLocalPC`. It is a ~610 byte function in the `CDisplay`
cluster and takes no meaningful arguments.

## Commands

### `/zwarp` (alias `/warp`)

| Command | Description |
| --- | --- |
| `/zwarp help` | Show usage |
| `/zwarp <dist>` | Warp forward `<dist>` units |
| `/zwarp (s)uccor` | Move to the zone's safe point |
| `/zwarp last` | Move to the last warp destination |
| `/zwarp return` | Return to the location prior to the last warp |
| `/zwarp loc <y> <x> [z]` | Warp to a specific location (current Z if omitted) |
| `/zwarp dir <dist>` | Warp forward a distance |
| `/zwarp id <n> [<n\|s\|e\|w> <dist>]` | Warp to a spawn id, or that far from it |
| `/zwarp (t)arget [<n\|s\|e\|w> <dist>]` | Warp to your target, or that far from it |
| `/zwarp (rt)arget [maxdist]` | Warp to your target, random direction, random 2-10 (or 2-`maxdist`) units out |
| `/zwarp (i)tem [<n\|s\|e\|w> <dist>]` | Warp to the current ground item (`/itemtarget` first) |
| `/zwarp wp <name>` | Warp to a named waypoint |
| `/zwarp compass` | Warp to the location the main compass line points at |
| `/zwarp (b)ehind [dist]` | Warp behind your target (default 5) |
| `/zwarp (f)ront [dist]` | Warp in front of your target |
| `/zwarp (l)eft [dist]` | Warp to the left of your target |
| `/zwarp (r)ight [dist]` | Warp to the right of your target |

Cardinal offsets follow EQ's axes: north is `+Y`, west is `+X`.

Spawn-relative warps use the spawn's `FloorHeight` rather than its `Z`, so you land
on the ground under a mounted, levitating or falling target instead of inside it.

### `/waypoint`

| Command | Description |
| --- | --- |
| `/waypoint add <name>` | Save the current location |
| `/waypoint update <name>` | Move an existing waypoint to the current location |
| `/waypoint delete <name>` | Remove a waypoint |
| `/waypoint list` | List saved waypoints with their zones |

Waypoints are per character, stored in `config\MQ2ZWarp_<Server>_<Character>.ini`:

```ini
[Waypoints]
bank=1234.56 -789.01 3.00 128.00:poknowledge
```

### `/exactloc`

Print your location at full precision (`%3.6f`), rather than the two decimals `/loc`
gives you.

### `/setgrav`

| Command | Description |
| --- | --- |
| `/setgrav` | Show the current and default zone gravity |
| `/setgrav <value>` | Override `pZoneInfo->ZoneGravity` |
| `/setgrav default` | Restore the gravity the zone loaded with |

The zone default is captured on zone-in, so `default` works for the zone you are
standing in.

## Data types

### `${ZWarp}`

| Member | Type | Description |
| --- | --- | --- |
| `TimeSinceWarp` | timestamp | Milliseconds since the last warp, 0 if you have not warped |
| `WarpedInZone` | bool | TRUE if you have warped since entering this zone |
| `Last` | string | Last warp destination as `y x z` |
| `Return` | string | Position prior to the last warp as `y x z` |

### `${Waypoint[name]}`

| Member | Type | Description |
| --- | --- | --- |
| `Name` | string | The waypoint name as given |
| `Exists` | bool | TRUE if the waypoint is saved |
| `Loc` | string | `y x z` |
| `Y` / `YCoord` | float | Y coordinate |
| `X` / `XCoord` | float | X coordinate |
| `Z` / `ZCoord` | float | Z coordinate |
| `Heading` | float | Heading saved with the waypoint (0-512) |
| `Zone` | string | Short name of the zone it was saved in |

`Exists` is safe to query for any name; the other members return NULL when the
waypoint is not found.

## Notes

- Warping is trivially visible to the server (your position jumps). Nothing here
  hides that, and see "What this does not do" above for the two client-side
  instrumentation paths that are currently left intact.
- `/warp` is registered as an alias. If another loaded plugin already owns `/warp`,
  MacroQuest will log a duplicate-command error and only `/zwarp` will work.
- `last` / `return` are cleared on zone, along with `${ZWarp.WarpedInZone}`.

## Parity gap

MQ2MMOWarp, via MQ2MMOBugs, installs three detours (names taken from MMOBugs' own
debug strings): `CEverQuest__HandleWorldMessage`, `UdpConnection_MMOBugs::SendMessage`,
and `Globals->WarpDetection`. Only the last is needed for warping, along with the
counter decrement. Current-client addresses:

| What | Address (2026-09-11) |
| --- | --- |
| Anomaly reporter (`reporter*, float pos[3], int reason`) | `0x1402DE160` |
| Relocate notifier (`PlayerClient*, int reason, float* pos, void*`) | `0x14030C8C0` |
| Teleport counter (dword) | `0x140E03140` |
| Periodic report of the counter (opcode `0x174`) | `0x1401AA260` |

Reason codes passed to the notifier across the client: 1, 2, 3, 4, 5, 7, 9. Reason 3
is the forced-relocate class used by `MoveLocalPlayerToSafeCoords` and several other
teleport sites.
