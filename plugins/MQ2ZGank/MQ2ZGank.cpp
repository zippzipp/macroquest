// MQ2ZGank.cpp : Defines the entry point for the DLL application.
//
// Reach commands: act on things that are out of range. A clone of MMOBugs' MQ2ReachIt
// (with the parts of MQ2MMOBugs it depends on built in).
//
// Every command sends the same three packets:
//
//   1. an 0xB0B2 movement update that puts us at the target,
//   2. the packet the client would send if we were standing there,
//   3. an 0xB0B2 movement update that puts us back where we are.
//
// The client never moves us. Only the server sees the trip there and back.
//
// Differences from MQ2ReachIt/MQ2MMOBugs:
//
//  * No UdpConnection::SendMessage detour. MMOBugs detours it to learn the movement
//    sequence number from the client's own 0xB0B2 packets. We read the sequence straight
//    out of the client's movement history instead. This also keeps us out of MQ2Packet's
//    way: two detours on the same address cannot coexist.
//  * No CPacketScrambler::hton/ntoh calls. In this client both are identity functions
//    whose only side effect is to increment the counters the client reports in opcode
//    0x7E96. MMOBugs calls them and then has to decrement those counters to cancel its own
//    calls. We never call them, so there is nothing to cancel. We do check that hton is
//    still the identity stub, and refuse to send if a patch changes that.
//  * Headings are packed the way the client packs them ((h + 2048) * 4, mod 2048), not
//    as int(h) & 0xfff.
//  * /sumcorpse's optional delay is handled in OnPulse rather than by Sleep()ing the game
//    thread.
//  * /saytarget and /hailtarget move back afterwards.
//
//   /sumcorpse [delayms]   - drag the targeted corpse to you
//   /gank, /grab           - pick up the current ground item (/itemtarget)
//   /faropen               - open the targeted object
//   /switch <door id>      - click a door or switch (see /doors)
//   /fartaunt [id <n>]     - taunt your target, or spawn <n>
//   /saytarget [text]      - say something to your target (no text = hail)
//   /throw [text]          - alias for /saytarget
//   /hailtarget            - hail your target
//   /zgank [debug|help]
//
// PLUGIN_API is only to be used for callbacks. Always use Initialize and
// Shutdown for setup and cleanup.
//

#include <mq/Plugin.h>

#include "common/includes/eqgame.private.h"

#include <functional>
#include <string>
#include <vector>

PreSetup("MQ2ZGank");
PLUGIN_VERSION(1.0);

constexpr const char* PluginMsg = "\ay[\aoMQ2ZGank\ax]\ax ";

//============================================================================
// Opcodes
//
// These are the values the client passes to CPacketScrambler::hton. hton is currently
// the identity function (see HtonIsIdentity), so they are also the wire values.
//============================================================================

constexpr uint16_t OP_MovementUpdate   = 0xB0B2;   // CEverQuest movement code
constexpr uint16_t OP_CorpseDrag       = 0x2180;
constexpr uint16_t OP_GroundItemPickup = 0xEA86;   // CEverQuest::LMouseUp
constexpr uint16_t OP_ClickObject      = 0xC591;   // open the targeted object; no payload
constexpr uint16_t OP_UseSwitch        = 0xFE47;   // EQSwitch::UseSwitch
constexpr uint16_t OP_Taunt            = 0x3E56;
constexpr uint16_t OP_ChannelMessage   = 0xB45A;

// Every client send site uses channel 4.
constexpr int kSendChannel = 4;

// MQ2MMOBugs refuses anything this size or larger.
constexpr size_t kMaxPayload = 0x1F4;

// Chat channel number for /say.
constexpr uint32_t kChatChannelSay = 8;

//============================================================================
// Packet layouts
//============================================================================

#pragma pack(push, 1)

