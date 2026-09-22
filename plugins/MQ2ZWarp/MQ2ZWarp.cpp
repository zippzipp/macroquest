// MQ2ZWarp.cpp : Defines the entry point for the DLL application.
//
// Position warping.
//
// The teleport primitive is CDisplay::MoveLocalPlayerToSafeCoords(), the client's
// own "put the player at the zone's safe point" routine. Rather than poking
// pLocalPlayer->Y/X/Z directly (which leaves velocity, collision state, the floor
// height cache and the view actor stale, so you fall, rubber-band or clip), we
// temporarily rewrite pZoneInfo->Safe{Y,X,Z}Loc to the destination, let the client
// relocate us properly, then put the real safe coords back.
//
// NOTE: this is not silent. MoveLocalPlayerToSafeCoords also (a) queues a reason-3
// "relocated" event to the client's own anomaly reporter and (b) increments a
// teleport counter that the client transmits to the server every ~525 seconds.
// MQ2MMOWarp/MQ2MMOBugs cancel both; MQ2ZWarp currently does not. See README.
//
//   /zwarp ...      - warp (see /zwarp help)
//   /warp ...       - alias for /zwarp
//   /waypoint ...   - manage named waypoints
//   /exactloc       - print full-precision location
//   /setgrav        - override zone gravity
//
// PLUGIN_API is only to be used for callbacks. Always use Initialize and
// Shutdown for setup and cleanup.
//

#include <mq/Plugin.h>

#include "common/includes/eqgame.private.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

PreSetup("MQ2ZWarp");
PLUGIN_VERSION(1.0);

constexpr const char* PluginMsg = "\ay[\aoMQ2ZWarp\ax]\ax ";

// EQ headings run 0-512 counter-clockwise from north.
constexpr double kPi = 3.14159265358979323846;
constexpr float  kHeadingToRadians = static_cast<float>(kPi / 256.0);

// INI section holding the named waypoints.
constexpr const char* kWaypointSection = "Waypoints";

//============================================================================
// State
//============================================================================

struct WarpLocation
{
	float Y = 0.0f;
	float X = 0.0f;
	float Z = 0.0f;
	float Heading = 0.0f;
	bool  Valid = false;
};

// Where the last /zwarp put us, and where we were standing before it.
static WarpLocation s_lastDestination;
static WarpLocation s_priorPosition;

static uint64_t s_lastWarpTime = 0;   // GetTickCount64() at the last warp, 0 = never
static bool     s_warpedInZone = false;

// Zone gravity as the client shipped it, so /setgrav default can restore it.
static float s_defaultGravity = 0.0f;
static bool  s_haveDefaultGravity = false;

static std::mt19937 s_rng{ std::random_device{}() };

//============================================================================
// The warp primitive
//============================================================================

using fnMoveLocalPlayerToSafeCoords = void(*)(void* pThis);

/**
 * @fn MoveLocalPlayerToSafeCoords
 *
 * Resolve CDisplay::MoveLocalPlayerToSafeCoords for the running eqgame.exe.
 *
 * The address is a static offset (see common/includes/eqgame.private.h) fixed up
 * for the actual load base, the same way the other private offsets in this tree
 * are handled.
 */
static void MoveLocalPlayerToSafeCoords()
{
	static const auto fn = reinterpret_cast<fnMoveLocalPlayerToSafeCoords>(
		FixEQGameOffset(CDisplay__MoveLocalPlayerToSafeCoords_x));

	// The function ignores its `this` and works entirely off globals, but pass
	// pDisplay anyway so the call is well-formed if that ever changes.
	fn(static_cast<CDisplay*>(pDisplay));
}

/**
 * @fn CanWarp
 *
 * Everything the warp primitive touches has to be live before we call it.
 */
static bool CanWarp()
{
	if (GetGameState() != GAMESTATE_INGAME)
	{
		WriteChatf("%s\arYou must be in game to warp.", PluginMsg);
		return false;
	}

	if (!pZoneInfo || !pLocalPlayer || !pDisplay)
	{
		WriteChatf("%s\arGame state is not ready to warp.", PluginMsg);
		return false;
	}

	return true;
}

/**
 * @fn WarpTo
 *
 * Move the local player to a location.
 *
 * @param y float - destination Y (north is positive)
 * @param x float - destination X (west is positive)
 * @param z float - destination Z
 * @param heading float - heading to face on arrival (0-512)
 *
 * @return bool - true if the warp was performed
 */
