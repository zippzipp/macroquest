/*
 * MacroQuest: The extension platform for EverQuest
 * Copyright (C) 2002-present MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "pch.h"
#include "MacroQuest.h"
#include "MQ2Main.h"
#include "Logging.h"
#include "mq/base/WString.h"

#include <TlHelp32.h>

namespace mq {

// this is the memory checker key struct
struct mckey
{
	union
	{
		int x;
		unsigned char a[4];
		char sa[4];
	};
};

// pointer to encryption pad for memory checker
unsigned int* extern_array0 = nullptr;

static bool s_doingSpellChecks = false;
static int s_inMemCheck4 = 0;

//============================================================================

class SpellManager_Detours
{
public:
#if !IS_EXPANSION_LEVEL(EXPANSION_LEVEL_COTF)
	DETOUR_TRAMPOLINE_DEF(bool, LoadTextSpells_Trampoline, (char*, char*, EQ_Spell*))
	bool LoadTextSpells_Detour(char* FileName, char* AssocFileName, EQ_Spell* SpellArray)
	{
		s_doingSpellChecks = true;
		bool ret = LoadTextSpells_Trampoline(FileName, AssocFileName, SpellArray);
		s_doingSpellChecks = false;
		return ret;
	}
#else
	DETOUR_TRAMPOLINE_DEF(bool, LoadTextSpells_Trampoline, (char*, char*, EQ_Spell*, SpellAffectData*))
	bool LoadTextSpells_Detour(char* FileName, char* AssocFileName, EQ_Spell* SpellArray, SpellAffectData* EffectArray)
	{
		s_doingSpellChecks = true;
		bool ret = LoadTextSpells_Trampoline(FileName, AssocFileName, SpellArray, EffectArray);
		s_doingSpellChecks = false;
		return ret;
	}
#endif
};

//============================================================================

#if 0
// TODO: Maybe someday revisit detection of assist completion...
//void SetAssist(BYTE* address)
//{
//	gbAssistComplete = AS_AssistReceived;
//
//	if (!address) return;
//	int Assistee = *(int*)address;
//
//	if (SPAWNINFO* pSpawn = GetSpawnByID(Assistee))
//	{
//		//DebugSpew("Assist Result: %d => %s", Assistee, pSpawn->Name);
//		gbAssistComplete = AS_AssistSent;
//	}
//}

// Defined in AssemblyFunctions.asm, need the forward declare
//void GetAssistParam();

//============================================================================

class CPacketScrambler_Detours
{
public:
	int ntoh_Detour(int nopcode);
	DETOUR_TRAMPOLINE_DEF(int, ntoh_Trampoline, (int))
};

// ntoh_detour actually climbs into the stack and pulls data out from the caller's
// stack frame. Because of this we need to avoid optimizing this function as it
// changes the layout of the stack. Keep optimizations off for this function or
// it will break.

#pragma optimize("", off)
int CPacketScrambler_Detours::ntoh_Detour(int nopcode)
{
	int hopcode = ntoh_Trampoline(nopcode);

#if 0
	if (hopcode == EQ_ASSIST)
	{
		GetAssistParam();
	}
#endif

	return hopcode;
}
#pragma optimize("", on)
#endif

enum class AddressDetourState
{
	None = 0,
	CodeDetour = 1,
	KnownSkippable = 2,
};

static AddressDetourState IsAddressDetoured(uintptr_t address, size_t width)
{
	if (s_doingSpellChecks || s_inMemCheck4 > 0)
		return AddressDetourState::KnownSkippable;

	// Executables start with a header that has 'MZ\x90\x00'. This is checking for this
	// magic value and skipping the crc32 of an executable since we don't care about those.
	if (address && width >= 4 && *(DWORD*)address == 0x00905a4d)
		return AddressDetourState::KnownSkippable;

	if (g_mq->IsAddressPatched(address, width))
		return AddressDetourState::CodeDetour;

	return AddressDetourState::None;
}

DETOUR_TRAMPOLINE_DEF(int, memcheck0_tramp, (unsigned char* buffer, size_t count))
int memcheck0(unsigned char* buffer, size_t count);
DETOUR_TRAMPOLINE_DEF(int, memcheck1_tramp, (unsigned char* buffer, size_t count, mckey key))
int memcheck1(unsigned char* buffer, size_t count, mckey key);
#if defined(__MemChecker4_x)
DETOUR_TRAMPOLINE_DEF(int WINAPI, memcheck4_tramp, (unsigned char* buffer, size_t* count))
int WINAPI memcheck4(unsigned char* buffer, size_t* count);
#endif

struct OrderedPatchSet
{
	OrderedPatchSet(std::vector<eqlib::MemoryPatch*>&& patches)
		: patches(std::move(patches))
	{
		if (!patches.empty())
		{
			lastAddress = patches.back()->GetAddress() + patches.back()->GetBytesSize();
		}
	}

	// Optimized based on the assumption that patches are sorted and addresses are accessed in increasing order
	uint8_t GetPatchedByte(uintptr_t address, uint8_t originalByte)
	{
		// Early out if there are no patches
		if (patches.empty())
			return originalByte;

		// Early out if address is out of range
		if (lastPatchIndex >= patches.size() || address < patches[lastPatchIndex]->GetAddress() || address >= lastAddress)
			return originalByte;

		// If this fails, the address has gone past the patch.
		if (!patches[lastPatchIndex]->IsAddressInRange(address))
		{
			++lastPatchIndex;

			// We've exhausted all of our patches.
			if (lastPatchIndex >= patches.size())
				return originalByte;

			// We haven't caught up to the next one yet.
			if (address < patches[lastPatchIndex]->GetAddress())
				return originalByte;
		}

		// At this point we are expecting a patch covering the current range. (It isn't possible to go from
		// one patch to another without going through the address range of the next patch)
		return patches[lastPatchIndex]->ReadOriginalByte(address);
	}

private:
	size_t lastPatchIndex = 0;
	uintptr_t lastAddress = 0;
	std::vector<eqlib::MemoryPatch*> patches;
};

static uint32_t DetourAwareHash(uint8_t* origBytes, size_t count, uint32_t value = 0xffffffff)
{
	uintptr_t addr = reinterpret_cast<uintptr_t>(origBytes);
	OrderedPatchSet patches{ g_mq->FindPatches(addr, count) };

	for (size_t i = 0; i < count; ++i)
	{
		// Feed in bytes to the hash algorithm using the source bytes of a detour
		// if the data range overlaps an active detour.
		uint8_t newByte = patches.GetPatchedByte(addr + i, origBytes[i]);

		int temp = static_cast<int>(newByte) ^ (value & 0xff);
		value = (static_cast<int>(value) >> 8) & 0xffffff;

		value ^= extern_array0[temp];
	}

	return value;
}

int memcheck0(unsigned char* buffer, size_t count)
{
	uintptr_t addr = reinterpret_cast<uintptr_t>(buffer);

	// If we are not detouring memory that overlaps this region, just let it pass through.
	AddressDetourState detourState = IsAddressDetoured(addr, count);
	if (detourState != AddressDetourState::CodeDetour)
	{
		return memcheck0_tramp(buffer, count);
	}

	return DetourAwareHash(buffer, count);
}

int memcheck1(unsigned char* buffer, size_t count, mckey key)
{
	unsigned int eax, edx;

	if (key.x != 0) {
		eax = ~key.a[0] & 0xff;
		eax = extern_array0[eax];
		eax ^= 0xffffff;

		edx = key.a[1];
		edx = (edx ^ eax) & 0xff;
		eax = ((int)eax >> 8) & 0xffffff;
		eax ^= extern_array0[edx];

		edx = key.a[2];
		edx = (edx ^ eax) & 0xff;
		eax = ((int)eax >> 8) & 0xffffff;
		eax ^= extern_array0[edx];

		edx = key.a[3];
		edx = (edx ^ eax) & 0xff;
		eax = ((int)eax >> 8) & 0xffffff;
		eax ^= extern_array0[edx];
	} else {
		eax = 0xffffffff;
	}

	return ~DetourAwareHash(buffer, count, eax);
}

#if defined(__MemChecker4_x)
int WINAPI memcheck4(unsigned char* buffer, size_t* count_)
{
	uintptr_t addr = reinterpret_cast<uintptr_t>(buffer);
	size_t count = *count_ & 0xff;
	uint8_t bmask = *gpMemCheckBitmask;

	if (!bmask && *gpMemCheckActive)
		*gpMemCheckBitmask |= 1;

	// If we are not detouring memory that overlaps this region, just let it pass through.
	AddressDetourState detourState = IsAddressDetoured(addr, count);
	if (detourState != AddressDetourState::CodeDetour)
	{
		s_inMemCheck4 = 1;
		int result = memcheck4_tramp(buffer, count_);
		s_inMemCheck4 = 0;
		return result;
	}

	unsigned int crc32 = DetourAwareHash(buffer, count, 0xffffffff);

	*gpMemCheckBitmask = bmask;

	return crc32;
}
#endif // defined(__MemChecker4_x)

DETOUR_TRAMPOLINE_DEF(uint64_t, decompress_block_trampoline, (uint64_t ctx))
uint64_t decompress_block_detour(uint64_t ctx)
{
	if (s_inMemCheck4)
		return decompress_block_trampoline(ctx);

	return 0;
}

void MQInitializeLogin();

DETOUR_TRAMPOLINE_DEF(BOOL WINAPI, FindModules_Trampoline, (HANDLE, HMODULE*, DWORD, DWORD*))
BOOL WINAPI FindModules_Detour(HANDLE hProcess, HMODULE* hModule, DWORD cb, DWORD* lpcbNeeded)
{
	if (s_inMemCheck4 != 1) return FindModules_Trampoline(hProcess, hModule, cb, lpcbNeeded);
	++s_inMemCheck4;
	bool getMacroQuestModules = true;
	bool result = GetFilteredModules(hProcess, hModule, cb, lpcbNeeded,
		[&getMacroQuestModules](HMODULE hModule) -> bool { return IsMacroQuestModule(hModule, getMacroQuestModules); }) ? TRUE : FALSE;
	--s_inMemCheck4;
	return result ? 1 : 0;
}

DETOUR_TRAMPOLINE_DEF(BOOL WINAPI, FindProcesses_Trampoline, (DWORD*, DWORD, DWORD*))
BOOL WINAPI FindProcesses_Detour(DWORD* lpidProcess, DWORD cb, DWORD* lpcbNeeded)
{
	if (s_inMemCheck4 != 1) return FindProcesses_Trampoline(lpidProcess, cb, lpcbNeeded);
	++s_inMemCheck4;
	bool getMacroQuestProcesses = true;
	bool result = GetFilteredProcesses(lpidProcess, cb, lpcbNeeded,
		[&getMacroQuestProcesses](std::string_view process_name) -> bool { return IsMacroQuestProcess(process_name, getMacroQuestProcesses); }) ? TRUE : FALSE;
	--s_inMemCheck4;
	return result ? 1 : 0;
}

DETOUR_TRAMPOLINE_DEF(BOOL WINAPI, Module32Next_Trampoline, (HANDLE, LPMODULEENTRY32))
static BOOL FilterModuleEntry(HANDLE hSnapshot, LPMODULEENTRY32 lpme)
{
	while (IsMacroQuestModule(lpme->hModule, true))
	{
		if (!Module32Next_Trampoline(hSnapshot, lpme))
			return FALSE;
	}

	return TRUE;
}

BOOL WINAPI Module32Next_Detour(HANDLE hSnapshot, LPMODULEENTRY32 lpme)
{
	if (Module32Next_Trampoline(hSnapshot, lpme))
	{
		return FilterModuleEntry(hSnapshot, lpme);
	}

	return FALSE;
}

DETOUR_TRAMPOLINE_DEF(BOOL WINAPI, Module32First_Trampoline, (HANDLE, LPMODULEENTRY32))
BOOL WINAPI Module32First_Detour(HANDLE hSnapshot, LPMODULEENTRY32 lpme)
{
	if (Module32First_Trampoline(hSnapshot, lpme))
	{
		return FilterModuleEntry(hSnapshot, lpme);
	}

	return FALSE;
}

DETOUR_TRAMPOLINE_DEF(BOOL WINAPI, Process32Next_Trampoline, (HANDLE, LPPROCESSENTRY32))
static BOOL FilterProcessEntry(HANDLE hSnapshot, LPPROCESSENTRY32 lppe)
{
	while (IsMacroQuestProcess(lppe->th32ProcessID, true)
		|| IsMacroQuestProcess(lppe->th32ParentProcessID, true))
	{
		if (!Process32Next_Trampoline(hSnapshot, lppe))
			return FALSE;
	}

	return TRUE;
}

BOOL WINAPI Process32Next_Detour(HANDLE hSnapshot, LPPROCESSENTRY32 lppe)
{
	if (Process32Next_Trampoline(hSnapshot, lppe))
	{
		return FilterProcessEntry(hSnapshot, lppe);
	}

	return FALSE;
}

DETOUR_TRAMPOLINE_DEF(BOOL WINAPI, Process32First_Trampoline, (HANDLE, LPPROCESSENTRY32))
BOOL WINAPI Process32First_Detour(HANDLE hSnapshot, LPPROCESSENTRY32 lppe)
{
	if (Process32First_Trampoline(hSnapshot, lppe))
	{
		return FilterProcessEntry(hSnapshot, lppe);
	}

	return FALSE;
}

uintptr_t __Module32First = 0;
uintptr_t __Module32Next = 0;
uintptr_t __Process32First = 0;
uintptr_t __Process32Next = 0;

struct HookInfo
{
	std::string name;
	uintptr_t address = 0;

	std::function<void(HookInfo&)> patch = nullptr;
};
static std::vector<HookInfo> s_hooks;
static std::vector<uintptr_t> s_patches;


template <typename T>
void AddHook_(uintptr_t address, T& detour, T*& target, const char* name)
{
	HookInfo hookInfo;
	hookInfo.name = name;
	hookInfo.address = 0;
	hookInfo.patch = [address, &detour, &target](HookInfo& hi)
		{
			hi.address = address;
			mq::detail::CreateDetour(hi.address, &(void*&)target, detour, hi.name);
		};
	
	s_hooks.push_back(std::move(hookInfo));
}

#define AddHook(address, detour, trampoline) \
	AddHook_(static_cast<uintptr_t>(address), detour, trampoline##_Ptr, STRINGIFY(address))

static void InstallHooks()
{
	for (HookInfo& hook : s_hooks)
	{
		if (hook.address == 0)
		{
			hook.patch(hook);
		}
	}
}

static void RemoveHooks()
{
	for (HookInfo& hook : s_hooks)
	{
		if (hook.address != 0)
		{
			mq::RemoveDetour(hook.address);
			hook.address = 0;
		}
	}
	s_hooks.clear();

	for (uintptr_t patchAddr : s_patches)
	{
		mq::RemovePatch(patchAddr);
	}
	s_patches.clear();
}

static bool IsHooked(uintptr_t addr)
{
	for (const HookInfo& hook : s_hooks)
	{
		if (hook.address == addr)
		{
			return true;
		}
	}
	return false;
}

//============================================================================
// Hook-scanner countermeasure
//
// CEverQuest::DoMainLoop contains an unrolled sequence of "check units". Each one
// loads the first byte of a monitored function, tests it against a list of detour
// shapes, and -- only if the verdict differs from a stored state byte -- reports
// the change to the server.
//
// Canonical unit shape (2026-09-11 client):
//
//     lea   rdx, [rip+disp]          ; or mov reg,[rip+disp], or lea+deref for imports
//     movzx eax, byte ptr [rdx]      ; <-- the 3 bytes we replace
//     cmp   al, 0xE9                 ; seven shape tests follow, three out-of-line
//     ...
//     xor   al, al                   ; terminal "clean" verdict
//     cmp   al, byte ptr [rip+disp]  ; <-- the state byte
//     je    next_unit
//
// We replace the movzx with `xor reg,reg; nop`. The loaded byte becomes zero, every
// shape test fails, and control reaches the terminal xor -- a permanent "clean"
// verdict. Flags are safe (the next instruction is the cmp, which overwrites them)
// and the register is not live afterwards.
//
// This anchors on the load/compare pair rather than on the branch layout. The
// previous implementation matched the comparison chain and flipped jne opcodes at
// fixed deltas {5, 13}; the 2026-09-11 client changed the register (rcx -> rdx/rax,
// which also introduced cl-form compares) and grew the shape list from two tests to
// seven, moving every delta. It silently matched zero sites while still logging
// "Patching". The movzx is the one instruction in a unit that cannot move relative
// to its own compare.
//
// Measured selectivity over the whole image, both builds, zero false positives:
//     2026-08-13 client: 24 matches, all inside DoMainLoop
//     2026-09-11 client: 25 matches, all inside DoMainLoop
//============================================================================

struct HookCheckUnit
{
	uintptr_t checkAddress = 0;      // the movzx -- our 3-byte patch site
	uintptr_t targetAddress = 0;     // the monitored function
	uintptr_t stateAddress = 0;      // the client's stored verdict byte
	uint8_t   patchBytes[3] = {};
	uint8_t   originalBytes[3] = {};
	bool      patched = false;
};

static std::vector<HookCheckUnit> s_checkUnits;

static int32_t ReadRel32(uintptr_t address)
{
	int32_t value;
	memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
	return value;
}

// Decode a check unit at `address`. Returns false for anything that is not an exact
// structural match, so this is safe to run over every 0F B6 in the image.
static bool DecodeCheckUnit(uintptr_t address, uintptr_t imageStart, uintptr_t imageEnd, HookCheckUnit& out)
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>(address);

	// movzx r32, byte ptr [reg]. mod must be 00 and rm must be a plain register:
	// rm == 4 would introduce a SIB byte, rm == 5 would be rip-relative.
	const uint8_t modrm = p[2];
	if ((modrm >> 6) != 0)
		return false;

	const uint8_t baseReg = modrm & 7;
	const uint8_t destReg = (modrm >> 3) & 7;
	if (baseReg == 4 || baseReg == 5)
		return false;

	// The compare must follow immediately and must read the register the movzx just
	// wrote. Only the two encodings the client emits are accepted.
	if (p[3] == 0x3C && p[4] == 0xE9)                            // cmp al, 0xE9
	{
		if (destReg != 0)
			return false;
	}
	else if (p[3] == 0x80 && p[4] == 0xF9 && p[5] == 0xE9)       // cmp cl, 0xE9
	{
		if (destReg != 1)
			return false;
	}
	else
	{
		return false;
	}

	// Resolve the monitored function from the load feeding the movzx. Three forms
	// appear; all of them write into baseReg.
	uintptr_t target = 0;
	uint8_t loadReg = 0xFF;

	// Form 3: lea rax, [rip+disp]; mov rax, [rax]   (import, three-step; new in Sep)
	const uint8_t* deref = reinterpret_cast<const uint8_t*>(address - 3);
	if (address >= imageStart + 12
		&& deref[0] == 0x48 && deref[1] == 0x8B
		&& (deref[2] >> 6) == 0 && (deref[2] & 7) == ((deref[2] >> 3) & 7))
	{
		const uint8_t* load = reinterpret_cast<const uint8_t*>(address - 10);
		if (load[0] == 0x48 && load[1] == 0x8D && (load[2] >> 6) == 0 && (load[2] & 7) == 5
			&& ((load[2] >> 3) & 7) == ((deref[2] >> 3) & 7))
		{
			const uintptr_t slot = (address - 10) + 7 + ReadRel32(address - 10 + 3);
			if (slot < imageStart || slot + sizeof(uintptr_t) > imageEnd)
				return false;

			memcpy(&target, reinterpret_cast<const void*>(slot), sizeof(target));
			loadReg = (deref[2] >> 3) & 7;
		}
	}

	// Forms 1 and 2: lea reg, [rip+disp] (internal) or mov reg, [rip+disp] (import).
	if (loadReg == 0xFF && address >= imageStart + 7)
	{
		const uint8_t* load = reinterpret_cast<const uint8_t*>(address - 7);
		if (load[0] == 0x48 && (load[1] == 0x8D || load[1] == 0x8B)
			&& (load[2] >> 6) == 0 && (load[2] & 7) == 5)
		{
			const uintptr_t value = (address - 7) + 7 + ReadRel32(address - 7 + 3);

			if (load[1] == 0x8D)
			{
				target = value;                                  // the function itself
			}
			else
			{
				if (value < imageStart || value + sizeof(uintptr_t) > imageEnd)
					return false;

				memcpy(&target, reinterpret_cast<const void*>(value), sizeof(target));
			}

			loadReg = (load[2] >> 3) & 7;
		}
	}

	if (loadReg != baseReg || target == 0)
		return false;

	// The verdict byte: the first `cmp al, byte ptr [rip+disp]` after the unit body.
	uintptr_t state = 0;
	for (uintptr_t scan = address; scan < address + 128; ++scan)
	{
		const uint8_t* s = reinterpret_cast<const uint8_t*>(scan);
		if (s[0] == 0x3A && s[1] == 0x05)
		{
			state = scan + 6 + ReadRel32(scan + 2);
			break;
		}
	}

	if (state < imageStart || state >= imageEnd)
		return false;

	out.checkAddress = address;
	out.targetAddress = target;
	out.stateAddress = state;
	out.patchBytes[0] = 0x31;                                            // xor r32, r32
	out.patchBytes[1] = static_cast<uint8_t>(0xC0 | (destReg << 3) | destReg);
	out.patchBytes[2] = 0x90;                                            // nop
	memcpy(out.originalBytes, p, sizeof(out.originalBytes));

	return true;
}

static void EnumerateCheckUnits()
{
	s_checkUnits.clear();

	const uintptr_t imageStart = reinterpret_cast<uintptr_t>(::GetModuleHandleA(nullptr));
	const uintptr_t imageEnd = g_eqgameimagesize;

	auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(imageStart);
	auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(imageStart + dosHeader->e_lfanew);
	auto* section = IMAGE_FIRST_SECTION(ntHeaders);

	for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section)
	{
		if ((section->Characteristics & IMAGE_SCN_CNT_CODE) == 0)
			continue;

		const uintptr_t sectionStart = imageStart + section->VirtualAddress;
		const uintptr_t sectionEnd = sectionStart + section->Misc.VirtualSize;

		for (uintptr_t address = sectionStart + 12; address + 8 < sectionEnd; ++address)
		{
			const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
			if (p[0] != 0x0F || p[1] != 0xB6)
				continue;

			HookCheckUnit unit;
			if (DecodeCheckUnit(address, imageStart, imageEnd, unit))
				s_checkUnits.push_back(unit);
		}
	}
}

static bool IsMonitoredTargetHooked(uintptr_t target)
{
	// Core hooks are in s_hooks. Plugin detours -- MQ2Bzsrch on
	// CBazaarSearchWnd::HandleSearchResults is the one that matters -- only ever
	// appear in the memory patcher's registry.
	return IsHooked(target) || (g_mq != nullptr && g_mq->IsAddressPatched(target, 1));
}

// Idempotent: patches every unit whose monitored target is currently hooked and is
// not already patched. Must run on the main thread (see the state-byte comment).
static int ApplyHookScannerPatches()
{
	int applied = 0;

	for (HookCheckUnit& unit : s_checkUnits)
	{
		if (unit.patched || !IsMonitoredTargetHooked(unit.targetAddress))
			continue;

		// Clear the client's stored verdict first. A unit reports on any *change*,
		// including hooked -> clean, so if it has already latched "hooked" -- which
		// happens when a plugin installs its detour after we last ran -- then letting
		// it observe the patched clean verdict would itself emit a report. Both
		// writes must land before the next DoMainLoop pass.
		*reinterpret_cast<volatile uint8_t*>(unit.stateAddress) = 0;

		if (!mq::AddPatch(unit.checkAddress, unit.patchBytes, sizeof(unit.patchBytes),
			unit.originalBytes, "HookScannerCheck"))
		{
			LOG_ERROR("HookMemChecker - failed to patch check unit at 0x{:X} monitoring 0x{:X}",
				unit.checkAddress, unit.targetAddress);
			continue;
		}

		unit.patched = true;
		s_patches.push_back(unit.checkAddress);
		++applied;

		LOG_DEBUG("HookMemChecker - blinded check unit at 0x{:X} monitoring 0x{:X}",
			unit.checkAddress, unit.targetAddress);
	}

	return applied;
}

// Call after anything that may have installed a detour on a monitored function --
// most importantly after a plugin's InitializePlugin has run.
void UpdateHookScannerPatches()
{
	if (s_checkUnits.empty())
		return;

	if (const int applied = ApplyHookScannerPatches(); applied > 0)
		LOG_INFO("HookMemChecker - blinded {} additional check unit(s)", applied);
}

static void HookMemChecker(bool Patch)
{
	LOG_DEBUG("HookMemChecker - {}atching", (Patch) ? "P" : "Unp");

	if (Patch)
	{
		mq::AddPatch(__compress_block, __decompress_block - __compress_block + DETOUR_BYTES_COUNT, "__compress_block");
		EzDetour(Spellmanager__LoadTextSpells, &SpellManager_Detours::LoadTextSpells_Detour, &SpellManager_Detours::LoadTextSpells_Trampoline);

		InstallHooks();

		EnumerateCheckUnits();

		// Fail loudly at zero. A signature that matches nothing is indistinguishable
		// from a signature with nothing to match, and that is exactly how the previous
		// implementation went days without protecting anything while logging "Patching".
		if (s_checkUnits.empty())
		{
			LOG_ERROR("HookMemChecker - FOUND NO CHECK UNITS. The client's hook scanner has "
				"changed shape and MQ's hooks are being reported to the server. Re-derive the "
				"check-unit signature before playing.");
#if !defined(EMULATOR)
			__debugbreak();
#endif
		}
		else
		{
			const int applied = ApplyHookScannerPatches();

			LOG_INFO("HookMemChecker - {} check units found, {} blinded",
				s_checkUnits.size(), applied);

			// __MemChecker4 is hooked unconditionally on live, so at least one unit must
			// match. Zero means the offsets resolved somewhere the client isn't looking.
			if (applied == 0)
			{
				LOG_ERROR("HookMemChecker - {} check units found but NONE monitors a hooked "
					"function. Expected __MemChecker4 at minimum; offsets are likely wrong.",
					s_checkUnits.size());
			}
		}
	}
	else
	{
		RemoveHooks();
		RemoveDetour(Spellmanager__LoadTextSpells);
		mq::RemovePatch(__compress_block);
		s_checkUnits.clear();
	}
}

void InitializeDetours()
{
#if !defined(EMULATOR)
	// hit the debugger if we don't hook this. take no chances
	if (!__MemChecker0
		|| !__MemChecker1
#if defined(__MemChecker4_x)
		|| !__MemChecker4
#endif
		|| !__EncryptPad0)
	{
		__debugbreak();
	}
#endif

	extern_array0 = reinterpret_cast<uint32_t*>(__EncryptPad0);

#if !defined(EMULATOR)
	AddHook(__MemChecker0, memcheck0, memcheck0_tramp);
	AddHook(__MemChecker1, memcheck1, memcheck1_tramp);
#if defined(__MemChecker4_x)
	AddHook(__MemChecker4, memcheck4, memcheck4_tramp);
#endif
#endif
	// DISABLED - do not re-enable without testing a zone-in.
	//
	// decompress_block_detour returns 0 for every call where s_inMemCheck4 is unset,
	// i.e. for all real decompression. On the 2026-09-11 client that kills the game
	// the moment you enter the world.
	//
	// This hook has in fact never been active here: __decompress_block (0x1405ABA40)
	// lies inside the __compress_block patch region registered a few lines below, and
	// MemoryPatcherImpl::AddPatchToList used to reject ANY overlap -- so CreateDetour
	// returned nullptr and AddHook_ discarded the failure silently (it sets
	// hi.address regardless, so IsHooked() still reported true). Relaxing that overlap
	// check to permit detours inside read-only region markers made this hook install
	// for the first time, which is what broke zone-in.
	//
	// Leaving it off restores the behaviour MQ has actually been shipping.
	//AddHook(__decompress_block, decompress_block_detour, decompress_block_trampoline);

	AddHook(__ModuleList, FindModules_Detour, FindModules_Trampoline);
	AddHook(__ProcessList, FindProcesses_Detour, FindProcesses_Trampoline);

	HMODULE hKernel32 = GetModuleHandle("kernel32.dll");
	__Module32First = (uintptr_t)GetProcAddress(hKernel32, "Module32First");
	__Module32Next = (uintptr_t)GetProcAddress(hKernel32, "Module32Next");
	__Process32First = (uintptr_t)GetProcAddress(hKernel32, "Process32First");
	__Process32Next = (uintptr_t)GetProcAddress(hKernel32, "Process32Next");

	AddHook(__Module32First, Module32First_Detour, Module32First_Trampoline);
	AddHook(__Module32Next, Module32Next_Detour, Module32Next_Trampoline);
	AddHook(__Process32First, Process32First_Detour, Process32First_Trampoline);
	AddHook(__Process32Next, Process32Next_Detour, Process32Next_Trampoline);

	HookMemChecker(true);
}

void ShutdownDetours()
{
	HookMemChecker(false);
}

} // namespace mq
