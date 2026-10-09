#include "stdafx.h"
#include <stdlib.h>
#include <process.h>
#include "MessageType.h"
#include <detours.h>
#include "InventorySack_AddItem.h"

#include <codecvt>
#include <memory>

#include "Exports.h"
#include <random>
#include <boost/property_tree/ptree.hpp>                                        
#include <boost/property_tree/json_parser.hpp>       
#include <boost/filesystem.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <iostream>
#include <sstream>
#include "Logger.h"
#include "VTableDispatch.h"
#include "CrashReporter.h"
#include "GameContext.h"


#define STASH_1 0
#define STASH_2 1
#define STASH_3 2
#define STASH_4 3
#define STASH_5 4
#define STASH_6 5
#define STASH_PRIVATE 1000

std::wstring GetIagdFolder();

HANDLE InventorySack_AddItem::m_hEvent;
DataQueue* InventorySack_AddItem::m_dataQueue;

InventorySack_AddItem::GameInfo_GameInfo_Param InventorySack_AddItem::dll_GameInfo_GameInfo_Param;
InventorySack_AddItem::GameInfo_GetHardcore InventorySack_AddItem::dll_GameInfo_GetHardcore;
GetPrivateStash InventorySack_AddItem::privateStashHook;
InventorySack_AddItem::InventorySack_AddItem_Drop InventorySack_AddItem::dll_InventorySack_AddItem_Drop;
InventorySack_AddItem::InventorySack_AddItem_Vec2 InventorySack_AddItem::dll_InventorySack_AddItem_Vec2;
InventorySack_AddItem::InventorySack_SetTransferOpen InventorySack_AddItem::dll_InventorySack_SetTransferOpen;
InventorySack_AddItem::InventorySack_FindNextPosition InventorySack_AddItem::dll_InventorySack_FindNextPosition;
InventorySack_AddItem::GameEngine_OpenMarket InventorySack_AddItem::dll_GameEngine_OpenMarket;
InventorySack_AddItem::GameEngine_CloseMarket InventorySack_AddItem::dll_GameEngine_CloseMarket;
InventorySack_AddItem::PlayerInventoryCtrl_DepositReagents InventorySack_AddItem::dll_PlayerInventoryCtrl_DepositReagents;
InventorySack_AddItem::GameEngine_PlayerSaleRequest InventorySack_AddItem::dll_GameEngine_PlayerSaleRequest;
InventorySack_AddItem::PlayerInventoryCtrl_RemoveItem InventorySack_AddItem::dll_PlayerInventoryCtrl_RemoveItem;
InventorySack_AddItem::ControllerCharacter_SendRemoveItemFromInventory InventorySack_AddItem::dll_ControllerCharacter_SendRemoveItemFromInventory;
InventorySack_AddItem::ControllerCharacter_GetEquipmentCtrl InventorySack_AddItem::dll_ControllerCharacter_GetEquipmentCtrl;
InventorySack_AddItem::InventorySack_ContainsItem InventorySack_AddItem::dll_InventorySack_ContainsItem;
std::wstring InventorySack_AddItem::m_storageFolder;
int InventorySack_AddItem::m_stashTabLootFrom;
int InventorySack_AddItem::m_stashTabDepositTo;
ULONGLONG InventorySack_AddItem::m_lastNotificationTickTime;
bool InventorySack_AddItem::m_isGrimDawnParsed;
SettingsReader InventorySack_AddItem::m_settingsReader;
bool InventorySack_AddItem::m_isActive;
int InventorySack_AddItem::m_gameUpdateIterationsRun;
InventorySack_AddItem::GameEngine_Update InventorySack_AddItem::dll_GameEngine_Update;
bool InventorySack_AddItem::m_isTransferStashOpen;

std::set<std::wstring> InventorySack_AddItem::m_depositQueue;
std::set<std::wstring> InventorySack_AddItem::m_bagQueue;
std::set<unsigned int> InventorySack_AddItem::m_nativeJunkIds;
bool InventorySack_AddItem::m_nativeJunkLoaded;
bool InventorySack_AddItem::m_marketOpen;
unsigned int InventorySack_AddItem::m_vendorId;
GAME::GameEngine* InventorySack_AddItem::m_marketEngine;
void* InventorySack_AddItem::m_controllerCharacter;
boost::mutex InventorySack_AddItem::m_mutex;

void InventorySack_AddItem::EnableHook() {
	VTableDispatch::Init();

	// GameInfo::
	dll_GameInfo_GameInfo_Param = (GameInfo_GameInfo_Param)GetProcAddressOrLogToFile(L"Engine.dll", GAMEINFO_CONSTRUCTOR_ARGS);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach((PVOID*)&dll_GameInfo_GameInfo_Param, Hooked_GameInfo_GameInfo_Param);
	DetourTransactionCommit();

	dll_InventorySack_AddItem_Drop = (InventorySack_AddItem_Drop)GetProcAddressOrLogToFile(L"Game.dll", "?AddItem@InventorySack@GAME@@QEAA_NPEAVItem@2@_N1@Z");
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach((PVOID*)&dll_InventorySack_AddItem_Drop, Hooked_InventorySack_AddItem_Drop);
	DetourTransactionCommit();


	dll_InventorySack_AddItem_Vec2 = (InventorySack_AddItem_Vec2)GetProcAddressOrLogToFile(L"Game.dll", "?AddItem@InventorySack@GAME@@QEAA_NAEBVVec2@2@PEAVItem@2@_N@Z");
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach((PVOID*)&dll_InventorySack_AddItem_Vec2, Hooked_InventorySack_AddItem_Vec2);
	DetourTransactionCommit();


	dll_InventorySack_SetTransferOpen = (InventorySack_SetTransferOpen)HookGame(
		SET_TRANSFER_OPEN,
		Hooked_InventorySack_SetTransferOpen,
		m_dataQueue,
		m_hEvent,
		2 // Diagnostic hook id only (formerly TYPE_OPEN_CLOSE_TRANSFER_STASH); instaloot needs this hook to know when the transfer stash is open.
	);

	m_isTransferStashOpen = false;

	dll_GameEngine_Update = (GameEngine_Update)HookGame(
		"?Update@GameEngine@GAME@@QEAAXH@Z",
		Hooked_GameEngine_Update,
		m_dataQueue,
		m_hEvent,
		TYPE_GAMEENGINE_UPDATE
	);

	
	m_isActive = false;
	m_gameUpdateIterationsRun = 0;
	privateStashHook.EnableHook();

	// bool GAME::GameInfo::GetHardcore(void)
	dll_GameInfo_GetHardcore = (GameInfo_GetHardcore)GetProcAddressOrLogToFile(L"Engine.dll", GET_HARDCORE);
	dll_InventorySack_FindNextPosition = (InventorySack_FindNextPosition)GetProcAddressOrLogToFile(L"Game.dll", "?FindNextPosition@InventorySack@GAME@@IEBA_NPEBVItem@2@AEAVRect@2@_N@Z");

	// The vendor action is deliberately tied to the game's own Move Reagents button. The button's normal
	// confirmation remains in charge; after the player confirms, this hook swaps only the action when a vendor
	// is open and IA has native Junk ids.
	dll_GameEngine_OpenMarket = (GameEngine_OpenMarket)GetProcAddressOrLogToFile(L"Game.dll", "?OpenMarket@GameEngine@GAME@@QEAAXIAEAW4Market_TypeEnum@2@AEBVVec2@2@@Z");
	dll_GameEngine_CloseMarket = (GameEngine_CloseMarket)GetProcAddressOrLogToFile(L"Game.dll", "?CloseMarket@GameEngine@GAME@@QEAAXI@Z");
	dll_PlayerInventoryCtrl_DepositReagents = (PlayerInventoryCtrl_DepositReagents)GetProcAddressOrLogToFile(L"Game.dll", "?DepositReagents@PlayerInventoryCtrl@GAME@@QEAAXXZ");
	dll_GameEngine_PlayerSaleRequest = (GameEngine_PlayerSaleRequest)GetProcAddressOrLogToFile(L"Game.dll", "?PlayerSaleRequest@GameEngine@GAME@@QEAA_NII_N@Z");
	dll_PlayerInventoryCtrl_RemoveItem = (PlayerInventoryCtrl_RemoveItem)GetProcAddressOrLogToFile(L"Game.dll", "?RemoveItem@PlayerInventoryCtrl@GAME@@QEAA_NI_N@Z");
	dll_ControllerCharacter_SendRemoveItemFromInventory = (ControllerCharacter_SendRemoveItemFromInventory)GetProcAddressOrLogToFile(L"Game.dll", "?SendRemoveItemFromInventory@ControllerCharacter@GAME@@QEAAXI@Z");
	dll_ControllerCharacter_GetEquipmentCtrl = (ControllerCharacter_GetEquipmentCtrl)GetProcAddressOrLogToFile(L"Game.dll", "?GetEquipmentCtrl@ControllerCharacter@GAME@@QEAAAEAVEquipmentCtrl@2@XZ");
	dll_InventorySack_ContainsItem = (InventorySack_ContainsItem)GetProcAddressOrLogToFile(L"Game.dll", "?ContainsItem@InventorySack@GAME@@QEBA_NI@Z");
	LoadNativeJunkIds();

	if (dll_GameEngine_OpenMarket != nullptr) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach((PVOID*)&dll_GameEngine_OpenMarket, Hooked_GameEngine_OpenMarket);
		DetourTransactionCommit();
	}
	if (dll_GameEngine_CloseMarket != nullptr) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach((PVOID*)&dll_GameEngine_CloseMarket, Hooked_GameEngine_CloseMarket);
		DetourTransactionCommit();
	}
	if (dll_PlayerInventoryCtrl_DepositReagents != nullptr) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach((PVOID*)&dll_PlayerInventoryCtrl_DepositReagents, Hooked_PlayerInventoryCtrl_DepositReagents);
		DetourTransactionCommit();
	}
	if (SortInventorySack != nullptr) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach((PVOID*)&SortInventorySack, Hooked_InventorySack_Sort);
		DetourTransactionCommit();
	}
	if (dll_ControllerCharacter_GetEquipmentCtrl != nullptr) {
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach((PVOID*)&dll_ControllerCharacter_GetEquipmentCtrl, Hooked_ControllerCharacter_GetEquipmentCtrl);
		DetourTransactionCommit();
	}

	
	if (m_isGrimDawnParsed) {
		LogToFile(LogLevel::INFO, L"Grim is parsed, displaying message..");
		DisplayMessage(L"Item Assistant", L"Item monitoring enabled");
	}
	else {
		LogToFile(LogLevel::INFO, L"Grim is not parsed, skipping message..");
	}

	LogToFile(LogLevel::INFO, L"Instaloot hook enabled");
}