static bool WarpTo(float y, float x, float z, float heading)
{
	if (!CanWarp())
		return false;

	// Remember where we came from before we move.
	s_priorPosition = { pLocalPlayer->Y, pLocalPlayer->X, pLocalPlayer->Z, pLocalPlayer->Heading, true };

	// Point the zone's safe coords at the destination, relocate, put them back.
	// Heading is deliberately left alone here: the client derives the arrival
	// facing from SafeHeading, and we want to control it ourselves below.
	const float savedY = pZoneInfo->SafeYLoc;
	const float savedX = pZoneInfo->SafeXLoc;
	const float savedZ = pZoneInfo->SafeZLoc;

	pZoneInfo->SafeYLoc = y;
	pZoneInfo->SafeXLoc = x;
	pZoneInfo->SafeZLoc = z;

	MoveLocalPlayerToSafeCoords();

	pZoneInfo->SafeYLoc = savedY;
	pZoneInfo->SafeXLoc = savedX;
	pZoneInfo->SafeZLoc = savedZ;

	pLocalPlayer->Heading = heading;

	s_lastDestination = { y, x, z, heading, true };
	s_lastWarpTime = GetTickCount64();
	s_warpedInZone = true;

	return true;
}

/**
 * @fn WarpToKeepHeading
 *
 * Warp without changing which way we are facing.
 */
static bool WarpToKeepHeading(float y, float x, float z)
{
	if (!pLocalPlayer)
		return false;

	return WarpTo(y, x, z, pLocalPlayer->Heading);
}

//============================================================================
// Geometry helpers
//============================================================================

/**
 * @fn OffsetByHeading
 *
 * Move a Y/X pair `dist` units along `heading`. Negative dist goes backwards.
 */
static void OffsetByHeading(float& y, float& x, float heading, float dist)
{
	const float radians = heading * kHeadingToRadians;

	y += dist * std::cos(radians);
	x += dist * std::sin(radians);
}

/**
 * @fn OffsetByCardinal
 *
 * Apply an "n|s|e|w <dist>" offset. North is +Y, west is +X.
 *
 * @return bool - false if the direction was not one of n/s/e/w
 */
static bool OffsetByCardinal(const char* dir, float dist, float& y, float& x)
{
	if (ci_equals(dir, "n") || ci_equals(dir, "north"))
		y += dist;
	else if (ci_equals(dir, "s") || ci_equals(dir, "south"))
		y -= dist;
	else if (ci_equals(dir, "w") || ci_equals(dir, "west"))
		x += dist;
	else if (ci_equals(dir, "e") || ci_equals(dir, "east"))
		x -= dist;
	else
		return false;

	return true;
}

/**
 * @fn WarpRelativeToSpawn
 *
 * Warp to a spawn, optionally offset "n|s|e|w <dist>" from it. Uses the spawn's
 * floor height rather than its Z so we do not end up inside a model that is in
 * the air (mounted, levitating, falling).
 */
static bool WarpRelativeToSpawn(PlayerClient* pSpawn, const char* dirArg, const char* distArg)
{
	if (!pSpawn)
		return false;

	float y = pSpawn->Y;
	float x = pSpawn->X;
	const float z = pSpawn->FloorHeight;

	if (dirArg && dirArg[0] != 0)
	{
		const float dist = GetFloatFromString(distArg, 0.0f);

		if (!OffsetByCardinal(dirArg, dist, y, x))
		{
			WriteChatf("%s\arUnknown direction '\ax%s\ar'. Use n, s, e or w.", PluginMsg, dirArg);
			return false;
		}
	}

	return WarpToKeepHeading(y, x, z);
}

//============================================================================
// Waypoints
//============================================================================

struct Waypoint
{
	std::string Name;
	std::string Zone;
	float Y = 0.0f;
	float X = 0.0f;
	float Z = 0.0f;
	float Heading = 0.0f;
	bool  Exists = false;
};

/**
 * @fn CurrentZoneShortName
 */
static const char* CurrentZoneShortName()
{
	if (!pZoneInfo)
		return "unknown";

	const char* shortName = GetShortZone(pZoneInfo->ZoneID);
	return shortName ? shortName : "unknown";
}

/**
 * @fn UpdateIniFileName
 *
 * Waypoints are per character, so the INI name can only be built once we are in
 * game with a name to use.
 */
static void UpdateIniFileName()
{
	if (pLocalPC && pLocalPC->Name[0] != 0)
		sprintf_s(INIFileName, "%s\\MQ2ZWarp_%s_%s.ini", gPathConfig, GetServerShortName(), pLocalPC->Name);
	else
		sprintf_s(INIFileName, "%s\\MQ2ZWarp.ini", gPathConfig);
}

