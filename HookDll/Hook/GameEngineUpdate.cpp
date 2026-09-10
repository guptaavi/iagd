#include "stdafx.h"
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include "MessageType.h"
#include "GameEngineUpdate.h"
#include "OverlayTransfer.h"
#include "Logger.h"
#include "Exports.h"


GameEngineUpdate* GameEngineUpdate::g_self;
void GameEngineUpdate::EnableHook() {
	originalMethod = (OriginalMethodPtr)HookGame(
		"?Update@GameEngine@GAME@@QEAAXH@Z",
		HookedMethod,
		m_dataQueue,
		m_hEvent,
		TYPE_GAMEENGINE_UPDATE
	);
}

GameEngineUpdate::GameEngineUpdate(DataQueue* dataQueue, HANDLE hEvent) {
	g_self = this;
	this->m_dataQueue = dataQueue;
	this->m_hEvent = hEvent;
}

GameEngineUpdate::GameEngineUpdate() {
	GameEngineUpdate::m_hEvent = nullptr;
}

void GameEngineUpdate::DisableHook() {
	Unhook((PVOID*)&originalMethod, HookedMethod);
}

void* __fastcall GameEngineUpdate::HookedMethod(void* This, int v) {
	// The game's own update first. An item created before it has ticked would be placed
	// into a sack the game is about to walk, and the capture hook on the same function
	// takes the same care.
	void* r = g_self->originalMethod(This, v);

	try {
		// Cheap when there is nothing queued, which is almost always: one mutex and one
		// empty check. Everything expensive is behind that.
		OverlayTransfer::ProcessPending((GAME::GameEngine*)This);
	}
	catch (const std::exception& ex) {
		LogToFile(LogLevel::FATAL, std::string("Overlay transfer tick: ") + ex.what());
	}
	catch (...) {
		LogToFile(LogLevel::FATAL, "Overlay transfer tick: unknown error.");
	}

	return r;
}