// 0xB0B2. The client packs this from PlayerClient+0x74 (CPhysicsInfo) in sub_140261e40.
struct MovementUpdate
{
/*0x00*/ uint16_t Sequence;      // MovementHistory+0x30
/*0x02*/ uint16_t SpawnID;
/*0x04*/ uint16_t VehicleID;     // mount spawn id, 0 if none
/*0x06*/ float    Y;
/*0x0a*/ float    SpeedZ;
/*0x0e*/ uint32_t HeadingBits;   // [0:11] EncodeAngle(Heading)
/*0x12*/ float    X;
/*0x16*/ float    Z;
/*0x1a*/ uint32_t SpeedBits;     // [0:9] SpeedHeading * 20, [10:19] SpeedRun * 40
/*0x1e*/ float    SpeedY;
/*0x22*/ float    SpeedX;
/*0x26*/ uint32_t PitchBits;     // [0:11] EncodeAngle(CameraAngle)
/*0x2a*/
};
static_assert(sizeof(MovementUpdate) == 0x2a);

// 0x2180
struct CorpseDrag
{
/*0x00*/ uint32_t CorpseNameLength;   // always 0x40
/*0x04*/ char     CorpseName[0x40];
/*0x44*/ uint32_t DraggerNameLength;  // always 0x40
/*0x48*/ char     DraggerName[0x40];
/*0x88*/ uint8_t  Unknown0x88;
/*0x89*/ uint32_t ZoneID;             // without the instance bits
/*0x8d*/ float    Y;                  // the corpse's location
/*0x91*/ float    X;
/*0x95*/ float    Z;
/*0x99*/ uint32_t Unknown0x99;
/*0x9d*/
};
static_assert(sizeof(CorpseDrag) == 0x9d);

// 0xEA86
struct GroundItemPickup
{
/*0x00*/ uint32_t DropID;
/*0x04*/ uint32_t SpawnID;
/*0x08*/ uint32_t Tick;       // GetTickCount()
/*0x0c*/
};
static_assert(sizeof(GroundItemPickup) == 0x0c);

// 0xFE47. Field order matches EQSwitch::UseSwitch(SpawnID, KeyID, PickSkill, ...).
struct UseSwitch
{
/*0x00*/ uint32_t SpawnID;
/*0x04*/ int32_t  KeyID;      // -1 = no key
/*0x08*/ int32_t  PickSkill;
/*0x0c*/ uint32_t SwitchID;
/*0x10*/
};
static_assert(sizeof(UseSwitch) == 0x10);

// 0x3E56
struct Taunt
{
/*0x00*/ uint32_t TargetID;
/*0x04*/
};
static_assert(sizeof(Taunt) == 0x04);

#pragma pack(pop)

/**
 * @class PacketWriter
 *
 * Serializes variable-length packets. Strings are written NUL-terminated with no
 * length prefix, which is how the client and MQ2ReachIt write them.
 */
class PacketWriter
{
public:
	void U8(uint8_t value) { m_data.push_back(value); }
	void U32(uint32_t value) { Raw(&value, sizeof(value)); }
	void String(const char* value) { Raw(value, strlen(value) + 1); }

	const std::vector<uint8_t>& Data() const { return m_data; }

private:
	void Raw(const void* data, size_t size)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		m_data.insert(m_data.end(), bytes, bytes + size);
	}

	std::vector<uint8_t> m_data;
};

//============================================================================
// State
//============================================================================

static bool s_debug = false;

struct ReachLocation
{
	float Y = 0.0f;
	float X = 0.0f;
	float Z = 0.0f;
	float Heading = 0.0f;
};

// A delayed move back, currently only used by /sumcorpse <delayms>.
struct PendingReturn
{
	bool          Active = false;
	uint64_t      DueTick = 0;  // GetTickCount64()
	ReachLocation Origin;
	bool          DropCorpse = false;
};
static PendingReturn s_pendingReturn;

//============================================================================
// Sending
//============================================================================

using fnSendMessage = bool(*)(void* pConnection, int channel, uint8_t* data, int length);