/**
 * @fn LoadWaypoint
 *
 * Read one waypoint out of the INI. Stored as "y x z heading:zone".
 */
static Waypoint LoadWaypoint(const std::string& name)
{
	Waypoint wp;
	wp.Name = name;

	const std::string value = GetPrivateProfileString(kWaypointSection, name, "", INIFileName);
	if (value.empty())
		return wp;

	char zone[MAX_STRING] = { 0 };
	if (sscanf_s(value.c_str(), "%f %f %f %f:%s", &wp.Y, &wp.X, &wp.Z, &wp.Heading, zone, static_cast<unsigned>(sizeof(zone))) < 4)
		return wp;

	wp.Zone = zone;
	wp.Exists = true;
	return wp;
}

/**
 * @fn SaveWaypoint
 *
 * Write the current position out under `name`.
 */
static void SaveWaypoint(const std::string& name)
{
	const std::string value = fmt::format("{:.2f} {:.2f} {:.2f} {:.2f}:{}",
		pLocalPlayer->Y, pLocalPlayer->X, pLocalPlayer->Z, pLocalPlayer->Heading, CurrentZoneShortName());

	WritePrivateProfileString(kWaypointSection, name, value, INIFileName);
}

//============================================================================
// ${ZWarp} and ${Waypoint[name]}
//============================================================================

class MQ2ZWarpType : public MQ2Type
{
public:
	enum class ZWarpMembers
	{
		TimeSinceWarp,
		WarpedInZone,
		Last,
		Return,
	};

	MQ2ZWarpType() : MQ2Type("ZWarp")
	{
		ScopedTypeMember(ZWarpMembers, TimeSinceWarp);
		ScopedTypeMember(ZWarpMembers, WarpedInZone);
		ScopedTypeMember(ZWarpMembers, Last);
		ScopedTypeMember(ZWarpMembers, Return);
	}

	bool GetMember(MQVarPtr VarPtr, const char* Member, char* Index, MQTypeVar& Dest) override
	{
		MQTypeMember* pMember = MQ2ZWarpType::FindMember(Member);
		if (!pMember)
			return false;

		switch (static_cast<ZWarpMembers>(pMember->ID))
		{
		case ZWarpMembers::TimeSinceWarp:
			Dest.UInt64 = s_lastWarpTime ? (GetTickCount64() - s_lastWarpTime) : 0;
			Dest.Type = datatypes::pTimeStampType;
			return true;

		case ZWarpMembers::WarpedInZone:
			Dest.Set(s_warpedInZone);
			Dest.Type = datatypes::pBoolType;
			return true;

		case ZWarpMembers::Last:
			if (!s_lastDestination.Valid)
				return false;
			sprintf_s(DataTypeTemp, "%.2f %.2f %.2f", s_lastDestination.Y, s_lastDestination.X, s_lastDestination.Z);
			Dest.Ptr = &DataTypeTemp[0];
			Dest.Type = datatypes::pStringType;
			return true;

		case ZWarpMembers::Return:
			if (!s_priorPosition.Valid)
				return false;
			sprintf_s(DataTypeTemp, "%.2f %.2f %.2f", s_priorPosition.Y, s_priorPosition.X, s_priorPosition.Z);
			Dest.Ptr = &DataTypeTemp[0];
			Dest.Type = datatypes::pStringType;
			return true;
		}

		return false;
	}

	bool ToString(MQVarPtr VarPtr, char* Destination) override
	{
		strcpy_s(Destination, MAX_STRING, s_warpedInZone ? "TRUE" : "FALSE");
		return true;
	}
};

class MQ2WaypointType : public MQ2Type
{
public:
	enum class WaypointMembers
	{
		Name,
		Exists,
		Loc,
		Y,
		YCoord,
		X,
		XCoord,
		Z,
		ZCoord,
		Heading,
		Zone,
	};

	MQ2WaypointType() : MQ2Type("Waypoint")
	{
		ScopedTypeMember(WaypointMembers, Name);
		ScopedTypeMember(WaypointMembers, Exists);
		ScopedTypeMember(WaypointMembers, Loc);
		ScopedTypeMember(WaypointMembers, Y);
		ScopedTypeMember(WaypointMembers, YCoord);
		ScopedTypeMember(WaypointMembers, X);
		ScopedTypeMember(WaypointMembers, XCoord);
		ScopedTypeMember(WaypointMembers, Z);
		ScopedTypeMember(WaypointMembers, ZCoord);
		ScopedTypeMember(WaypointMembers, Heading);
		ScopedTypeMember(WaypointMembers, Zone);
	}