InventorySack_AddItem::InventorySack_AddItem(DataQueue* dataQueue, HANDLE hEvent) {
	InventorySack_AddItem::m_dataQueue = dataQueue;
	InventorySack_AddItem::m_hEvent = hEvent;
	privateStashHook = GetPrivateStash(dataQueue, hEvent);
	m_storageFolder = GetIagdFolder() + L"itemqueue\\ingoing\\";
	LogToFile(LogLevel::INFO, L"Storing instaloot items into " + m_storageFolder);

	boost::filesystem::create_directories(m_storageFolder);
	m_settingsReader = SettingsReader();
	m_stashTabLootFrom = m_settingsReader.GetStashTabToLootFrom();
	m_stashTabDepositTo = m_settingsReader.GetStashTabToDepositTo();
	m_isGrimDawnParsed = m_settingsReader.GetIsGrimDawnParsed();
	m_lastNotificationTickTime = 0;
	m_isActive = false;
	m_gameUpdateIterationsRun = 0;
	m_nativeJunkLoaded = false;
	m_marketOpen = false;
	m_vendorId = 0;
	m_marketEngine = nullptr;
	m_controllerCharacter = nullptr;
}

InventorySack_AddItem::InventorySack_AddItem() {
	InventorySack_AddItem::m_hEvent = NULL;
}

void InventorySack_AddItem::DisableHook() {
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());


	DetourDetach((PVOID*)&dll_GameInfo_GameInfo_Param, Hooked_GameInfo_GameInfo_Param);
	
	DetourTransactionCommit();

	privateStashHook.DisableHook();
}

/// <summary>
/// Allows for the activation / deactivation of instaloot when Item Assistant is not running.
/// This prevents GD from instalooting items when the IA client is closed
/// </summary>
/// <param name="isActive"></param>
void InventorySack_AddItem::SetActive(bool isActive)
{
	bool isActivating = isActive && !m_isActive;
	m_isActive = isActive;

	// IA has been shut down, start the thread now
	if (isActivating) {
		(HANDLE)_beginthread(ThreadMain, NULL, 0);
	}
}



// Since were creating from an existing object we'll need to call Get() on isHardcore and ModLabel
void* __fastcall InventorySack_AddItem::Hooked_GameInfo_GameInfo_Param(void* This , void* info) {
	void* result = dll_GameInfo_GameInfo_Param(This, info);
	try {
		bool isHardcore = dll_GameInfo_GetHardcore(This);
		DataItemPtr dataEvent(new DataItem(TYPE_GameInfo_IsHardcore_via_init, sizeof(isHardcore), (char*)&isHardcore));
		m_dataQueue->push(dataEvent);
		SetEvent(m_hEvent);
	}
	catch (std::exception& ex) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wide = converter.from_bytes(ex.what());
		LogToFile(LogLevel::FATAL, L"Error parsing in InventorySack_AddItem::Hooked_GameInfo_GameInfo_Param.. " + wide);
	}
	catch (...) {
		LogToFile(LogLevel::FATAL, L"Error parsing in InventorySack_AddItem::Hooked_GameInfo_GameInfo_Param.. (triple-dot)");
	}

	return result;
}



/// <summary>
/// Called primarily when dropping an item into a closed stash (eg drop item from tab 3 to tab 4)
/// </summary>
/// <param name="This"></param>
/// <param name="item"></param>
/// <param name="findPosition"></param>
/// <param name="SkipPlaySound"></param>
/// <returns></returns>
void* __fastcall InventorySack_AddItem::Hooked_InventorySack_AddItem_Drop(void* This, GAME::Item *item, bool findPosition, bool SkipPlaySound) {
	fnNoteItemAdded(); // Diagnostics only -- fires for every sack, including the player's own inventory.
	CrashReporter::Note("AddItem(Drop)", (uint64_t)This, (uint64_t)item);
	try {
		if (HandleItem(This, item)) {
			return (void*)1;
		}
	}
	catch (std::exception& ex) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wide = converter.from_bytes(ex.what());
		LogToFile(LogLevel::FATAL, L"Error looting item in Hooked_InventorySack_AddItem_Vec2.. " + wide);
	}
	catch (...) {
		LogToFile(LogLevel::FATAL, L"Error looting item in Hooked_InventorySack_AddItem_Vec2.. (triple-dot)");
	}

	void* v = dll_InventorySack_AddItem_Drop(This, item, findPosition, SkipPlaySound);
	return v;
}

/// <summary>
/// Regular "add to tab/stash" move. Ctrl+click and regular item drop
/// </summary>
/// <param name="This"></param>
/// <param name="position"></param>
/// <param name="item"></param>
/// <param name="SkipPlaySound"></param>
/// <returns></returns>
void* __fastcall InventorySack_AddItem::Hooked_InventorySack_AddItem_Vec2(void* This, void* position, GAME::Item* item, bool SkipPlaySound) {
	fnNoteItemAdded(); // Diagnostics only -- fires for every sack, including the player's own inventory.
	CrashReporter::Note("AddItem(Vec2)", (uint64_t)This, (uint64_t)item);
	try {
		if (HandleItem(This, item)) {
			return (void*)1;
		}
	}
	catch (std::exception& ex) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wide = converter.from_bytes(ex.what());
		LogToFile(LogLevel::FATAL, L"Error looting item in Hooked_InventorySack_AddItem_Vec2.. " + wide);
	}
	catch (...) {
		LogToFile(LogLevel::FATAL, L"Error looting item in Hooked_InventorySack_AddItem_Vec2.. (triple-dot)");
	}

	return dll_InventorySack_AddItem_Vec2(This, position, item, SkipPlaySound);
}

