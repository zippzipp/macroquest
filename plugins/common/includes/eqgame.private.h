
// CDisplay::MoveLocalPlayerToSafeCoords() -- relocates the local player (and the
// controlled player / view actor) to pZoneInfo->Safe{Y,X,Z}Loc, resetting velocity,
// collision and camera state on the way. Used by MQ2ZWarp: overwrite the zone's safe
// coords, call this, restore them. Located by its rip-relative reads of
// instEQZoneInfo+0x1fc/0x200/0x204 writing pinstLocalPlayer+0x74/0x78/0x7c.
#define CDisplay__MoveLocalPlayerToSafeCoords_x  0x1401a7890

// UdpConnection::SendMessage(this, int channel, uint8_t* data, int len) -> bool.
// Every client send site calls it as SendMessage(*__gWorld, 4, buf, 2 + payload), with
// the opcode in the first two bytes of buf. 924 callers. Used by MQ2ZGank. MQ2Packet
// has its own copy of this offset (PACKET_DEFAULT_SEND_OFFSET).
#define UdpConnection__SendMessage_x             0x14056f310

// The movement-history singleton (a static object, not a pointer). The u16 at +0x30 is
// the sequence number stamped into every outgoing 0xB0B2 movement packet. The client
// reads it, sends, and then its history recorder (0x1402de160) increments it. Located
// through sub_1402de5d0, which returns this address and is called right before each
// 0xB0B2 send in CEverQuest's movement code.
#define instMovementHistory_x                    0x140d833d0
#define MovementHistory__Sequence_off            0x30
