#include "SettingsReader.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/optional/optional.hpp>
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include "Logger.h"

std::wstring GetIagdFolder();

namespace {

	/// <summary>
	/// A key name as a player would write it, to a virtual-key code.
	///
	/// Deliberately a short list rather than a full keyboard map: a hotkey that opens a
	/// window over a game wants to be a key the game does not use, and the keys that
	/// qualify are the function keys and the navigation block. Single letters and digits
	/// are handled separately below, for anyone who wants one anyway.
	/// </summary>
	int NamedVirtualKey(const std::wstring& name) {
		static const struct { const wchar_t* name; int key; } table[] = {
			{ L"F1", VK_F1 }, { L"F2", VK_F2 }, { L"F3", VK_F3 }, { L"F4", VK_F4 },
			{ L"F5", VK_F5 }, { L"F6", VK_F6 }, { L"F7", VK_F7 }, { L"F8", VK_F8 },
			{ L"F9", VK_F9 }, { L"F10", VK_F10 }, { L"F11", VK_F11 }, { L"F12", VK_F12 },
			{ L"INSERT", VK_INSERT }, { L"DELETE", VK_DELETE }, { L"HOME", VK_HOME },
			{ L"END", VK_END }, { L"PAGEUP", VK_PRIOR }, { L"PAGEDOWN", VK_NEXT },
			{ L"PAUSE", VK_PAUSE }, { L"SCROLLLOCK", VK_SCROLL }, { L"TAB", VK_TAB },
			{ L"BACKQUOTE", VK_OEM_3 }, { L"TILDE", VK_OEM_3 },
		};

		for (const auto& entry : table) {
			if (name == entry.name) {
				return entry.key;
			}
		}

		// A single letter or digit is its own virtual-key code on every layout Windows
		// reports, which is why these do not need a table.
		if (name.size() == 1 && ((name[0] >= L'A' && name[0] <= L'Z') || (name[0] >= L'0' && name[0] <= L'9'))) {
			return (int)name[0];
		}

		return 0;
	}
}



int SettingsReader::GetStashTabToLootFrom() {
	boost::property_tree::wptree loadPtreeRoot;

	const auto settingsJson = GetIagdFolder() + L"settings.json";
	std::wifstream json(settingsJson);

	boost::property_tree::read_json(json, loadPtreeRoot);
	auto child = loadPtreeRoot.get_child_optional(L"local.stashToLootFrom");
	if (!child)
	{
		LogToFile(LogLevel::WARNING, L"No \"loot from\" configuration found, defaulting to last stash tab");
		return 0;
	}

	const int stashToLootFrom = loadPtreeRoot.get<int>(L"local.stashToLootFrom");


	if (stashToLootFrom == 0) {
		LogToFile(LogLevel::INFO, L"Configured to loot from last stash tab");

	}
	else {
		LogToFile(LogLevel::INFO, L"Configured to loot from tab: " + std::to_wstring(stashToLootFrom));
	}

	return stashToLootFrom;
}

int SettingsReader::GetStashTabToDepositTo() {
	boost::property_tree::wptree loadPtreeRoot;

	const auto settingsJson = GetIagdFolder() + L"settings.json";
	std::wifstream json(settingsJson);

	boost::property_tree::read_json(json, loadPtreeRoot);
	auto child = loadPtreeRoot.get_child_optional(L"local.stashToDepositTo");
	if (!child)
	{
		LogToFile(LogLevel::WARNING, L"No \"deposit to\" configuration found, defaulting to second-to-last stash tab");
		return 0;
	}

	const int stashToDepositTo = loadPtreeRoot.get<int>(L"local.stashToDepositTo");


	if (stashToDepositTo == 0) {
		LogToFile(LogLevel::INFO, L"Configured to deposit to last stash tab");

	}
	else {
		LogToFile(LogLevel::INFO, L"Configured to deposit to tab: " + std::to_wstring(stashToDepositTo));
	}

	return stashToDepositTo;
}