/**
 * @fn HtonIsIdentity
 *
 * CPacketScrambler::hton is currently
 *
 *     inc  dword ptr [rip+counter]   ; FF 05 rel32
 *     mov  eax, edx                  ; 8B C2
 *     ret                            ; C3
 *
 * So opcodes go out unscrambled, and skipping it leaves the reported counter alone. If a
 * patch starts scrambling opcodes, raw opcodes would reach the server as garbage. Check
 * the shape before every send rather than trusting it.
 */
static bool HtonIsIdentity()
{
	const auto* code = reinterpret_cast<const uint8_t*>(CPacketScrambler__hton);
	if (!code)
		return false;

	return code[0] == 0xFF && code[1] == 0x05
		&& code[6] == 0x8B && code[7] == 0xC2
		&& code[8] == 0xC3;
}

static void DumpPacket(const uint8_t* data, size_t size)
{
	std::string hex;
	const size_t shown = std::min<size_t>(size, 48);

	for (size_t i = 0; i < shown; ++i)
		hex += fmt::format("{:02x}", data[i]);

	if (shown < size)
		hex += "...";

	WriteChatf("%s\a-t[debug]\ax op=\ay%04X\ax len=\ay%d\ax %s", PluginMsg,
		data[0] | (data[1] << 8), static_cast<int>(size), hex.c_str());
}

/**
 * @fn SendGamePacket
 *
 * Send one packet the way every client send site does:
 * UdpConnection::SendMessage(*__gWorld, 4, [opcode][payload], 2 + payload size).
 *
 * @param opcode uint16_t - logical opcode (identical to the wire opcode, see HtonIsIdentity)
 * @param payload const void* - payload bytes, may be null if payloadSize is 0
 * @param payloadSize size_t - payload size, excluding the opcode
 *
 * @return bool - SendMessage's result
 */
static bool SendGamePacket(uint16_t opcode, const void* payload, size_t payloadSize)
{
	if (!HtonIsIdentity())
	{
		WriteChatf("%s\arCPacketScrambler::hton is no longer the identity stub, so opcodes may be scrambled in this client. Nothing was sent.", PluginMsg);
		return false;
	}

	if (payloadSize >= kMaxPayload)
	{
		WriteChatf("%s\arPacket %04X is too large (%d bytes).", PluginMsg, opcode, static_cast<int>(payloadSize));
		return false;
	}

	void* pConnection = __gWorld ? *reinterpret_cast<void**>(__gWorld) : nullptr;
	if (!pConnection)
	{
		WriteChatf("%s\arNot connected.", PluginMsg);
		return false;
	}

	static const auto sendMessage = reinterpret_cast<fnSendMessage>(
		FixEQGameOffset(UdpConnection__SendMessage_x));

	uint8_t buffer[2 + kMaxPayload];
	memcpy(buffer, &opcode, sizeof(opcode));
	if (payloadSize)
		memcpy(buffer + 2, payload, payloadSize);

	const int length = static_cast<int>(payloadSize + 2);

	if (s_debug)
		DumpPacket(buffer, length);

	return sendMessage(pConnection, kSendChannel, buffer, length);
}

//============================================================================
// Movement
//============================================================================

/**
 * @fn EncodeAngle
 *
 * The client packs headings and camera pitch as (value + 2048) * 4, reduced (signed)
 * mod 2048, into a 12-bit field.
 */
static uint32_t EncodeAngle(float value)
{
	const int scaled = static_cast<int>((value + 2048.0f) * 4.0f) % 2048;
	return static_cast<uint32_t>(scaled) & 0xfff;
}

/**
 * @fn NextMovementSequence
 *
 * The sequence number the client will stamp on its next 0xB0B2 packet. The client
 * reads this, sends, and then its history recorder increments it. MQ2MMOBugs uses
 * "last sequence seen going out + 1", which is the same value.
 *
 * Our packet does not advance it, so the client's next movement update reuses the
 * number, exactly as it does under MQ2MMOBugs.
 */
static uint16_t NextMovementSequence()
{
	static const uintptr_t address = FixEQGameOffset(instMovementHistory_x) + MovementHistory__Sequence_off;

	return *reinterpret_cast<const uint16_t*>(address);
}