	bool GetMember(MQVarPtr VarPtr, const char* Member, char* Index, MQTypeVar& Dest) override
	{
		auto* pWaypoint = static_cast<Waypoint*>(VarPtr.Ptr);
		if (!pWaypoint)
			return false;

		MQTypeMember* pMember = MQ2WaypointType::FindMember(Member);
		if (!pMember)
			return false;

		switch (static_cast<WaypointMembers>(pMember->ID))
		{
		case WaypointMembers::Name:
			strcpy_s(DataTypeTemp, pWaypoint->Name.c_str());
			Dest.Ptr = &DataTypeTemp[0];
			Dest.Type = datatypes::pStringType;
			return true;

		case WaypointMembers::Exists:
			Dest.Set(pWaypoint->Exists);
			Dest.Type = datatypes::pBoolType;
			return true;

		case WaypointMembers::Zone:
			strcpy_s(DataTypeTemp, pWaypoint->Zone.c_str());
			Dest.Ptr = &DataTypeTemp[0];
			Dest.Type = datatypes::pStringType;
			return true;

		case WaypointMembers::Loc:
			if (!pWaypoint->Exists)
				return false;
			sprintf_s(DataTypeTemp, "%.2f %.2f %.2f", pWaypoint->Y, pWaypoint->X, pWaypoint->Z);
			Dest.Ptr = &DataTypeTemp[0];
			Dest.Type = datatypes::pStringType;
			return true;

		case WaypointMembers::Y:
		case WaypointMembers::YCoord:
			if (!pWaypoint->Exists)
				return false;
			Dest.Float = pWaypoint->Y;
			Dest.Type = datatypes::pFloatType;
			return true;

		case WaypointMembers::X:
		case WaypointMembers::XCoord:
			if (!pWaypoint->Exists)
				return false;
			Dest.Float = pWaypoint->X;
			Dest.Type = datatypes::pFloatType;
			return true;

		case WaypointMembers::Z:
		case WaypointMembers::ZCoord:
			if (!pWaypoint->Exists)
				return false;
			Dest.Float = pWaypoint->Z;
			Dest.Type = datatypes::pFloatType;
			return true;

		case WaypointMembers::Heading:
			if (!pWaypoint->Exists)
				return false;
			Dest.Float = pWaypoint->Heading;
			Dest.Type = datatypes::pFloatType;
			return true;
		}

		return false;
	}

	bool ToString(MQVarPtr VarPtr, char* Destination) override
	{
		auto* pWaypoint = static_cast<Waypoint*>(VarPtr.Ptr);
		if (!pWaypoint)
			return false;

		strcpy_s(Destination, MAX_STRING, pWaypoint->Name.c_str());
		return true;
	}
};

static MQ2ZWarpType*    pZWarpType = nullptr;
static MQ2WaypointType* pWaypointType = nullptr;

// Backing store for ${Waypoint[name]}. The TLO hands out a pointer to this, so it
// only stays valid until the next lookup - which is all a single member access needs.
static Waypoint s_waypointResult;

static bool dataZWarp(const char*, MQTypeVar& ret)
{
	ret.DWord = 1;
	ret.Type = pZWarpType;
	return true;
}

static bool dataWaypoint(const char* szIndex, MQTypeVar& ret)
{
	if (!szIndex || szIndex[0] == 0)
		return false;

	s_waypointResult = LoadWaypoint(szIndex);
	ret.Ptr = &s_waypointResult;
	ret.Type = pWaypointType;
	return true;
}

//============================================================================
// /zwarp
//============================================================================

