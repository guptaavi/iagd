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
	static std::set<std::wstring> m_bagQueue;
	static std::set<unsigned int> m_nativeJunkIds;
	static bool m_nativeJunkLoaded;
	static bool m_marketOpen;
	static unsigned int m_vendorId;
	static GAME::GameEngine* m_marketEngine;
	static void* m_controllerCharacter;
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
	typedef void (__thiscall* GameEngine_OpenMarket)(void*, unsigned int, int*, void*);
	typedef void (__thiscall* GameEngine_CloseMarket)(void*, unsigned int);
	typedef void (__thiscall* PlayerInventoryCtrl_DepositReagents)(void*);
	typedef bool (__thiscall* GameEngine_PlayerSaleRequest)(void*, unsigned int, unsigned int, bool);
	typedef bool (__thiscall* PlayerInventoryCtrl_RemoveItem)(void*, unsigned int, bool);
	typedef void (__thiscall* ControllerCharacter_SendRemoveItemFromInventory)(void*, unsigned int);
	typedef void* (__thiscall* ControllerCharacter_GetEquipmentCtrl)(void*);
	typedef bool (__thiscall* InventorySack_ContainsItem)(void*, unsigned int);


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
	static GameEngine_OpenMarket dll_GameEngine_OpenMarket;
	static GameEngine_CloseMarket dll_GameEngine_CloseMarket;
	static PlayerInventoryCtrl_DepositReagents dll_PlayerInventoryCtrl_DepositReagents;
	static GameEngine_PlayerSaleRequest dll_GameEngine_PlayerSaleRequest;
	static PlayerInventoryCtrl_RemoveItem dll_PlayerInventoryCtrl_RemoveItem;
	static ControllerCharacter_SendRemoveItemFromInventory dll_ControllerCharacter_SendRemoveItemFromInventory;
	static ControllerCharacter_GetEquipmentCtrl dll_ControllerCharacter_GetEquipmentCtrl;
	static InventorySack_ContainsItem dll_InventorySack_ContainsItem;


	// Hook proxy methods
	static void* __fastcall Hooked_GameInfo_GameInfo_Param(void* This, void* info);
	static void* __fastcall Hooked_InventorySack_AddItem_Drop(void* This, GAME::Item* item, bool findPosition, bool SkipPlaySound);
	static void* __fastcall Hooked_InventorySack_AddItem_Vec2(void* This, void*, GAME::Item* item, bool SkipPlaySound);
	static void* __fastcall Hooked_InventorySack_SetTransferOpen(void* This, bool isOpen);
	static void* __fastcall Hooked_GameEngine_Update(void* This, int v);
	static void __fastcall Hooked_GameEngine_OpenMarket(void* This, unsigned int who, int* marketType, void* at);
	static void __fastcall Hooked_GameEngine_CloseMarket(void* This, unsigned int who);
	static void __fastcall Hooked_PlayerInventoryCtrl_DepositReagents(void* This);
	static bool __fastcall Hooked_InventorySack_Sort(void* This, int sortMode);
	static void* __fastcall Hooked_ControllerCharacter_GetEquipmentCtrl(void* This);


	// Helper/internal methods
	static bool HandleItem(void* stash, GAME::Item* item);
	static bool Persist(GAME::ItemReplicaInfo replicaInfo, bool isHardcore, std::wstring mod, const std::vector<GAME::GameTextLine>& gameTextLines);
	static void DisplayMessage(std::wstring, std::wstring);
	static bool IsRelevant(const GAME::ItemReplicaInfo& item);
	static bool IsSackToLootFrom(void* stash, GAME::GameEngine* gameEngine);
	static GAME::InventorySack* GetSackToDepositTo(GAME::GameEngine* gameEngine);
	static GAME::ItemReplicaInfo* ReadReplicaInfo(const std::wstring& filename);
	static bool ProcessBagItem(GAME::GameEngine* gameEngine, const std::wstring& filename);
	static int MovePlayerBagsToIA(GAME::GameEngine* gameEngine, GAME::PlayerInventoryCtrl* inventory);
	static void LoadNativeJunkIds();
	static void SaveNativeJunkIds();
	static bool SellNativeJunk(GAME::PlayerInventoryCtrl* inventory);


	



	static void ThreadMain(void*);
};