void* __fastcall InventorySack_AddItem::Hooked_InventorySack_SetTransferOpen(void* This, bool isOpen) {
	m_isTransferStashOpen = isOpen;
	return dll_InventorySack_SetTransferOpen(This, isOpen);
}

/// <summary>
/// Checks if the provided replica info contains any of the exclusion parameters
/// We are not interested in potions, quest items, components or soulbound items.
/// </summary>
/// <param name="item"></param>
/// <returns></returns>
/// Dump the full contents of an ItemReplicaInfo for diagnostics.
/// Logs every field via the C++ struct mapping, plus a raw hex dump of the
/// object so we can locate where the real fields land if the struct layout
/// has drifted from the game binary.
static void DumpReplicaInfo(const GAME::ItemReplicaInfo& item) {
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
	auto w = [&](const std::string& s) -> std::wstring {
		try { return conv.from_bytes(s); }
		catch (...) { return L"<non-utf8>"; }
	};

	std::wstring msg = L"=== ItemReplicaInfo dump ===";
	msg += L"\n  id                = " + std::to_wstring(item.id);
	msg += L"\n  baseRecord        = " + w(item.baseRecord);
	msg += L"\n  prefixRecord      = " + w(item.prefixRecord);
	msg += L"\n  suffixRecord      = " + w(item.suffixRecord);
	msg += L"\n  seed              = " + std::to_wstring(item.seed);
	msg += L"\n  modifierRecord    = " + w(item.modifierRecord);
	msg += L"\n  materiaRecord     = " + w(item.materiaRecord);
	msg += L"\n  relicBonus        = " + w(item.relicBonus);
	msg += L"\n  relicSeed         = " + std::to_wstring(item.relicSeed);
	msg += L"\n  enchantmentRecord = " + w(item.enchantmentRecord);
	msg += L"\n  enchantmentLevel  = " + std::to_wstring(item.enchantmentLevel);
	msg += L"\n  enchantmentSeed   = " + std::to_wstring(item.enchantmentSeed);
	msg += L"\n  transmuteRecord   = " + w(item.transmuteRecord);
	msg += L"\n  ascendant1        = " + w(item.ascendant1);
	msg += L"\n  ascendant2        = " + w(item.ascendant2);
	msg += L"\n  var1              = " + std::to_wstring(item.var1);
	msg += L"\n  velocity          = (" + std::to_wstring(item.velocity.x) + L", "
		+ std::to_wstring(item.velocity.y) + L", " + std::to_wstring(item.velocity.z) + L")";
	msg += L"\n  owner             = " + std::to_wstring(item.owner);
	msg += L"\n  stackSize         = " + std::to_wstring(item.stackSize);
	msg += L"\n  seedRerolls       = " + std::to_wstring(item.seedRerolls);
	msg += L"\n  affixRerolls      = " + std::to_wstring(item.affixRerolls);

	// Raw hex dump of the object. sizeof gives us the struct our DLL believes
	// in; the game may write more, but this shows how our fields overlay memory.
	const unsigned char* raw = reinterpret_cast<const unsigned char*>(&item);
	const size_t rawLen = sizeof(GAME::ItemReplicaInfo);
	msg += L"\n  sizeof            = " + std::to_wstring(rawLen);

	wchar_t buf[64];
	std::wstring hex;
	for (size_t i = 0; i < rawLen; ++i) {
		if (i % 16 == 0) {
			swprintf(buf, 64, L"\n    %04zX:", i);
			hex += buf;
		}
		swprintf(buf, 64, L" %02X", raw[i]);
		hex += buf;
	}
	msg += L"\n  raw bytes:" + hex;

	// Dword view with absolute offsets. The tail is where the layout is drifting,
	// so this makes it trivial to spot which offset actually holds stackSize (==1).
	std::wstring dwords;
	for (size_t i = 0; i + 4 <= rawLen; i += 4) {
		unsigned int v;
		memcpy(&v, raw + i, sizeof(v));
		swprintf(buf, 64, L"\n    +0x%03zX (%3zu) = %u", i, i, v);
		dwords += buf;
	}
	msg += L"\n  dwords:" + dwords;

	LogToFile(LogLevel::INFO, msg);
}

bool InventorySack_AddItem::IsRelevant(const GAME::ItemReplicaInfo& item) {

	if (!m_isGrimDawnParsed) {
		m_isGrimDawnParsed = m_settingsReader.GetIsGrimDawnParsed();
		if (!m_isGrimDawnParsed) {
			DisplayMessage(L"Item not looted", L"Grim Dawn not parsed");
			return false;
		} else {
			DisplayMessage(L"Item Assistant", L"Item monitoring enabled");
		}
	}
	

	if (item.stackSize > 1) {
		LogToFile(LogLevel::WARNING, (L"Stackable item - IA does not loot stackable items, got " + std::to_wstring(item.stackSize) + L" in stacksize"));
		DisplayMessage(L"Stackable item - IA does not loot stackable items", L"Item Assistant");
		// DumpReplicaInfo(item);
		return false;
	}
	
	if (item.baseRecord.find("/storyelements/") != std::string::npos) {
		// We'll allow lokarr, but only lokarr out of storyelements items.
		if (item.baseRecord.find("records/storyelements/signs/signh.dbr") != std::string::npos) {} // Lokarr's Gaze
		else if (item.baseRecord.find("records/storyelements/signs/signf.dbr") != std::string::npos) {} // Lokarr's Boots
		else if (item.baseRecord.find("records/storyelements/signs/signs.dbr") != std::string::npos) {} // Lokarr's Mantle
		else if (item.baseRecord.find("records/storyelements/signs/signt.dbr") != std::string::npos) {} // Lokarr's Coat
		else if (item.baseRecord.find("records/storyelements/questassets/q000_torso.dbr") != std::string::npos) {} // Gazer Man
		else if (item.baseRecord.find("records/endlessdungeon/items/q001_torso.dbr") != std::string::npos) {} // Miss Gazer Man
		else {
			DisplayMessage(L"Quest item - IA does not support this specific item", L"Item Assistant");
			return false;
		}
	}

	if (item.baseRecord.find("/materia/") != std::string::npos) {
		DisplayMessage(L"Component ignored - IA does not loot components", L"Item Assistant");
		return false;
	}

	if (item.baseRecord.find("records/items/misc/") != std::string::npos) {
		DisplayMessage(L"Special item ignored - IA does not loot this item", L"Item Assistant");
		return false;
	}
	
	if (item.baseRecord.find("/questitems/") != std::string::npos) {
		DisplayMessage(L"Quest item ignored - IA does not loot quest items", L"Item Assistant");
		return false;
	}

	if (item.baseRecord.find("/crafting/") != std::string::npos) {
		DisplayMessage(L"Component ignored - IA does not loot components", L"Item Assistant");
		return false;
	}

	// Salt bag - Frequently get questions about these (Lifegiver is OK, both regular and mystical)
	if (item.baseRecord.find("gearaccessories/necklaces/a00_necklace.dbr") != std::string::npos ) {
		DisplayMessage(L"Special item - This item is not supported by IA", L"Item Assistant");
		return false;
	}

	return true;
}