/**
 * @fn SendMovement
 *
 * Tell the server we are standing at a location, stationary.
 */
static bool SendMovement(float y, float x, float z, float heading)
{
	PlayerClient* pMe = pControlledPlayer ? pControlledPlayer : pLocalPlayer;
	if (!pMe)
		return false;

	MovementUpdate packet = {};
	packet.Sequence = NextMovementSequence();
	packet.SpawnID = static_cast<uint16_t>(pMe->SpawnID);
	packet.Y = y;
	packet.X = x;
	packet.Z = z;
	packet.HeadingBits = EncodeAngle(heading);
	packet.PitchBits = EncodeAngle(pMe->CameraAngle);

	return SendGamePacket(OP_MovementUpdate, &packet, sizeof(packet));
}

static bool SendMovement(const ReachLocation& loc)
{
	return SendMovement(loc.Y, loc.X, loc.Z, loc.Heading);
}

static ReachLocation CurrentLocation()
{
	return { pLocalPlayer->Y, pLocalPlayer->X, pLocalPlayer->Z, pLocalPlayer->Heading };
}

/**
 * @fn ReachAndDo
 *
 * Move to a location, run `action`, move back. We keep facing the way we face now.
 *
 * @return bool - the action's result, or false if the first move failed
 */
static bool ReachAndDo(float y, float x, float z, const std::function<bool()>& action)
{
	const ReachLocation origin = CurrentLocation();

	if (!SendMovement(y, x, z, origin.Heading))
		return false;

	const bool result = action();

	SendMovement(origin);
	return result;
}

/**
 * @fn CanReach
 *
 * Everything the commands touch has to be live, and the movement packet has no way to
 * describe a mount, so refuse while mounted.
 */
static bool CanReach(const char* command)
{
	if (GetGameState() != GAMESTATE_INGAME || !pLocalPlayer || !pLocalPC)
	{
		WriteChatf("%s\arYou must be in game to use %s.", PluginMsg, command);
		return false;
	}

	if (pLocalPlayer->Mount)
	{
		WriteChatf("%s\ayYou can't use %s while mounted.", PluginMsg, command);
		return false;
	}

	return true;
}

//============================================================================
// /sumcorpse
//============================================================================

static void FinishCorpseDrag()
{
	// The dragged corpse follows us from here on; drop it where we stand.
	DoCommand("/squelch /timed 10 /corpsedrop");
}

static void Command_SumCorpse(PlayerClient* pChar, const char* szLine)
{
	if (!CanReach("/sumcorpse"))
		return;

	PlayerClient* pCorpse = pTarget;
	if (!pCorpse || pCorpse->Type != SPAWN_CORPSE)
	{
		WriteChatf("%s\arYou need a corpse targeted to summon.", PluginMsg);
		return;
	}

	if (s_pendingReturn.Active)
	{
		WriteChatf("%s\ayStill waiting to move back from the last /sumcorpse.", PluginMsg);
		return;
	}

	char arg1[MAX_STRING] = { 0 };
	GetArg(arg1, szLine, 1);
	const int delayMs = std::max(0, GetIntFromString(arg1, 0));

	CorpseDrag packet = {};
	packet.CorpseNameLength = sizeof(packet.CorpseName);
	strncpy_s(packet.CorpseName, pCorpse->Name, _TRUNCATE);
	packet.DraggerNameLength = sizeof(packet.DraggerName);
	strncpy_s(packet.DraggerName, pLocalPlayer->Name, _TRUNCATE);
	packet.ZoneID = pLocalPC->zoneId;
	packet.Y = pCorpse->Y;
	packet.X = pCorpse->X;
	packet.Z = pCorpse->Z;

	const ReachLocation origin = CurrentLocation();

	if (!SendMovement(pCorpse->Y, pCorpse->X, pCorpse->Z, origin.Heading))
		return;

	SendGamePacket(OP_CorpseDrag, &packet, sizeof(packet));

	if (delayMs > 0)
	{
		s_pendingReturn.Active = true;
		s_pendingReturn.DueTick = GetTickCount64() + delayMs;
		s_pendingReturn.Origin = origin;
		s_pendingReturn.DropCorpse = true;
	}
	else
	{
		SendMovement(origin);
		FinishCorpseDrag();
	}

	WriteChatf("%s\agDragged '\ax%s\ag'.", PluginMsg, pCorpse->DisplayedName);
}