static void ShowWarpHelp()
{
	WriteChatf("%sUsage:", PluginMsg);
	WriteChatf("\ao/zwarp <dist>\ax                         \at- Warp forward <dist> units.");
	WriteChatf("\ao/zwarp (s)uccor\ax                       \at- Move to the zone's safe point.");
	WriteChatf("\ao/zwarp last\ax                           \at- Move to the last warp destination.");
	WriteChatf("\ao/zwarp return\ax                         \at- Return to the location prior to the last warp.");
	WriteChatf("\ao/zwarp loc <y> <x> [z]\ax                \at- Warp to a specific location.");
	WriteChatf("\ao/zwarp dir <dist>\ax                     \at- Warp forward a distance.");
	WriteChatf("\ao/zwarp id <n> [<n|s|e|w> <dist>]\ax      \at- Warp to a spawn id, or that far north/south/east/west of it.");
	WriteChatf("\ao/zwarp (t)arget [<n|s|e|w> <dist>]\ax    \at- Warp to your target, or that far north/south/east/west of it.");
	WriteChatf("\ao/zwarp (rt)arget [maxdist]\ax            \at- Warp to your target, random direction, random 2-10 (or 2-maxdist) units out.");
	WriteChatf("\ao/zwarp (i)tem [<n|s|e|w> <dist>]\ax      \at- Warp to the current ground item, or that far from it.");
	WriteChatf("\ao/zwarp wp <name>\ax                      \at- Warp to a named waypoint.");
	WriteChatf("\ao/zwarp compass\ax                        \at- Warp to the location the main compass line points at.");
	WriteChatf("\ao/zwarp (b)ehind [dist]\ax                \at- Warp behind your target.");
	WriteChatf("\ao/zwarp (f)ront [dist]\ax                 \at- Warp in front of your target.");
	WriteChatf("\ao/zwarp (l)eft [dist]\ax                  \at- Warp to the left of your target.");
	WriteChatf("\ao/zwarp (r)ight [dist]\ax                 \at- Warp to the right of your target.");
}

/**
 * @fn WarpAroundTarget
 *
 * behind/front/left/right. The offset is taken along the target's own heading,
 * rotated by `headingOffset` quarter-turns worth of heading units.
 */
static bool WarpAroundTarget(float headingOffset, const char* distArg, const char* what)
{
	PlayerClient* pTargetSpawn = pTarget;
	if (!pTargetSpawn)
	{
		WriteChatf("%s\arYou must have a target for /zwarp %s.", PluginMsg, what);
		return false;
	}

	const float dist = GetFloatFromString(distArg, 5.0f);

	float y = pTargetSpawn->Y;
	float x = pTargetSpawn->X;

	OffsetByHeading(y, x, pTargetSpawn->Heading + headingOffset, dist);

	return WarpToKeepHeading(y, x, pTargetSpawn->FloorHeight);
}

/**
 * @fn Command_ZWarp
 */