std::wstring randomFilename() {
	std::wstring str(L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

	std::random_device rd;
	std::mt19937 generator(rd());

	std::shuffle(str.begin(), str.end(), generator);

	return str.substr(0, 32) + L".csv";    // assumes 32 < number of characters in str         
}



bool InventorySack_AddItem::Persist(
	GAME::ItemReplicaInfo replicaInfo,
	bool isHardcore,
	std::wstring mod,
	const std::vector<GAME::GameTextLine>& gameTextLines)
{
	std::wstring fullPath = m_storageFolder + randomFilename();
	std::wstring fullPathTmp = fullPath + L".tmp";

	// Use std::ofstream (narrow) and convert all wide strings to UTF-8 explicitly.
	// std::wofstream converts through the system ANSI codepage, which destroys
	// characters outside that codepage (e.g. Polish, Portuguese).
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> utf8conv;

	std::ofstream stream;
	stream.open(fullPathTmp, std::ios::binary);

	// Write UTF-8 BOM so readers can detect encoding
	stream << "\xEF\xBB\xBF";

	// Existing replica CSV line -- unchanged, IA client reads this format already.
	// mod and Serialize output are ASCII-safe, but convert them properly anyway.
	stream << utf8conv.to_bytes(mod) << ";" << (isHardcore ? 1 : 0) << ";" << utf8conv.to_bytes(GAME::Serialize(replicaInfo)) << "\n";

	// Append stats, one per line: textClass;text
	// The IA client can read these after the first line, ignoring them if it doesn't
	// understand them yet (backwards compatible, since it only reads line 1 today).
	for (const auto& line : gameTextLines) {
		// Sanitize: strip \r\n from text to prevent multi-line corruption
		std::string textUtf8 = utf8conv.to_bytes(line.text);
		textUtf8.erase(std::remove(textUtf8.begin(), textUtf8.end(), '\r'), textUtf8.end());
		textUtf8.erase(std::remove(textUtf8.begin(), textUtf8.end(), '\n'), textUtf8.end());
		stream << line.textClass << ";" << textUtf8 << "\n";
	}

	stream.flush();
	stream.close();

	LogToFile(LogLevel::INFO, L"Storing to " + fullPath + L" (" +
		std::to_wstring(gameTextLines.size()) + L" stat lines)");

	std::ifstream verification;
	verification.open(fullPathTmp);
	if (verification) {
		verification.close();
		if (MoveFileW(fullPathTmp.c_str(), fullPath.c_str()))
			return true;
	}

	LogToFile(LogLevel::WARNING, L"Error: written CSV file does not exist");
	return false;
}


void InventorySack_AddItem::DisplayMessage(std::wstring text, std::wstring body) {
	const ULONGLONG now = GetTickCount64();
	try {


		// Limit notifications to 1 per 3s, roughly the fade time.
		if (now - m_lastNotificationTickTime > 3000) {
			GAME::Color color;
			color.r = 1;
			color.g = 1;
			color.b = 1;
			color.a = 1;

			// TODO: How can translation support be added?

			GAME::Engine* engine = fnGetEngine();
			if (engine == nullptr) {
				LogToFile(LogLevel::WARNING, L"Attempted to display text in-game, but no engine was set.");
				return;
			}


			LogToFile(LogLevel::INFO, L"Display: " + text + L" - " + body);

			// Checking if the game is loading here says true.. checking if its waiting crashes.. odd..
			fnShowCinematicText(engine, &text, &body, 5, &color, false);
			m_lastNotificationTickTime = now;

		} else {
			LogToFile(LogLevel::INFO, L"Muted: " + text);
		}
	}
	catch (std::exception& ex) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wide = converter.from_bytes(ex.what());

		LogToFile(LogLevel::WARNING, L": Encountered an exception while displaying text in-game: " + wide);
	}
	catch (...) {
		LogToFile(LogLevel::WARNING, L"ERROR: Encountered an exception while displaying text in-game");
	}
}

/// <summary>
/// 
/// </summary>
/// <param name="stashTab">GAME::InventorySack*</param>
/// <returns>If this inventory sack is the one we wish to loot items from</returns>
bool InventorySack_AddItem::IsSackToLootFrom(void* stashTab, GAME::GameEngine* gameEngine) {
	if (gameEngine == nullptr) {
		return false;
	}

	void* realPtr = fnGetPlayerTransfer(gameEngine);
	if (realPtr == nullptr) {
		return false;
	}

	auto sacks = static_cast<std::vector<GAME::InventorySack*>*>(realPtr);

	// Not enough sacks to support running IA
	if (sacks->size() < 2) {
		return false;
	}


	// Determine the correct stash sack..
	size_t toLootFrom;
	if (m_stashTabLootFrom == 0) {
		toLootFrom = sacks->size() - 1;
	}
	else {
		// m_stashTabLootFrom is index from 1, we never want to go <0 nor >= size.
		toLootFrom = max(0,
			min(sacks->size() - 1, m_stashTabLootFrom - 1)
		);
	}

	// Is this the sack we want to loot from?
	const auto sourceSackPtr = sacks->at(toLootFrom);
	return static_cast<void*>(sourceSackPtr) == stashTab;
}

/// <summary>
/// Attempt to classify the item as relevant <-> not relevant for looting, and pass it on for persisting.
/// </summary>
/// <param name="stash"></param>
/// <param name="item"></param>
/// <returns></returns>
bool InventorySack_AddItem::HandleItem(void* stash, GAME::Item* item) {
	if (!m_isActive || stash == nullptr || item == nullptr)
		return false;

	auto gameEngine = fnGetGameEngine();
	if (!fnIsWorldAlive(gameEngine))
		return false;

	if (!IsSackToLootFrom(stash, gameEngine))
		return false;

	GAME::ItemReplicaInfo replica;
	fnItemGetItemReplicaInfo(item, replica);
	if (!IsRelevant(replica))
		return false;

	GAME::Engine* engine = fnGetEngine();
	if (engine == nullptr) {
		LogToFile(LogLevel::WARNING, L"Engine is null, aborting..");
		return false;
	}
	GAME::GameInfo* gameInfo = fnGetGameInfo(engine);
	if (gameInfo == nullptr) {
		LogToFile(LogLevel::WARNING, L"GameInfo is null, aborting..");
		return false;
	}

	std::vector<GAME::GameTextLine> gameTextLines = {};
	GAME::Character* character = (GAME::Character*)fnGetMainPlayer(gameEngine);
	if (character != nullptr) {
		VTableDispatch::Call(item, character, &gameTextLines, true);
		LogToFile(LogLevel::INFO, L"VTableDispatch returned " +
			std::to_wstring(gameTextLines.size()) + L" text lines");
	}
	else {
		LogToFile(LogLevel::WARNING, L"HandleItem: no character, persisting without stats");
	}

	// Resolved here rather than read from the cache: this is the game's own thread, so it is free to
	// make the call, and the loot path then does not depend on GameEngine::Update having run first.
	std::wstring modName;
	bool isHardcore = false;
	if (!GameContext::Resolve(gameInfo, modName, isHardcore)) {
		LogToFile(LogLevel::WARNING, L"HandleItem: no game context, cannot tell which queue to persist to");
		return false;
	}

	if (Persist(replica, isHardcore, modName, gameTextLines)) {
		DisplayMessage(L"Item looted", L"By Item Assistant");
		fnPlayDropSound(item);
		return true;
	}

	return false;
}

/// <summary>
/// We look for CSV files that the IA client has written to a specific folder, when wanting to move items back into the game.
/// </summary>
/// <param name="modName"></param>
/// <param name="isHardcore"></param>
/// <returns></returns>
std::wstring GetFolderToLootFrom(std::wstring modName, bool isHardcore) {
	boost::property_tree::wptree loadPtreeRoot;

	std::wstring folder;
	if (modName.empty()) {
		folder = GetIagdFolder() + L"itemqueue\\outgoing\\" + (isHardcore ? L"hc" : L"sc");
	}
	else {
		folder = GetIagdFolder() + L"itemqueue\\outgoing\\" + (isHardcore ? L"hc" : L"sc") + L"\\" + modName;
	}

	if (!boost::filesystem::is_directory(folder)) {
		boost::filesystem::create_directories(folder);
	}

	return folder;
}