//============================================================================
// /gank, /grab
//============================================================================

static void Command_Gank(PlayerClient* pChar, const char* szLine)
{
	if (!CanReach("/gank"))
		return;

	if (pLocalPC->GetInventorySlot(InvSlot_Cursor))
	{
		WriteChatf("%s\ayPlease get rid of the item on your cursor before using /gank.", PluginMsg);
		return;
	}

	if (!HasCurrentGroundSpawn())
	{
		WriteChatf("%s\ayPlease use /itemtarget to acquire a target.", PluginMsg);
		return;
	}

	const MQGroundSpawn ground = CurrentGroundSpawn();
	const CVector3 pos = ground.Position();

	GroundItemPickup packet = {};
	packet.DropID = static_cast<uint32_t>(ground.ID());
	packet.SpawnID = pLocalPlayer->SpawnID;
	packet.Tick = GetTickCount();

	ReachAndDo(pos.Y, pos.X, pos.Z, [&] {
		return SendGamePacket(OP_GroundItemPickup, &packet, sizeof(packet));
	});

	ClearGroundSpawn();
}

//============================================================================
// /faropen
//============================================================================

static void Command_FarOpen(PlayerClient* pChar, const char* szLine)
{
	if (!CanReach("/faropen"))
		return;

	PlayerClient* pObject = pTarget;
	if (!pObject)
	{
		WriteChatf("%s\arYou need an object targeted to open.", PluginMsg);
		return;
	}

	ReachAndDo(pObject->Y, pObject->X, pObject->Z, [] {
		return SendGamePacket(OP_ClickObject, nullptr, 0);
	});
}

//============================================================================
// /switch
//============================================================================

static void Command_Switch(PlayerClient* pChar, const char* szLine)
{
	char arg1[MAX_STRING] = { 0 };
	GetArg(arg1, szLine, 1);

	if (arg1[0] == 0)
	{
		WriteChatf("%s\ayUsage: \am/switch <door id>\ax. Use \ag/doors\ax for a list of doors and switches in the zone.", PluginMsg);
		return;
	}

	if (!CanReach("/switch"))
		return;

	const int switchId = GetIntFromString(arg1, -1);

	EQSwitch* pSwitch = pSwitchMgr ? pSwitchMgr->GetSwitchById(switchId) : nullptr;
	if (!pSwitch)
	{
		WriteChatf("%s\ayCouldn't find door/switch '\ax%s\ay'.", PluginMsg, arg1);
		return;
	}

	pSwitchTarget = pSwitch;

	UseSwitch packet = {};
	packet.SpawnID = pLocalPlayer->SpawnID;
	packet.KeyID = -1;
	packet.PickSkill = 0;
	packet.SwitchID = pSwitch->ID;

	ReachAndDo(pSwitch->Y, pSwitch->X, pSwitch->Z, [&] {
		return SendGamePacket(OP_UseSwitch, &packet, sizeof(packet));
	});

	WriteChatf("%s\agClicked '\ax%s\ag'.", PluginMsg, pSwitch->Name);
}

//============================================================================
// /fartaunt
//============================================================================

