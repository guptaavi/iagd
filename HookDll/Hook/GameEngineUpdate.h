#pragma once
#include <windows.h>
#include <vector>
#include "DataQueue.h"
#include "BaseMethodHook.h"
#include <string>
#include "GrimTypes.h"

/// <summary>
/// The in-game browser's foothold on the game's own thread.
///
/// The overlay draws on the render thread and searches on a worker, but creating an item and
/// putting it in a stash sack has to happen where the game expects to be called from, and
/// GameEngine::Update is that place.
///
/// This is a second detour on a function InventorySack_AddItem also detours, which is worth
/// knowing about. Detours chains them: this one is attached after that one and detached
/// before it, which the required/optional hook split in dllmain.cpp arranges. The two do not
/// share state -- the capture hook moves items out of the game, this moves them in.
/// </summary>
class GameEngineUpdate : public BaseMethodHook {
public:
	GameEngineUpdate();
	GameEngineUpdate(DataQueue* dataQueue, HANDLE hEvent);
	void EnableHook() override;
	void DisableHook() override;

protected:
	// void GAME::GameEngine::Update(int)
	typedef void* (__thiscall* OriginalMethodPtr)(void* This, int v);
	OriginalMethodPtr originalMethod;

	static GameEngineUpdate* g_self;
	static void* __fastcall HookedMethod(void* This, int v);
};