static void Command_ZWarp(PlayerClient* pChar, const char* szLine)
{
	char arg1[MAX_STRING] = { 0 };
	char arg2[MAX_STRING] = { 0 };
	char arg3[MAX_STRING] = { 0 };
	char arg4[MAX_STRING] = { 0 };

	GetArg(arg1, szLine, 1);
	GetArg(arg2, szLine, 2);
	GetArg(arg3, szLine, 3);
	GetArg(arg4, szLine, 4);

	if (arg1[0] == 0 || ci_equals(arg1, "help"))
	{
		ShowWarpHelp();
		return;
	}

	if (!CanWarp())
		return;

	// /zwarp <dist> - bare number means "forward this far".
	if (arg1[0] == '-' || arg1[0] == '.' || (arg1[0] >= '0' && arg1[0] <= '9'))
	{
		const float dist = GetFloatFromString(arg1, 0.0f);

		float y = pLocalPlayer->Y;
		float x = pLocalPlayer->X;
		OffsetByHeading(y, x, pLocalPlayer->Heading, dist);

		WarpToKeepHeading(y, x, pLocalPlayer->Z);
		return;
	}

	if (ci_equals(arg1, "succor") || ci_equals(arg1, "s"))
	{
		WarpTo(pZoneInfo->SafeYLoc, pZoneInfo->SafeXLoc, pZoneInfo->SafeZLoc, pZoneInfo->SafeHeading);
		WriteChatf("%s\agMoved to the zone's safe point.", PluginMsg);
		return;
	}

	if (ci_equals(arg1, "last"))
	{
		if (!s_lastDestination.Valid)
		{
			WriteChatf("%s\ayYou must have warped before to use this command.", PluginMsg);
			return;
		}

		const WarpLocation dest = s_lastDestination;
		WarpTo(dest.Y, dest.X, dest.Z, dest.Heading);
		return;
	}

	if (ci_equals(arg1, "return"))
	{
		if (!s_priorPosition.Valid)
		{
			WriteChatf("%s\ayYou must have warped first to use this command.", PluginMsg);
			return;
		}

		const WarpLocation dest = s_priorPosition;
		WarpTo(dest.Y, dest.X, dest.Z, dest.Heading);
		WriteChatf("%s\agReturning to position prior to last warp.", PluginMsg);
		return;
	}

	if (ci_equals(arg1, "loc"))
	{
		if (arg2[0] == 0 || arg3[0] == 0)
		{
			WriteChatf("%s\ayYou must provide <y> <x> [z] if going to a location.", PluginMsg);
			return;
		}

		const float y = GetFloatFromString(arg2, 0.0f);
		const float x = GetFloatFromString(arg3, 0.0f);
		const float z = arg4[0] ? GetFloatFromString(arg4, 0.0f) : pLocalPlayer->Z;

		WarpToKeepHeading(y, x, z);
		return;
	}

	if (ci_equals(arg1, "dir"))
	{
		if (arg2[0] == 0)
		{
			WriteChatf("%s\ayYou MUST provide <dist> if going in your current direction.", PluginMsg);
			return;
		}

		const float dist = GetFloatFromString(arg2, 0.0f);

		float y = pLocalPlayer->Y;
		float x = pLocalPlayer->X;
		OffsetByHeading(y, x, pLocalPlayer->Heading, dist);

		WarpToKeepHeading(y, x, pLocalPlayer->Z);
		return;
	}

	if (ci_equals(arg1, "id"))
	{
		const int spawnId = GetIntFromString(arg2, 0);

		PlayerClient* pSpawn = pSpawnManager ? pSpawnManager->GetSpawnByID(spawnId) : nullptr;
		if (!pSpawn)
		{
			WriteChatf("%s\ayCould not find spawn matching id %d.", PluginMsg, spawnId);
			return;
		}

		WarpRelativeToSpawn(pSpawn, arg3, arg4);
		return;
	}

	if (ci_equals(arg1, "target") || ci_equals(arg1, "t"))
	{
		if (!pTarget)
		{
			WriteChatf("%s\ayYou must have a target for warping to a target.", PluginMsg);
			return;
		}

		WarpRelativeToSpawn(pTarget, arg2, arg3);
		return;
	}

	if (ci_equals(arg1, "rtarget") || ci_equals(arg1, "rtar"))
	{
		if (!pTarget)
		{
			WriteChatf("%s\ayYou must have a target for warping to a target.", PluginMsg);
			return;
		}

		const float maxDist = arg2[0] ? GetFloatFromString(arg2, 10.0f) : 10.0f;

		std::uniform_real_distribution<float> distRoll(2.0f, std::max(2.0f, maxDist));
		std::uniform_real_distribution<float> headingRoll(0.0f, 512.0f);

		float y = pTarget->Y;
		float x = pTarget->X;
		OffsetByHeading(y, x, headingRoll(s_rng), distRoll(s_rng));

		WarpToKeepHeading(y, x, pTarget->FloorHeight);
		return;
	}

	if (ci_equals(arg1, "item") || ci_equals(arg1, "i"))
	{
		if (!HasCurrentGroundSpawn())
		{
			WriteChatf("%s\ayPlease use /itemtarget to acquire a target in order to warp to an item.", PluginMsg);
			return;
		}

		const CVector3 pos = CurrentGroundSpawn().Position();

		float y = pos.Y;
		float x = pos.X;

		if (arg2[0] != 0)
		{
			const float dist = GetFloatFromString(arg3, 0.0f);

			if (!OffsetByCardinal(arg2, dist, y, x))
			{
				WriteChatf("%s\arUnknown direction '\ax%s\ar'. Use n, s, e or w.", PluginMsg, arg2);
				return;
			}
		}

		WarpToKeepHeading(y, x, pos.Z);
		return;
	}

	if (ci_equals(arg1, "wp"))
	{
		if (arg2[0] == 0)
		{
			WriteChatf("%s\ayYou didn't specify a waypoint.", PluginMsg);
			return;
		}

		const Waypoint wp = LoadWaypoint(arg2);
		if (!wp.Exists)
		{
			WriteChatf("%s\ayWaypoint '\ax%s\ay' does not exist.", PluginMsg, arg2);
			return;
		}

		if (!wp.Zone.empty() && !ci_equals(wp.Zone, CurrentZoneShortName()))
			WriteChatf("%s\ayWaypoint '\ax%s\ay' was saved in \ax%s\ay, not this zone.", PluginMsg, arg2, wp.Zone.c_str());

		WarpTo(wp.Y, wp.X, wp.Z, wp.Heading);
		return;
	}

	if (ci_equals(arg1, "compass"))
	{
		CompassLineSource* pLine = nullptr;

		if (pCompassWnd)
		{
			for (int i = 0; i < pCompassWnd->lineData.GetLength(); ++i)
			{
				CompassLineSource* pCandidate = pCompassWnd->lineData[i];
				if (pCandidate && pCandidate->ShowLine)
				{
					pLine = pCandidate;
					break;
				}
			}
		}

		if (!pLine)
		{
			WriteChatf("%s\ayNo compass point to warp to.", PluginMsg);
			return;
		}

		WriteChatf("%s\agMoving to compass point %.2f %.2f %.2f", PluginMsg, pLine->Y, pLine->X, pLine->Z);
		WarpToKeepHeading(pLine->Y, pLine->X, pLine->Z);
		return;
	}

	// Target-relative directions. Heading units: 128 == a quarter turn counter-clockwise.
	if (ci_equals(arg1, "behind") || ci_equals(arg1, "b"))
	{
		WarpAroundTarget(256.0f, arg2, "(b)ehind");
		return;
	}

	if (ci_equals(arg1, "front") || ci_equals(arg1, "f"))
	{
		WarpAroundTarget(0.0f, arg2, "(f)ront");
		return;
	}

	if (ci_equals(arg1, "left") || ci_equals(arg1, "l"))
	{
		WarpAroundTarget(128.0f, arg2, "(l)eft");
		return;
	}

	if (ci_equals(arg1, "right") || ci_equals(arg1, "r"))
	{
		WarpAroundTarget(-128.0f, arg2, "(r)ight");
		return;
	}

	WriteChatf("%s\arUnknown command: \ax%s", PluginMsg, arg1);
	ShowWarpHelp();
}