/// <summary>
/// When depositing items in-game, instead of deleting the CSV, we just move them into a "deleted" folder. (soft-delete)
/// It is up to the IA client to delete these CSV files in a timely manner.
/// </summary>
/// <param name="modName"></param>
/// <param name="isHardcore"></param>
/// <returns></returns>
std::wstring GetFolderToMoveTo(std::wstring modName, bool isHardcore) {
	boost::property_tree::wptree loadPtreeRoot;

	std::wstring folder;
	if (modName.empty()) {
		folder = GetIagdFolder() + L"itemqueue\\deleted\\" + (isHardcore ? L"hc" : L"sc");
	}
	else {
		folder = GetIagdFolder() + L"itemqueue\\deleted\\" + (isHardcore ? L"hc" : L"sc") + L"\\" + modName;
	}

	if (!boost::filesystem::is_directory(folder)) {
		boost::filesystem::create_directories(folder);
	}

	return folder;
}

/// <summary>Queue roots for the separate IA -> player-bag path.</summary>
static std::wstring GetBagFolder(std::wstring modName, bool isHardcore, const wchar_t* root) {
	std::wstring folder = GetIagdFolder() + L"itemqueue\\" + root + L"\\" + (isHardcore ? L"hc" : L"sc");
	if (!modName.empty()) {
		folder += L"\\" + modName;
	}
	boost::filesystem::create_directories(folder);
	return folder;
}

/// <summary>
/// Read a .CSV file into a GAME::ItemReplicaInfo object
/// </summary>
/// <param name="filename">A valid CSV file</param>
/// <returns></returns>
GAME::ItemReplicaInfo* InventorySack_AddItem::ReadReplicaInfo(const std::wstring& filename) {
	try {
		std::ifstream file(filename);
		return GAME::Deserialize(GAME::GetNextLineAndSplitIntoTokens(file));
	}
	catch (std::exception& ex) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wide = converter.from_bytes(ex.what());

		LogToFile(LogLevel::FATAL, L"ERROR Creating ReplicaItem.." + wide);

	}
	catch (...) {
		LogToFile(LogLevel::FATAL, L"ERROR Creating ReplicaItem.. (triple-dot)");
	}

	return nullptr;
}

/// <summary>
/// Add one queued item to the player's actual inventory sacks. This intentionally uses the same
/// PlayerInventoryCtrl::GetNumberOfSacks/GetSack route as the game UI; Player::GetSack is a different
/// collection and is not the visible personal bag inventory.
/// </summary>
bool InventorySack_AddItem::ProcessBagItem(GAME::GameEngine* gameEngine, const std::wstring& filename) {
	if (gameEngine == nullptr || fnGetMainPlayer == nullptr || fnPlayerGetController == nullptr ||
		fnControllerGetInventoryCtrl == nullptr || fnPlayerInventoryCtrl_GetNumberOfSacks == nullptr ||
		fnPlayerInventoryCtrl_GetSack == nullptr || dll_InventorySack_FindNextPosition == nullptr ||
		dll_InventorySack_AddItem_Vec2 == nullptr) {
		LogToFile(LogLevel::WARNING, L"Player-bag transfer skipped: inventory exports are unavailable");
		return false;
	}

	GAME::Player* player = fnGetMainPlayer(gameEngine);
	GAME::ControllerPlayer* controller = m_controllerCharacter != nullptr
		? reinterpret_cast<GAME::ControllerPlayer*>(m_controllerCharacter)
		: (player == nullptr ? nullptr : fnPlayerGetController(player));
	GAME::PlayerInventoryCtrl* inventory = controller == nullptr ? nullptr : fnControllerGetInventoryCtrl(controller);
	if (inventory == nullptr) {
		return false;
	}

	const unsigned int sackCount = fnPlayerInventoryCtrl_GetNumberOfSacks(inventory);
	if (sackCount == 0 || sackCount > 64) {
		LogToFile(LogLevel::WARNING, L"Player-bag transfer found an implausible sack count: " + std::to_wstring(sackCount));
		return false;
	}

	GAME::ItemReplicaInfo* replica = ReadReplicaInfo(filename);
	if (replica == nullptr) {
		return false;
	}

	bool accepted = false;
	try {
		GAME::Item* item = fnCreateItem(replica);
		if (item != nullptr) {
			for (unsigned int i = 0; i < sackCount && !accepted; ++i) {
				GAME::InventorySack* sack = fnPlayerInventoryCtrl_GetSack(inventory, static_cast<int>(i));
				if (sack == nullptr) {
					continue;
				}

				GAME::Rect position;
				if (dll_InventorySack_FindNextPosition(sack, item, &position, true)) {
					// The existing typedef is intentionally pointer-shaped for the older hook ABI; the game
					// returns a non-zero BOOL here, so null remains the failure result.
					accepted = dll_InventorySack_AddItem_Vec2(sack, (void*)&position, item, false) != nullptr;
				}
			}

			if (accepted && fnItemGetItemReplicaInfo != nullptr) {
				// CreateItem assigns the live Grim Dawn item id. Keep that native id, not IA's database id,
				// because the vendor sale API operates on the game's inventory objects.
				GAME::ItemReplicaInfo liveReplica;
				fnItemGetItemReplicaInfo(item, liveReplica);
				if (liveReplica.id != 0) {
					m_nativeJunkIds.insert(liveReplica.id);
					SaveNativeJunkIds();
					LogToFile(LogLevel::INFO, L"Marked newly inserted player-bag item as native Junk id " + std::to_wstring(liveReplica.id));
				}
			}
		}
	}
	catch (...) {
		LogToFile(LogLevel::WARNING, L"Exception while inserting an item into player bags");
		accepted = false;
	}

	delete replica;
	return accepted;
}