static void Command_FarTaunt(PlayerClient* pChar, const char* szLine)
{
	char arg1[MAX_STRING] = { 0 };
	char arg2[MAX_STRING] = { 0 };
	GetArg(arg1, szLine, 1);
	GetArg(arg2, szLine, 2);

	if (ci_equals(arg1, "help"))
	{
		WriteChatf("%s\am/fartaunt\ax             \at- Taunt your target.", PluginMsg);
		WriteChatf("%s\am/fartaunt id <spawnid>\ax \at- Target and taunt the mob with that spawn id.", PluginMsg);
		return;
	}

	if (!CanReach("/fartaunt"))
		return;

	if (ci_equals(arg1, "id"))
	{
		const int spawnId = GetIntFromString(arg2, 0);

		PlayerClient* pSpawn = pSpawnManager ? pSpawnManager->GetSpawnByID(spawnId) : nullptr;
		if (!pSpawn)
		{
			WriteChatf("%s\ayCould not find spawn matching id %d.", PluginMsg, spawnId);
			return;
		}

		pTarget = pSpawn;
	}

	PlayerClient* pMob = pTarget;
	if (!pMob || pMob->Type != SPAWN_NPC)
	{
		WriteChatf("%s\arYou must target an NPC to use this command. See /fartaunt help.", PluginMsg);
		return;
	}

	Taunt packet = {};
	packet.TargetID = pMob->SpawnID;

	// Stand one unit short of the mob rather than inside it.
	ReachAndDo(pMob->Y, pMob->X - 1.0f, pMob->Z, [&] {
		return SendGamePacket(OP_Taunt, &packet, sizeof(packet));
	});
}

//============================================================================
// /saytarget, /throw, /hailtarget
//============================================================================

/**
 * @fn BuildSayPacket
 *
 * 0xB45A as MQ2ReachIt builds it for a /say to one NPC.
 */
static std::vector<uint8_t> BuildSayPacket(const char* sender, const char* target, const char* text, bool hail)
{
	PacketWriter writer;
	writer.String(sender);
	writer.String(target);
	writer.U32(0);
	writer.U32(0);
	writer.U32(0);
	writer.U32(kChatChannelSay);
	writer.U8(hail ? 1 : 0);    // MQ2ReachIt sets this only when hailing
	writer.U32(0);
	writer.U32(0x100);
	writer.String(text);
	writer.U32(0);
	writer.U32(0);

	return writer.Data();
}

static void SayToTarget(const char* szText, const char* command)
{
	if (!CanReach(command))
		return;

	PlayerClient* pNpc = pTarget;
	if (!pNpc)
	{
		WriteChatf("%s\ayYou must have a target to throw your voice.", PluginMsg);
		return;
	}

	if (pNpc->Type != SPAWN_NPC)
	{
		WriteChatf("%s\ayYou can only throw your voice to an NPC!", PluginMsg);
		return;
	}

	const bool hail = !szText || szText[0] == 0;

	char text[256] = { 0 };
	if (hail)
		sprintf_s(text, "Hail, %s", pNpc->DisplayedName);
	else
		strncpy_s(text, szText, _TRUNCATE);

	const std::vector<uint8_t> packet = BuildSayPacket(pLocalPlayer->Name, pNpc->Name, text, hail);

	ReachAndDo(pNpc->Y, pNpc->X, pNpc->Z, [&] {
		return SendGamePacket(OP_ChannelMessage, packet.data(), packet.size());
	});
}

static void Command_SayTarget(PlayerClient* pChar, const char* szLine)
{
	SayToTarget(szLine, "/saytarget");
}

static void Command_HailTarget(PlayerClient* pChar, const char* szLine)
{
	SayToTarget("", "/hailtarget");
}

//============================================================================
// /zgank
//============================================================================

static void ShowHelp()
{
	WriteChatf("%sUsage:", PluginMsg);
	WriteChatf("\am/sumcorpse [delayms]\ax  \at- Drag the targeted corpse to you, optionally waiting before moving back.");
	WriteChatf("\am/gank\ax, \am/grab\ax          \at- Pick up the current ground item (/itemtarget).");
	WriteChatf("\am/faropen\ax              \at- Open the targeted object.");
	WriteChatf("\am/switch <door id>\ax     \at- Click a door or switch (see /doors).");
	WriteChatf("\am/fartaunt [id <n>]\ax    \at- Taunt your target, or spawn <n>.");
	WriteChatf("\am/saytarget [text]\ax     \at- Say something to your target. No text hails it. \am/throw\ax is an alias.");
	WriteChatf("\am/hailtarget\ax           \at- Hail your target.");
	WriteChatf("\am/zgank debug [on|off]\ax \at- Print every packet MQ2ZGank sends.");
}