//============================================================================
// /waypoint
//============================================================================

static void ShowWaypointHelp()
{
	WriteChatf("%s\ayInvalid syntax. Usage:", PluginMsg);
	WriteChatf("\ag/waypoint add <name>\ax    \at- Save the current location.");
	WriteChatf("\ag/waypoint update <name>\ax \at- Move an existing waypoint to the current location.");
	WriteChatf("\ag/waypoint delete <name>\ax \at- Remove a waypoint.");
	WriteChatf("\ag/waypoint list\ax          \at- List saved waypoints.");
}

static void Command_Waypoint(PlayerClient* pChar, const char* szLine)
{
	char arg1[MAX_STRING] = { 0 };
	char arg2[MAX_STRING] = { 0 };

	GetArg(arg1, szLine, 1);
	GetArg(arg2, szLine, 2);

	if (arg1[0] == 0 || ci_equals(arg1, "help"))
	{
		ShowWaypointHelp();
		return;
	}

	if (ci_equals(arg1, "list"))
	{
		const std::vector<std::string> names = GetPrivateProfileKeys(kWaypointSection, INIFileName);

		WriteChatf("%sWaypoints for \ag%s\ax:", PluginMsg, pLocalPC ? pLocalPC->Name : "unknown");

		if (names.empty())
		{
			WriteChatf("  \ay(none)");
			return;
		}

		for (const std::string& name : names)
		{
			const Waypoint wp = LoadWaypoint(name);
			if (wp.Zone.empty())
				WriteChatf("- \ag%s", name.c_str());
			else
				WriteChatf("- \ag%s\ax ( \at%s\ax )", name.c_str(), wp.Zone.c_str());
		}

		return;
	}

	if (ci_equals(arg1, "add") || ci_equals(arg1, "update"))
	{
		if (arg2[0] == 0)
		{
			WriteChatf("%s\ayYou didn't specify a name for the waypoint.", PluginMsg);
			return;
		}

		if (GetGameState() != GAMESTATE_INGAME || !pLocalPlayer)
		{
			WriteChatf("%s\arYou must be in game to save a waypoint.", PluginMsg);
			return;
		}

		const bool exists = LoadWaypoint(arg2).Exists;

		if (ci_equals(arg1, "add") && exists)
		{
			WriteChatf("%s\ayWaypoint '\ax%s\ay' already exists. To update it use: /waypoint update %s", PluginMsg, arg2, arg2);
			return;
		}

		SaveWaypoint(arg2);

		if (exists)
			WriteChatf("%s\agWaypoint '\ax%s\ag' updated to current location.", PluginMsg, arg2);
		else
			WriteChatf("%s\agWaypoint '\ax%s\ag' added.", PluginMsg, arg2);

		return;
	}

	if (ci_equals(arg1, "delete"))
	{
		if (arg2[0] == 0)
		{
			WriteChatf("%s\ayYou didn't specify a waypoint to delete.", PluginMsg);
			return;
		}

		if (!LoadWaypoint(arg2).Exists)
		{
			WriteChatf("%s\ayWaypoint '\ax%s\ay' does not exist.", PluginMsg, arg2);
			return;
		}

		// Passing a null value removes the key.
		::WritePrivateProfileStringA(kWaypointSection, arg2, nullptr, INIFileName);
		WriteChatf("%s\agWaypoint '\ax%s\ag' deleted.", PluginMsg, arg2);
		return;
	}

	ShowWaypointHelp();
}