/// <summary>
/// Move eligible items from the live player bags directly into IA's incoming
/// queue.  This is deliberately independent of the transfer-stash sacks: the
/// database is unlimited, so opening an in-game stash is unnecessary.
/// </summary>
int InventorySack_AddItem::MovePlayerBagsToIA(
	GAME::GameEngine* gameEngine,
	GAME::PlayerInventoryCtrl* inventory)
{
	if (gameEngine == nullptr || inventory == nullptr || fnGetMainPlayer == nullptr ||
		fnCharacterGetInventoryReplica == nullptr || fnPlayerInventoryRemoveItem == nullptr) {
		LogToFile(LogLevel::WARNING, L"Direct bag-to-IA transfer skipped: inventory exports are unavailable");
		return 0;
	}

	GAME::Player* player = fnGetMainPlayer(gameEngine);
	if (player == nullptr) {
		return 0;
	}

	GAME::Engine* engine = fnGetEngine();
	GAME::GameInfo* gameInfo = engine == nullptr ? nullptr : fnGetGameInfo(engine);
	if (gameInfo == nullptr) {
		LogToFile(LogLevel::WARNING, L"Direct bag-to-IA transfer skipped: game context is unavailable");
		return 0;
	}

	std::wstring modName;
	bool isHardcore = false;
	if (!GameContext::Resolve(gameInfo, modName, isHardcore)) {
		LogToFile(LogLevel::WARNING, L"Direct bag-to-IA transfer skipped: game context could not be resolved");
		return 0;
	}

	// A Grim Dawn character cannot carry anywhere near this many distinct
	// item records.  Supplying our own capacity keeps the game allocator out of
	// this temporary vector and lets us destroy only the string-bearing records
	// that were actually constructed by GetInventoryReplica.
	constexpr size_t maxReplicas = 1024;
	std::unique_ptr<unsigned char[]> storage(new unsigned char[sizeof(GAME::InventoryReplica) * maxReplicas]);
	GAME::InventoryReplicaVector replicas;
	replicas.begin = reinterpret_cast<GAME::InventoryReplica*>(storage.get());
	replicas.end = replicas.begin;
	replicas.capacity = replicas.begin + maxReplicas;

	try {
		fnCharacterGetInventoryReplica(
			reinterpret_cast<const GAME::Character*>(player), replicas);
	}
	catch (...) {
		LogToFile(LogLevel::WARNING, L"Direct bag-to-IA transfer failed while reading character inventory replicas");
		return 0;
	}

	if (replicas.begin != reinterpret_cast<GAME::InventoryReplica*>(storage.get()) ||
		replicas.end < replicas.begin || replicas.end > replicas.capacity) {
		// The fixed buffer should be sufficient for every legal player inventory.
		// If a future game build changes that assumption, refuse the operation
		// rather than touching memory owned by an unknown allocator.
		LogToFile(LogLevel::WARNING, L"Direct bag-to-IA transfer refused: inventory replica count exceeded the safety buffer");
		return 0;
	}

	const size_t count = static_cast<size_t>(replicas.end - replicas.begin);
	int moved = 0;
	void* removeController = m_controllerCharacter;
	if (removeController == nullptr && fnPlayerGetController != nullptr) {
		removeController = fnPlayerGetController(player);
	}

	for (size_t i = 0; i < count; ++i) {
		GAME::InventoryReplica& inventoryReplica = replicas.begin[i];
		GAME::ItemReplicaInfo& replica = inventoryReplica.item;

		// GetInventoryReplica includes equipped items.  Those records carry an
		// equipment location; UINT_MAX is the marker used by the game for bag
		// items, which are the only items this button may move.
		if (inventoryReplica.equipmentLocation != 0xffffffffu || replica.id == 0) {
			continue;
		}
		if (!IsRelevant(replica)) {
			continue;
		}

		// Write the durable IA record before removing the live item.  If the
		// game refuses the removal, the item remains in the bag and the log makes
		// the duplicate-safe failure visible instead of risking item loss.
		if (!Persist(replica, isHardcore, modName, {})) {
			LogToFile(LogLevel::WARNING, L"Direct bag-to-IA transfer could not persist item " + std::to_wstring(replica.id));
			continue;
		}

		if (!fnPlayerInventoryRemoveItem(inventory, replica.id, true)) {
			LogToFile(LogLevel::WARNING, L"Direct bag-to-IA item persisted but could not be removed from player bags: " + std::to_wstring(replica.id));
			continue;
		}

		if (removeController != nullptr && dll_ControllerCharacter_SendRemoveItemFromInventory != nullptr) {
			dll_ControllerCharacter_SendRemoveItemFromInventory(removeController, replica.id);
		}
		++moved;
		LogToFile(LogLevel::INFO, L"Moved player-bag item directly into IA: native id " + std::to_wstring(replica.id));
	}

	// The vector storage itself belongs to this function.  Its elements contain
	// std::string fields constructed by the game's copy routine, so run the
	// normal ItemReplicaInfo destructor before releasing the raw buffer.
	for (size_t i = 0; i < count; ++i) {
		replicas.begin[i].item.~ItemReplicaInfo();
	}

	if (moved > 0) {
		DisplayMessage(std::to_wstring(moved) + L" item(s) moved to Item Assistant", L"By Item Assistant");
	}
	return moved;
}

static std::wstring NativeJunkFile() {
	return GetIagdFolder() + L"data\\native-junk-items.txt";
}

void InventorySack_AddItem::LoadNativeJunkIds() {
	if (m_nativeJunkLoaded) {
		return;
	}

	m_nativeJunkLoaded = true;
	const std::wstring filename = NativeJunkFile();
	std::wifstream file(filename);
	std::wstring owner;
	file >> owner;
	if (owner != L"pid=" + std::to_wstring(GetCurrentProcessId())) {
		// Native item ids are valid for this game process only. IA marks remain persistent, but an old
		// process's numeric ids must never be reused to sell a different item after a game restart.
		LogToFile(LogLevel::INFO, L"Ignoring native Junk marks from another game process");
		return;
	}
	unsigned int id = 0;
	while (file >> id) {
		if (id != 0) m_nativeJunkIds.insert(id);
	}
	LogToFile(LogLevel::INFO, L"Loaded " + std::to_wstring(m_nativeJunkIds.size()) + L" native Junk item ids");
}