bool SettingsReader::GetIsGrimDawnParsed() {
	boost::property_tree::wptree loadPtreeRoot;

	const auto settingsJson = GetIagdFolder() + L"settings.json";
	std::wifstream json(settingsJson);


	boost::property_tree::read_json(json, loadPtreeRoot);
	auto child = loadPtreeRoot.get_child_optional(L"local.isGrimDawnParsed");
	if (!child)
	{
		LogToFile(LogLevel::WARNING, L"GrimDawnParsed: No configuration found, defaulting to NOT parsed");
		return false;
	}

	const bool isGdParsed = loadPtreeRoot.get<bool>(L"local.isGrimDawnParsed");
	LogToFile(LogLevel::INFO, std::wstring(L"Grim Dawn parsed: ") + (isGdParsed ? L"True" : L"False"));

	return isGdParsed;
}

bool SettingsReader::GetIsOverlayEnabled(bool defaultValue) {
	boost::property_tree::wptree loadPtreeRoot;

	const auto settingsJson = GetIagdFolder() + L"settings.json";
	std::wifstream json(settingsJson);

	boost::property_tree::read_json(json, loadPtreeRoot);
	auto child = loadPtreeRoot.get_child_optional(L"local.overlayEnabled");
	if (!child) {
		return defaultValue;
	}

	const bool enabled = loadPtreeRoot.get<bool>(L"local.overlayEnabled");
	LogToFile(LogLevel::INFO, std::wstring(L"In-game browser: settings.json says ")
		+ (enabled ? L"enabled" : L"disabled"));

	return enabled;
}

bool SettingsReader::GetIsDarkMode() {
	boost::property_tree::wptree loadPtreeRoot;

	const auto settingsJson = GetIagdFolder() + L"settings.json";
	std::wifstream json(settingsJson);

	boost::property_tree::read_json(json, loadPtreeRoot);
	auto child = loadPtreeRoot.get_child_optional(L"persistent.darkMode");
	if (!child) {
		return true;
	}

	return loadPtreeRoot.get<bool>(L"persistent.darkMode");
}

int SettingsReader::GetOverlayHotkey() {
	boost::property_tree::wptree loadPtreeRoot;

	const auto settingsJson = GetIagdFolder() + L"settings.json";
	std::wifstream json(settingsJson);

	boost::property_tree::read_json(json, loadPtreeRoot);
	auto child = loadPtreeRoot.get_child_optional(L"local.overlayHotkey");
	if (!child) {
		// Not worth a warning: nobody has to configure this, and the default is a key the
		// game does not use.
		return VK_F9;
	}

	std::wstring configured = loadPtreeRoot.get<std::wstring>(L"local.overlayHotkey");
	configured.erase(std::remove_if(configured.begin(), configured.end(),
		[](wchar_t c) { return std::iswspace(c) != 0; }), configured.end());
	std::transform(configured.begin(), configured.end(), configured.begin(),
		[](wchar_t c) { return (wchar_t)std::towupper(c); });

	if (configured.empty()) {
		return VK_F9;
	}

	// A number is taken as a virtual-key code directly, so a key this build has no name
	// for can still be configured without waiting for a new build.
	if (configured.find_first_not_of(L"0123456789") == std::wstring::npos) {
		const int code = std::stoi(configured);
		if (code > 0 && code <= 0xFF) {
			LogToFile(LogLevel::INFO, L"Overlay hotkey: virtual-key code " + std::to_wstring(code));
			return code;
		}
	}
	else {
		const int named = NamedVirtualKey(configured);
		if (named != 0) {
			LogToFile(LogLevel::INFO, L"Overlay hotkey: " + configured);
			return named;
		}
	}

	LogToFile(LogLevel::WARNING, L"Overlay hotkey: \"" + configured
		+ L"\" is not a key this build recognises, falling back to F9.");
	return VK_F9;
}

bool SettingsReader::GetIsRunningInWine() {
	boost::property_tree::wptree loadPtreeRoot;

	const auto settingsJson = GetIagdFolder() + L"settings.json";
	std::wifstream json(settingsJson);

	boost::property_tree::read_json(json, loadPtreeRoot);
	auto child = loadPtreeRoot.get_child_optional(L"persistent.isRunningInWine");
	if (!child)
	{
		LogToFile(LogLevel::WARNING, L"RunningInWine: No configuration found, defaulting to false");
		return false;
	}

	const bool isWine = loadPtreeRoot.get<bool>(L"persistent.isRunningInWine");
	LogToFile(LogLevel::INFO, std::wstring(L"Running in Wine: ") + (isWine ? L"True" : L"False"));

	return isWine;
}
