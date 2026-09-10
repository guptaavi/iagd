#pragma once
class SettingsReader
{
public:
	int GetStashTabToLootFrom();
	bool GetIsGrimDawnParsed();
	int GetStashTabToDepositTo();
	bool GetIsRunningInWine();

	/// <summary>
	/// The virtual-key code that opens and closes the in-game item browser.
	///
	/// Read from "local.overlayHotkey", which may be a key name ("F9", "Insert", "B") or a
	/// virtual-key code as a number. Defaults to F9: Grim Dawn binds none of the function
	/// keys, so the default cannot take an action away from the player.
	/// </summary>
	int GetOverlayHotkey();

	/// <summary>
	/// Whether the client is in dark mode ("persistent.darkMode"), which the in-game
	/// browser follows so that it does not put a bright panel over a dark game for a
	/// player who has already said which they prefer. Defaults to dark: the overlay is
	/// drawn over Grim Dawn, which is dark whatever the client is set to.
	/// </summary>
	bool GetIsDarkMode();

	/// <summary>
	/// Whether the in-game item browser may install at all ("local.overlayEnabled").
	///
	/// A switch rather than a capability check: the overlay already refuses to run on a
	/// renderer it cannot draw into, and this is for the cases the DLL cannot detect --
	/// a player who simply does not want it, and Wine, where the whole path is unproven.
	/// The caller supplies the default, because what "absent" should mean depends on where
	/// it is being read.
	/// </summary>
	bool GetIsOverlayEnabled(bool defaultValue);
};