void InventorySack_AddItem::SaveNativeJunkIds() {
	const std::wstring filename = NativeJunkFile();
	boost::filesystem::create_directories(GetIagdFolder() + L"data\\");
	const std::wstring temporary = filename + L".tmp";
	std::wofstream file(temporary, std::ios::trunc);
	if (!file) {
		LogToFile(LogLevel::WARNING, L"Could not write native Junk marks: " + temporary);
		return;
	}
	file << L"pid=" << GetCurrentProcessId() << L"\n";
	for (const auto id : m_nativeJunkIds) {
		file << id << L"\n";
	}
	file.close();
	if (!MoveFileExW(temporary.c_str(), filename.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		LogToFile(LogLevel::WARNING, L"Could not replace native Junk marks: " + filename);
	}
}

bool InventorySack_AddItem::SellNativeJunk(GAME::PlayerInventoryCtrl* inventory) {
	LoadNativeJunkIds();
	if (!m_marketOpen || m_marketEngine == nullptr || m_vendorId == 0 || inventory == nullptr || m_nativeJunkIds.empty()) {
		return false;
	}
	if (dll_GameEngine_PlayerSaleRequest == nullptr || dll_PlayerInventoryCtrl_RemoveItem == nullptr ||
		dll_InventorySack_ContainsItem == nullptr || fnPlayerInventoryCtrl_GetNumberOfSacks == nullptr ||
		fnPlayerInventoryCtrl_GetSack == nullptr) {
		LogToFile(LogLevel::WARNING, L"Junk sale refused: required vendor or inventory export is unavailable");
		return true;
	}

	GAME::Player* player = fnGetMainPlayer(m_marketEngine);
	GAME::ControllerPlayer* controller = m_controllerCharacter != nullptr
		? reinterpret_cast<GAME::ControllerPlayer*>(m_controllerCharacter)
		: (player == nullptr || fnPlayerGetController == nullptr ? nullptr : fnPlayerGetController(player));
	if (controller == nullptr) {
		LogToFile(LogLevel::WARNING, L"Junk sale refused: player controller is unavailable");
		return true;
	}

	const unsigned int sackCount = fnPlayerInventoryCtrl_GetNumberOfSacks(inventory);
	if (sackCount == 0 || sackCount > 64) {
		return true;
	}

	std::vector<unsigned int> candidates;
	std::vector<unsigned int> stale;
	for (const auto id : m_nativeJunkIds) {
		bool found = false;
		for (unsigned int i = 0; i < sackCount && !found; ++i) {
			GAME::InventorySack* sack = fnPlayerInventoryCtrl_GetSack(inventory, static_cast<int>(i));
			found = sack != nullptr && dll_InventorySack_ContainsItem(sack, id);
		}
		if (found) candidates.push_back(id);
		else stale.push_back(id);
	}
	for (const auto id : stale) m_nativeJunkIds.erase(id);

	if (candidates.empty()) {
		SaveNativeJunkIds();
		return false;
	}

	int sold = 0;
	for (const auto id : candidates) {
		LogToFile(LogLevel::INFO, L"Selling native Junk item " + std::to_wstring(id) + L" to vendor " + std::to_wstring(m_vendorId));
		if (!dll_GameEngine_PlayerSaleRequest(m_marketEngine, m_vendorId, id, false)) {
			LogToFile(LogLevel::WARNING, L"Vendor refused native Junk item; stopping sale");
			break;
		}

		if (!dll_PlayerInventoryCtrl_RemoveItem(inventory, id, true)) {
			LogToFile(LogLevel::WARNING, L"Vendor paid but RemoveItem failed; stopping sale to avoid a duplicate sale");
			break;
		}

		if (dll_ControllerCharacter_SendRemoveItemFromInventory != nullptr) {
			dll_ControllerCharacter_SendRemoveItemFromInventory(controller, id);
		}
		m_nativeJunkIds.erase(id);
		sold++;
	}

	SaveNativeJunkIds();
	if (sold > 0) {
		DisplayMessage(std::to_wstring(sold) + L" Junk item(s) sold", L"By Item Assistant");
	}
	return true;
}

void __fastcall InventorySack_AddItem::Hooked_GameEngine_OpenMarket(void* This, unsigned int who, int* marketType, void* at) {
	m_marketOpen = true;
	m_vendorId = who;
	m_marketEngine = static_cast<GAME::GameEngine*>(This);
	if (dll_GameEngine_OpenMarket != nullptr) {
		dll_GameEngine_OpenMarket(This, who, marketType, at);
	}
}

void __fastcall InventorySack_AddItem::Hooked_GameEngine_CloseMarket(void* This, unsigned int who) {
	m_marketOpen = false;
	m_vendorId = 0;
	m_marketEngine = nullptr;
	if (dll_GameEngine_CloseMarket != nullptr) {
		dll_GameEngine_CloseMarket(This, who);
	}
}

void __fastcall InventorySack_AddItem::Hooked_PlayerInventoryCtrl_DepositReagents(void* This) {
	if (m_isActive && m_marketOpen) {
		GAME::Player* player = m_marketEngine == nullptr ? nullptr : fnGetMainPlayer(m_marketEngine);
		GAME::ControllerPlayer* controller = m_controllerCharacter != nullptr
			? reinterpret_cast<GAME::ControllerPlayer*>(m_controllerCharacter)
			: (player == nullptr || fnPlayerGetController == nullptr ? nullptr : fnPlayerGetController(player));
		GAME::PlayerInventoryCtrl* inventory = controller == nullptr || fnControllerGetInventoryCtrl == nullptr
			? nullptr : fnControllerGetInventoryCtrl(controller);
		if (SellNativeJunk(inventory)) {
			return;
		}
	}

	if (dll_PlayerInventoryCtrl_DepositReagents != nullptr) {
		dll_PlayerInventoryCtrl_DepositReagents(This);
	}
}

bool __fastcall InventorySack_AddItem::Hooked_InventorySack_Sort(void* This, int sortMode) {
	// Auto Sort is also exposed by the game's Player inventory UI.  The direct
	// bag -> IA trigger is intentionally resolved against PlayerInventoryCtrl's
	// sacks, not IA's transfer-stash vector.
	if (m_isActive && !m_marketOpen) {
		GAME::GameEngine* gameEngine = fnGetGameEngine();
		GAME::Player* player = gameEngine == nullptr ? nullptr : fnGetMainPlayer(gameEngine);
		GAME::ControllerPlayer* controller = m_controllerCharacter != nullptr
			? reinterpret_cast<GAME::ControllerPlayer*>(m_controllerCharacter)
			: (player == nullptr || fnPlayerGetController == nullptr ? nullptr : fnPlayerGetController(player));
		GAME::PlayerInventoryCtrl* inventory = controller == nullptr || fnControllerGetInventoryCtrl == nullptr
			? nullptr : fnControllerGetInventoryCtrl(controller);
		int matchingPlayerSack = -1;
		if (inventory != nullptr && fnPlayerInventoryCtrl_GetNumberOfSacks != nullptr &&
			fnPlayerInventoryCtrl_GetSack != nullptr) {
			unsigned int sackCount = fnPlayerInventoryCtrl_GetNumberOfSacks(inventory);
			if (sackCount <= 64) {
				for (unsigned int i = 0; i < sackCount; ++i) {
					GAME::InventorySack* sack = fnPlayerInventoryCtrl_GetSack(inventory, static_cast<int>(i));
					if (sack == reinterpret_cast<GAME::InventorySack*>(This)) {
						matchingPlayerSack = static_cast<int>(i);
					}
				}
			}
		}
		// Sack 1 is the right-hand player-bag Auto Sort control in Grim Dawn's
		// current Player inventory layout.
		if (matchingPlayerSack == 1) {
			int moved = MovePlayerBagsToIA(gameEngine, inventory);
			if (moved > 0) {
				return true;
			}
		}
	}

	return SortInventorySack == nullptr ? false : SortInventorySack(This, sortMode);
}

void* __fastcall InventorySack_AddItem::Hooked_ControllerCharacter_GetEquipmentCtrl(void* This) {
	void* result = dll_ControllerCharacter_GetEquipmentCtrl == nullptr
		? nullptr : dll_ControllerCharacter_GetEquipmentCtrl(This);
	if (This != nullptr && This != m_controllerCharacter) {
		m_controllerCharacter = This;
		LogToFile(LogLevel::INFO, L"Captured live ControllerCharacter from GetEquipmentCtrl");
	}
	return result;
}

/// <summary>
/// Returns the inventory sack to deposit items to (or nullptr on failure)
/// </summary>
/// <param name="gameEngine"></param>
/// <returns></returns>
GAME::InventorySack* InventorySack_AddItem::GetSackToDepositTo(GAME::GameEngine* gameEngine) {
	if (gameEngine == nullptr) {
		return nullptr;
	}

	void* realPtr = fnGetPlayerTransfer(gameEngine);
	if (realPtr == nullptr) {
		return nullptr;
	}

	auto sacks = static_cast<std::vector<GAME::InventorySack*>*>(realPtr);

	// Not enough sacks to even support running IA
	if (sacks->size() < 2) {
		return nullptr;
	}


	// Determine the correct stash sack..
	size_t toDepositTo;
	if (m_stashTabDepositTo == 0) {
		toDepositTo = sacks->size() - 2;
	}
	else {
		// m_stashTabLootFrom is index from 1, we never want to go <0 nor >= size.
		toDepositTo = max(0,
			min(sacks->size() - 1, m_stashTabDepositTo - 1)
		);
	}

	return sacks->at(toDepositTo);
}


/// <summary>
/// GameEngine::Update() hook
/// Responsible for moving items from .csv and back into the game.
/// </summary>
/// <param name="This"></param>
/// <param name="notUsed"></param>
/// <param name="s"></param>
/// <param name="f"></param>
/// <param name="b"></param>
/// <param name="f2"></param>
/// <returns></returns>
void* __fastcall InventorySack_AddItem::Hooked_GameEngine_Update(void* This, int v) {
	try {
		// Diagnostics: catch the exact frame the world dies under us. Only writes on
		// a transition, and is deliberately outside the m_isActive gate so the log is
		// comparable between "IA attached and active" and "IA attached but idle".
		fnLogWorldStateTransition((GAME::GameEngine*)This, L"GameEngine::Update");

		// IA not running? Continue
		if (!m_isActive) {
			//LogToFile(L"Debug: NotActive");
			return dll_GameEngine_Update(This, v);
		}

		// If the game is not in a a "ready state", just continue.
		if (!fnIsWorldAlive((GAME::GameEngine*)This)) {
			//LogToFile(L"Debug: NotReady");
			m_isTransferStashOpen = false; // Just to be on the safe side
			return dll_GameEngine_Update(This, v);
		}

		// No need to check *constantly* (at least not until we get a thread here)
		if (++m_gameUpdateIterationsRun < 30) {
			//LogToFile(L"Debug: NotIteration");
			return dll_GameEngine_Update(This, v);
		}

		m_gameUpdateIterationsRun = 0;

		auto engine = fnGetEngine();
		if (engine == nullptr) {
			LogToFile(LogLevel::INFO, L"Debug: NoEngine");
			return dll_GameEngine_Update(This, v);
		}

		GAME::GameInfo* gameInfo = fnGetGameInfo(engine);
		if (gameInfo == nullptr) {
			// This fires during exit-to-menu teardown: GameInfo is destroyed while
			// GameEngine is still ticking. Until recently this path returned without
			// calling the original Update(), which dropped the game's update tick.
			//
			// The world can still report itself alive at this point, so the transition check at the
			// top of this function has not necessarily fired. Drop the cached mod name here too, or a
			// polling thread keeps working the departing world's queue folder.
			GameContext::Invalidate();
			LogToFile(LogLevel::WARNING,
				L"GameInfo is null (world tearing down?), skipping IA work this tick. msSinceLastAddItem="
				+ std::to_wstring(fnMsSinceLastAddItem()));
			return dll_GameEngine_Update(This, v);
		}

		// The one place the mod name and hardcore flag are read out of the game. This runs on the
		// game's own thread, every 30 ticks, whether or not the transfer stash is open, which is what
		// keeps a current copy available to the two polling threads. A no-op once the world is known.
		std::wstring modName;
		bool isHardcore = false;
		if (!GameContext::Resolve(gameInfo, modName, isHardcore)) {
			return dll_GameEngine_Update(This, v);
		}

		// Player-bag transfers do not require the transfer stash to be open. They are kept separate from
		// the older stash queue so a full personal inventory never deletes an IA item or blocks stash work.
		{
			boost::lock_guard<boost::mutex> guard(m_mutex);
			if (!m_bagQueue.empty()) {
				std::wstring completedFolder = GetBagFolder(modName, isHardcore, L"completed-bags");
				for (auto it = m_bagQueue.begin(); it != m_bagQueue.end(); ++it) {
					const bool accepted = ProcessBagItem((GAME::GameEngine*)This, *it);
					if (accepted) {
						const std::wstring target = completedFolder + L"\\" + boost::filesystem::path(*it).filename().wstring();
						if (!MoveFile(it->c_str(), target.c_str())) {
							LogToFile(LogLevel::WARNING, L"Failed moving player-bag queue file to acknowledgement folder: " + *it);
						}
						else {
							LogToFile(LogLevel::INFO, L"Player-bag transfer accepted: " + target);
						}
					}
					else {
						// Leave the original file in outgoing-bags. The polling thread will retry on a later game
						// update, which means freeing one bag slot automatically drains the remaining batch.
						LogToFile(LogLevel::INFO, L"Player bags are full or unavailable; transfer remains queued: " + *it);
					}
				}
				m_bagQueue.clear();
			}
		}

		if (m_isTransferStashOpen) {
			void* sackPtr = GetSackToDepositTo((GAME::GameEngine*)This);
			if (sackPtr != nullptr) {
				GAME::Rect itemPosition;
				boost::lock_guard<boost::mutex> guard(m_mutex);


				bool success = false;
				std::wstring targetFolder = GetFolderToMoveTo(modName, isHardcore);
				for (auto it = m_depositQueue.begin(); it != m_depositQueue.end(); ++it) {
					std::wstring targetFile = targetFolder + L"\\" + randomFilename();
					LogToFile(LogLevel::INFO, L"Handling file " + *it);

					GAME::ItemReplicaInfo* replica = ReadReplicaInfo(*it);
					if (replica != nullptr) {
						//LogToFile(L"DEBUG Creating item from replica..");
						try {
							auto item = fnCreateItem(replica);
							//LogToFile(L"DEBUG Adding item to inventory sack..");
							if (item == nullptr) {
								LogToFile(LogLevel::FATAL, L"Error creating item, re-depositing back into IA. (Mod item transferred into vanilla?)");
								targetFile = m_storageFolder + randomFilename();
								LogToFile(LogLevel::INFO, L"Moving to " + targetFile);
							}
							else {
								if (dll_InventorySack_FindNextPosition(sackPtr, item, &itemPosition, true)) {
									dll_InventorySack_AddItem_Vec2(sackPtr, (void*)&itemPosition, item, false);
									LogToFile(LogLevel::INFO, L"Item deposited, moving to " + targetFile);
									success = true;
								}
								else {
									targetFile = m_storageFolder + randomFilename();
									LogToFile(LogLevel::INFO, L"Target sack is full, re-depositing to IA as " + targetFile);
								}

							}
						}
						catch (...) {
							LogToFile(LogLevel::FATAL, L"Invalid item, moving to " + targetFile);
						}
						delete replica;

					}
					else {
						LogToFile(LogLevel::INFO, L"Invalid item, moving to " + targetFile);
					}

					if (!MoveFile(it->c_str(), targetFile.c_str())) {
						LogToFile(LogLevel::WARNING, L"Failed moving file: \"" + *it + L"\" to \"" + targetFile + L"\", error code: " + std::to_wstring(GetLastError()));
					}
				}


				if (!m_depositQueue.empty()) {
					if (success) {
						if (m_depositQueue.size() == 1) {
							DisplayMessage(L"An item was deposited", L"By Item Assistant");
						}
						else {
							DisplayMessage(std::to_wstring(m_depositQueue.size()) + L" items were deposited", L"By Item Assistant");
						}

						// Sort the items, as we've deposited them all in position 1,1
						SortInventorySack(sackPtr, 1);
					}
					else {
						DisplayMessage(L"Could not transfer item, moved back to IA.", L"By Item Assistant");

					}
					// fnPlayDropSound(item);


					// We got a mutex so this is safe (and if it wasn't, we'd just loot it next time IA starts)
					m_depositQueue.clear();
				}

			}
		}
	}
	catch (std::exception& ex) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wide = converter.from_bytes(ex.what());
		LogToFile(LogLevel::FATAL, L"Error parsing in InventorySack_AddItem::Hooked_GameEngine_Update.. " + wide);
	}
	catch (...) {
		LogToFile(LogLevel::FATAL, L"Error parsing in InventorySack_AddItem::Hooked_GameEngine_Update.. (triple-dot)");
	}

	return dll_GameEngine_Update(This, v);
}

