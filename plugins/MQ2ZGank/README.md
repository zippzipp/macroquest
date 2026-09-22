# MQ2ZGank

Reach commands: act on things that are out of range. This is a clone of MMOBugs'
MQ2ReachIt, with the parts of MQ2MMOBugs that it depends on built in.

Each command sends a movement update that puts you at the target, then the packet the
client would send if you were standing there, then a movement update that puts you back.
The client never moves you.

## Commands

| Command | Packet | Notes |
|---|---|---|
| `/sumcorpse [delayms]` | 0x2180 corpse drag | Target a corpse. With a delay, the move back is sent from OnPulse. `/corpsedrop` runs 1s after the move back. |
| `/gank`, `/grab` | 0xEA86 ground item pickup | Uses the current `/itemtarget`. Your cursor must be empty. |
| `/faropen` | 0xC591 (no payload) | Opens the targeted object. |
| `/switch <door id>` | 0xFE47 use switch | Door ids come from `/doors`. |
| `/fartaunt [id <n>]` | 0x3E56 taunt | Stands 1 unit short of the mob on X. |
| `/saytarget [text]`, `/throw [text]` | 0xB45A channel message | Says the text to the targeted NPC. With no text, it hails. |
| `/hailtarget` | 0xB45A | `Hail, <name>` to the targeted NPC. |
| `/zgank debug [on\|off]` | | Prints every packet the plugin sends. |

All commands refuse to run while you are mounted: the movement update would need the
mount's spawn id.

MQ2ReachIt's `/autoreach`, `/ubertrade` and `/usemerch` are not included. The last two
already print "not working" in MQ2ReachIt. `/autoreach` needs a hook on the client's
merchant buy/sell sends.

## Offsets (`plugins/common/includes/eqgame.private.h`)

| Define | Sep 11 2026 | What it is |
|---|---|---|
| `UdpConnection__SendMessage_x` | `0x14056F310` | `bool SendMessage(this, int channel, uint8_t* data, int len)`, called as `(*__gWorld, 4, [opcode][payload], len)` |
| `instMovementHistory_x` | `0x140D833D0` | static object; the u16 at `+0x30` is the next 0xB0B2 sequence number |

`CPacketScrambler__hton` and `__gWorld` come from eqlib.

## How it differs from MMOBugs

- **No `SendMessage` detour.** MMOBugs detours `SendMessage` to watch the client's own
  0xB0B2 packets for the sequence number. The client keeps that number in its movement
  history (read, send, then its history recorder increments it), so we read it directly.
  Without a detour, this plugin also runs alongside MQ2Packet, which detours the same
  function.
- **No `hton`/`ntoh` calls.** In this client, `CPacketScrambler::hton` is
  `inc [0x140F86EF4]; mov eax, edx; ret` and `ntoh` is the same against `0x140F86EF0`.
  Every legit send site decrements the counter after sending, and the client reports both
  counters to the server in opcode 0x7E96. MMOBugs calls `hton`/`ntoh` and then decrements
  the counters to cancel its own calls. We never call them, so the counters are untouched.
  Before every send we check that `hton` still has that identity shape, and refuse to send
  if it doesn't.
- **Headings are packed like the client packs them:** `((h + 2048) * 4) mod 2048`, in 12
  bits. MMOBugs sends `int(h) & 0xFFF`.
- `/sumcorpse`'s delay doesn't `Sleep()` the game thread, and `/saytarget` moves back.

The injected 0xB0B2 doesn't advance the sequence number, so the client's next movement
update reuses it. MMOBugs does the same.

## After a patch

1. `UdpConnection__SendMessage_x`: the function called right after `hton` at every send
   site (~900 callers). MQ2Packet needs the same offset.
2. `instMovementHistory_x`: the address returned by the small function called just before
   each `mov edx, 0xB0B2` / `hton` pair. `+0x30` is read into the packet's first word.
3. Opcodes: confirm each one still appears as `mov edx, <op>` before `hton` in the matching
   client function.
4. Load the plugin and run `/zgank`. It warns if `hton` has stopped being the identity stub.