static void Command_ZGank(PlayerClient* pChar, const char* szLine)
{
	char arg1[MAX_STRING] = { 0 };
	char arg2[MAX_STRING] = { 0 };
	GetArg(arg1, szLine, 1);
	GetArg(arg2, szLine, 2);

	if (ci_equals(arg1, "debug"))
	{
		if (arg2[0] == 0)
			s_debug = !s_debug;
		else
			s_debug = GetBoolFromString(arg2, s_debug);

		WriteChatf("%sDebug is now %s\ax.", PluginMsg, s_debug ? "\agon" : "\aroff");
		return;
	}

	ShowHelp();

	if (!HtonIsIdentity())
		WriteChatf("%s\arWarning: CPacketScrambler::hton is not the identity stub in this client. Sending is disabled.", PluginMsg);
}

//============================================================================
// Plugin callbacks
//============================================================================

/**
 * @fn InitializePlugin
 *
 * This is called once on plugin initialization and can be considered the startup
 * routine for the plugin.
 */
PLUGIN_API void InitializePlugin()
{
	DebugSpewAlways("MQ2ZGank::Initializing version %f", MQ2Version);

	AddCommand("/sumcorpse", Command_SumCorpse, false, true, true);
	AddCommand("/gank", Command_Gank, false, true, true);
	AddCommand("/grab", Command_Gank, false, true, true);
	AddCommand("/faropen", Command_FarOpen, false, true, true);
	AddCommand("/switch", Command_Switch, false, true, true);
	AddCommand("/fartaunt", Command_FarTaunt, false, true, true);
	AddCommand("/saytarget", Command_SayTarget, false, true, true);
	AddCommand("/throw", Command_SayTarget, false, true, true);
	AddCommand("/hailtarget", Command_HailTarget, false, true, true);
	AddCommand("/zgank", Command_ZGank, false, true, true);
}

/**
 * @fn ShutdownPlugin
 *
 * This is called once when the plugin has been asked to shutdown. The plugin has
 * not actually shut down until this completes.
 */
PLUGIN_API void ShutdownPlugin()
{
	DebugSpewAlways("MQ2ZGank::Shutting down");

	// Don't leave the server thinking we are standing somewhere else.
	if (s_pendingReturn.Active && GetGameState() == GAMESTATE_INGAME)
		SendMovement(s_pendingReturn.Origin);
	s_pendingReturn.Active = false;

	RemoveCommand("/sumcorpse");
	RemoveCommand("/gank");
	RemoveCommand("/grab");
	RemoveCommand("/faropen");
	RemoveCommand("/switch");
	RemoveCommand("/fartaunt");
	RemoveCommand("/saytarget");
	RemoveCommand("/throw");
	RemoveCommand("/hailtarget");
	RemoveCommand("/zgank");
}

/**
 * @fn OnPulse
 *
 * Sends the delayed move back for /sumcorpse <delayms>.
 */
PLUGIN_API void OnPulse()
{
	if (!s_pendingReturn.Active || GetTickCount64() < s_pendingReturn.DueTick)
		return;

	s_pendingReturn.Active = false;

	if (GetGameState() != GAMESTATE_INGAME || !pLocalPlayer)
		return;

	SendMovement(s_pendingReturn.Origin);

	if (s_pendingReturn.DropCorpse)
		FinishCorpseDrag();
}

/**
 * @fn SetGameState
 *
 * This is called when the GameState changes. It is also called once after the
 * plugin is initialized.
 */
PLUGIN_API void SetGameState(int GameState)
{
	if (GameState != GAMESTATE_INGAME)
		s_pendingReturn.Active = false;
}

/**
 * @fn OnBeginZone
 *
 * A pending move back belongs to the zone we are leaving.
 */
PLUGIN_API void OnBeginZone()
{
	s_pendingReturn.Active = false;
}