/// <summary>
/// Looks for new files added to the current itemqueue\outgoing\sc-hc\modname folder, to deposit into the game.
/// Files found are added to a std::set locked by a mutex, read by GameEngine::update()
/// </summary>
/// <param name=""></param>
void InventorySack_AddItem::ThreadMain(void*) {
	LogToFile(LogLevel::INFO, L"IA is running, starting deposit listener..");
	try {
		std::set<std::wstring> knownFiles = std::set<std::wstring>();

		while (m_isActive) {
			Sleep(500);


			std::wstring modName;
			bool isHardcore = false;
			if (!GameContext::TryGet(modName, isHardcore)) {
				// No world loaded. Nothing can be deposited into one that is not there, and
				// guessing the folder means reading another character's queue.
				continue;
			}

			std::wstring folder = GetFolderToLootFrom(modName, isHardcore);
			std::wstring bagFolder = GetBagFolder(modName, isHardcore, L"outgoing-bags");
			// LogToFile(std::wstring(L"Looking for files in dir: ") + folder);

			for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(folder), {})) {
				auto filename = std::wstring(entry.path().c_str());
				if (knownFiles.find(filename) == knownFiles.end()) {
					boost::lock_guard<boost::mutex> guard(m_mutex);

					if (boost::algorithm::ends_with(filename, ".csv")) {
						LogToFile(LogLevel::INFO, std::wstring(L"Found file: ") + std::wstring(entry.path().c_str()));
						m_depositQueue.insert(filename);
					}
					else {
						LogToFile(LogLevel::INFO, std::wstring(L"Ignoring file: ") + std::wstring(entry.path().c_str()));
					}
					knownFiles.insert(filename);
				}
			}

			// Unlike the old stash queue, a bag file that cannot fit must be discovered again after the
			// player frees a slot. The set itself de-duplicates files that are already waiting this tick.
			for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(bagFolder), {})) {
				auto filename = std::wstring(entry.path().c_str());
				boost::lock_guard<boost::mutex> guard(m_mutex);

				if (boost::algorithm::ends_with(filename, ".csv") && m_bagQueue.insert(filename).second) {
					LogToFile(LogLevel::INFO, std::wstring(L"Found player-bag file: ") + filename);
				}
			}
		}
	}
	catch (std::exception& ex) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
		std::wstring wide = converter.from_bytes(ex.what());
		LogToFile(LogLevel::FATAL, L"Error parsing in InventorySack_AddItem::ThreadMain.. " + wide);
	}
	catch (...) {
		LogToFile(LogLevel::FATAL, L"Error parsing in InventorySack_AddItem::ThreadMain.. (triple-dot)");
	}
	LogToFile(LogLevel::INFO, L"Stopping deposit listener..");
}
