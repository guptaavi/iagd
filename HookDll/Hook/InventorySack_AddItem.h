#pragma once
#include <set>

#include "DataQueue.h"
#include "BaseMethodHook.h"
#include "GetPrivateStash.h"
#include "GrimTypes.h"
#include "SettingsReader.h"

class InventorySack_AddItem : public BaseMethodHook {
public:
	InventorySack_AddItem();
	InventorySack_AddItem(DataQueue* dataQueue, HANDLE hEvent);
	void EnableHook() override;
	void DisableHook() override;

	/// <summary>
	/// If the IA client is actively running (if not: Just disable all extra functionality)
	/// </summary>
	/// <param name="isActive"></param>
	void SetActive(bool isActive);

	/// <summary>
	/// Whether the player currently has the transfer stash open.
	///
	/// The in-game browser needs this: an item can only be created into a stash sack the
	/// game has actually opened, so a transfer requested with it closed is held rather than
	/// attempted. Tracked here because this class already hooks InventorySack::SetTransferOpen.
	/// </summary>
	static bool IsTransferStashOpen() { return m_isTransferStashOpen; }

	/// <summary>
	/// The stash sack the client is configured to deposit into, or null when the game is not
	/// in a state to have one. GAME THREAD ONLY.
	///
	/// Public because the in-game browser places items into the same sack, by the same rule,
	/// and two answers to "which stash tab" would be one too many.
	/// </summary>
	static GAME::InventorySack* GetSackToDepositTo(GAME::GameEngine* gameEngine);

	/// <summary>
	/// The game's own "find a free slot" and "put it there" calls, as resolved by this
	/// class at attach. Exposed so the in-game browser places an item through exactly the
	/// path the client's deposit already uses, rather than resolving its own copies that
	/// could drift out of step after a game patch. Null until EnableHook has run.
	/// </summary>
	static bool PlaceInSack(void* sack, GAME::Item* item, GAME::Rect* position) {
		if (dll_InventorySack_FindNextPosition == nullptr || dll_InventorySack_AddItem_Vec2 == nullptr) {
			return false;
		}

		if (!dll_InventorySack_FindNextPosition(sack, item, position, true)) {
			return false;
		}

		dll_InventorySack_AddItem_Vec2(sack, (void*)position, item, false);
		return true;
	}

	/// Whether the two calls above resolved. False means no transfer can be attempted.
	static bool CanPlaceItems() {
		return dll_InventorySack_FindNextPosition != nullptr && dll_InventorySack_AddItem_Vec2 != nullptr;
	}

	/// Shows a message in the game's own cinematic-text banner, rate limited to one every
	/// three seconds. The in-game browser reports its transfers the same way the client's
	/// deposit does, so the player sees one kind of message rather than two.
	static void ShowMessage(std::wstring header, std::wstring body) { DisplayMessage(header, body); }

private:
	struct Vec2f {
		float x,y;
	};

	static DataQueue* m_dataQueue;
	static HANDLE m_hEvent;
	static GetPrivateStash privateStashHook;
	static int m_stashTabLootFrom;
	static int m_stashTabDepositTo;
	static bool m_isGrimDawnParsed;
	static SettingsReader m_settingsReader;
	static bool m_isActive;
	static int m_gameUpdateIterationsRun;

	static bool m_isTransferStashOpen;
	static std::set<std::wstring> m_depositQueue;
	static boost::mutex m_mutex;


	// Hook typedefs
	typedef int* (__thiscall *GameEngine_GetTransferSack)(void* This, int idx);

	typedef int*(__thiscall *GameInfo_GameInfo_Param)(void*, void* info);
	typedef int*(__thiscall *GameInfo_GameInfo)(void*);

	typedef int*(__thiscall *InventorySack_InventorySack)(void*);
	typedef int*(__thiscall *InventorySack_InventorySackParam)(void*, void* stdstring);

	typedef int*(__thiscall *InventorySack_AddItem_Drop)(void* This, GAME::Item* item, bool findPosition, bool SkipPlaySound);
	typedef int*(__thiscall* InventorySack_AddItem_Vec2)(void* This, void* position, GAME::Item* item, bool SkipPlaySound);
	typedef void* (__thiscall* InventorySack_SetTransferOpen)(void* This, bool isOpen);

	typedef bool(__thiscall* InventorySack_FindNextPosition)(void*, GAME::Item* item, GAME::Rect*, bool unknown);
	typedef bool(__thiscall* GameInfo_GetHardcore)(void*);
	typedef int* (__thiscall* GameEngine_Update)(void* This, int v);
	typedef char* (__thiscall* GameEngine_GetGameInfo)(void* This);


	// Hook variable defs
	static GameInfo_GetHardcore dll_GameInfo_GetHardcore;

	static GameEngine_Update dll_GameEngine_Update;
	static GameInfo_GameInfo_Param dll_GameInfo_GameInfo_Param;
	static GameInfo_GameInfo dll_GameInfo_GameInfo;
	static InventorySack_AddItem_Drop dll_InventorySack_AddItem_Drop;
	static InventorySack_AddItem_Vec2 dll_InventorySack_AddItem_Vec2;
	static InventorySack_SetTransferOpen dll_InventorySack_SetTransferOpen;
	static std::wstring m_storageFolder;
	static ULONGLONG m_lastNotificationTickTime;
	static InventorySack_FindNextPosition dll_InventorySack_FindNextPosition;


	// Hook proxy methods
	static void* __fastcall Hooked_GameInfo_GameInfo_Param(void* This, void* info);
	static void* __fastcall Hooked_InventorySack_AddItem_Drop(void* This, GAME::Item* item, bool findPosition, bool SkipPlaySound);
	static void* __fastcall Hooked_InventorySack_AddItem_Vec2(void* This, void*, GAME::Item* item, bool SkipPlaySound);
	static void* __fastcall Hooked_InventorySack_SetTransferOpen(void* This, bool isOpen);
	static void* __fastcall Hooked_GameEngine_Update(void* This, int v);


	// Helper/internal methods
	static bool HandleItem(void* stash, GAME::Item* item);
	static bool Persist(GAME::ItemReplicaInfo replicaInfo, bool isHardcore, std::wstring mod, const std::vector<GAME::GameTextLine>& gameTextLines);
	static void DisplayMessage(std::wstring, std::wstring);
	static bool IsRelevant(const GAME::ItemReplicaInfo& item);
	static bool IsSackToLootFrom(void* stash, GAME::GameEngine* gameEngine);
	static GAME::ItemReplicaInfo* ReadReplicaInfo(const std::wstring& filename);


	



	static void ThreadMain(void*);
};