//============================================================================
// /exactloc and /setgrav
//============================================================================

static void Command_ExactLoc(PlayerClient* pChar, const char* szLine)
{
	if (GetGameState() != GAMESTATE_INGAME || !pLocalPlayer)
	{
		WriteChatf("%s\arYou must be in game.", PluginMsg);
		return;
	}

	WriteChatf("%s\atYour location is \ay%3.6f \ay%3.6f \ay%3.6f", PluginMsg,
		pLocalPlayer->Y, pLocalPlayer->X, pLocalPlayer->Z);
}

static void Command_SetGrav(PlayerClient* pChar, const char* szLine)
{
	char arg1[MAX_STRING] = { 0 };
	GetArg(arg1, szLine, 1);

	if (GetGameState() != GAMESTATE_INGAME || !pZoneInfo)
	{
		WriteChatf("%s\arYou must be in game.", PluginMsg);
		return;
	}

	if (arg1[0] == 0)
	{
		WriteChatf("%sZone gravity is \ay%4.2f\ax (zone default \ay%4.2f\ax). Usage: /setgrav <value|default>",
			PluginMsg, pZoneInfo->ZoneGravity, s_defaultGravity);
		return;
	}

	const float previous = pZoneInfo->ZoneGravity;
	const float gravity = ci_equals(arg1, "default") ? s_defaultGravity : GetFloatFromString(arg1, previous);

	pZoneInfo->ZoneGravity = gravity;

	WriteChatf("%s\atZone gravity now set from \ay%4.2f \atto \ay%4.2f", PluginMsg, previous, gravity);
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
	DebugSpewAlways("MQ2ZWarp::Initializing version %f", MQ2Version);

	UpdateIniFileName();

	AddCommand("/zwarp", Command_ZWarp, false, true, true);
	AddCommand("/warp", Command_ZWarp, false, true, true);
	AddCommand("/waypoint", Command_Waypoint, false, true, true);
	AddCommand("/exactloc", Command_ExactLoc, false, true, true);
	AddCommand("/setgrav", Command_SetGrav, false, true, true);

	pZWarpType = new MQ2ZWarpType;
	AddMQ2Data("ZWarp", dataZWarp);

	pWaypointType = new MQ2WaypointType;
	AddMQ2Data("Waypoint", dataWaypoint);
}

/**
 * @fn ShutdownPlugin
 *
 * This is called once when the plugin has been asked to shutdown. The plugin has
 * not actually shut down until this completes.
 */
PLUGIN_API void ShutdownPlugin()
{
	DebugSpewAlways("MQ2ZWarp::Shutting down");

	RemoveCommand("/zwarp");
	RemoveCommand("/warp");
	RemoveCommand("/waypoint");
	RemoveCommand("/exactloc");
	RemoveCommand("/setgrav");

	RemoveMQ2Data("ZWarp");
	delete pZWarpType;
	pZWarpType = nullptr;

	RemoveMQ2Data("Waypoint");
	delete pWaypointType;
	pWaypointType = nullptr;
}

/**
 * @fn SetGameState
 *
 * This is called when the GameState changes. It is also called once after the
 * plugin is initialized.
 *
 * @param GameState int - The value of GameState at the time of the call
 */
PLUGIN_API void SetGameState(int GameState)
{
	if (GameState == GAMESTATE_INGAME)
	{
		UpdateIniFileName();

		if (pZoneInfo)
		{
			s_defaultGravity = pZoneInfo->ZoneGravity;
			s_haveDefaultGravity = true;
		}
	}
	else
	{
		s_haveDefaultGravity = false;
	}
}

/**
 * @fn OnZoned
 *
 * This is called each time a zone change occurs, after the zone is complete.
 */
PLUGIN_API void OnZoned()
{
	// Last/return destinations are zone-local, and the new zone brings its own
	// gravity and safe point with it.
	s_lastDestination.Valid = false;
	s_priorPosition.Valid = false;
	s_warpedInZone = false;

	if (pZoneInfo)
	{
		s_defaultGravity = pZoneInfo->ZoneGravity;
		s_haveDefaultGravity = true;
	}
}
