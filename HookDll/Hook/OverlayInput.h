#pragma once

#include "BaseMethodHook.h"

/// <summary>
/// One window message on its way to the overlay, captured on the game's window thread and
/// replayed on the render thread.
///
/// Copied rather than handled where it arrives because RmlUi lives entirely on the render
/// thread and is not thread safe, and because handling it on the window thread would mean
/// the game's message pump waiting on the overlay's layout.
/// </summary>
struct OverlayWindowMessage {
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
};

/// <summary>
/// Takes mouse and keyboard away from the game while the overlay is open.
///
/// Grim Dawn reads input through DINPUT8 and GetCursorPos rather than through window
/// messages, so subclassing the window would not stop the character running around while
/// the player types in a search box. What it does instead is hook the game's own input
/// layer, which DirectInput.dll exports by name:
///
///   ?GetNumKeyEvents@DirectInputDevice@GAME@@UEBAHXZ
///   ?GetNumMouseEvents@DirectInputDevice@GAME@@UEBAHXZ
///   ?IsButtonDown@DirectInputDevice@GAME@@UEBA_NW4Button@InputDevice@2@@Z
///
/// While the overlay is open these report an empty queue and no held buttons, so the game
/// sees no input at all rather than being asked to ignore input it can see. While the
/// overlay is closed they pass straight through and the game behaves exactly as it does
/// without this hook.
///
/// Deliberately only these three. GetKeyEvent, GetMouseEvent and GetCursorPosition return
/// class types by value whose layouts are not published, and reading them would mean
/// depending on a reverse-engineered struct that a game patch can silently change.
/// Suppression does not need them: an empty queue is never indexed. The overlay's own
/// input comes from window messages instead, which is what RmlUi's Win32 platform backend
/// consumes anyway.
/// </summary>
class OverlayInput : public BaseMethodHook {
public:
    OverlayInput();
    OverlayInput(DataQueue* dataQueue, HANDLE hEvent);

    void EnableHook() override;
    void DisableHook() override;

    /// <summary>
    /// Turns suppression on and off. Called when the overlay opens and closes.
    /// Safe to call from any thread; the flag is read on the game's input thread.
    /// </summary>
    static void SetSuppressing(bool suppress);

    /// Whether the game's input is currently being withheld.
    static bool IsSuppressing();

    /// <summary>
    /// The game's own top-level window, or null before the subclass is installed.
    /// Everything the overlay needs a window for -- messages, the mouse cursor, the client
    /// size it maps mouse coordinates through -- comes from this one.
    /// </summary>
    static HWND Window();

    /// <summary>
    /// Takes the next input message the overlay has not seen yet. False when there are
    /// none left. Call from the render thread only.
    /// </summary>
    static bool PopMessage(OverlayWindowMessage& out);

    /// <summary>
    /// How many times the open/close key has been pressed since this was last called, and
    /// resets the count.
    ///
    /// A count rather than a flag so a press is never lost between frames, and never
    /// applied twice. Auto-repeat is not counted: holding the key opens the overlay once.
    /// </summary>
    static unsigned int TakeTogglePresses();

    /// <summary>
    /// The virtual-key code that opens and closes the overlay. Read once at attach from
    /// the client's settings.json, defaulting to F9.
    /// </summary>
    static void SetToggleKey(int virtualKey);
    static int ToggleKey();

private:
    /// <summary>
    /// Watches the game's window for the messages the overlay is driven by.
    ///
    /// The game reads gameplay input through DirectInput rather than through its window
    /// procedure, so this cannot stop the character moving -- that is what the DirectInput
    /// hooks above are for. What it can do is see the events in the form RmlUi's Win32
    /// backend already understands, including the WM_CHAR the keyboard layout has already
    /// turned into a character, which no amount of scancode reading reproduces correctly.
    ///
    /// While the overlay is open the input messages are also swallowed rather than passed
    /// on, so nothing in the game that does read the window queue can act on them either.
    /// </summary>
    static LRESULT CALLBACK Hooked_WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    /// True for the messages the overlay consumes: mouse, keyboard and the IME's
    /// composition traffic. Everything else -- sizing, focus, painting, Alt combinations --
    /// belongs to the game and is never touched.
    static bool IsOverlayInputMessage(UINT message);

    static HWND m_window;
    static WNDPROC m_originalWndProc;

    typedef int (__thiscall* DirectInputDevice_GetNumEvents)(void* This);
    typedef bool (__thiscall* DirectInputDevice_IsButtonDown)(void* This, int button);

    static DirectInputDevice_GetNumEvents dll_GetNumKeyEvents;
    static DirectInputDevice_GetNumEvents dll_GetNumMouseEvents;
    static DirectInputDevice_IsButtonDown dll_IsButtonDown;

    static int __fastcall Hooked_GetNumKeyEvents(void* This);
    static int __fastcall Hooked_GetNumMouseEvents(void* This);
    static bool __fastcall Hooked_IsButtonDown(void* This, int button);

    /// Read on the game's input thread, written when the overlay opens or closes.
    /// volatile rather than atomic to match the rest of this DLL; a torn read of a bool
    /// is not a thing on any platform this runs on, and the worst case is one frame of
    /// stale suppression state either way.
    static volatile bool m_isSuppressing;

    static bool m_isInstalled;

    /// <summary>
    /// Buttons that were already held when suppression ended, and must keep reading as
    /// released until the player actually lets go.
    ///
    /// Without this the game is handed a press whose beginning it never saw: the player
    /// holds the mouse down, the overlay closes, and the game suddenly sees a held button
    /// and acts on it. Suppression has to end for a button on its release, not on the
    /// overlay's close.
    ///
    /// The count is a bound, not the real size of the game's Button enum, which is not
    /// published; anything outside it is passed through unchanged.
    /// </summary>
    static const int ButtonSlots = 512;
    static bool m_isHeldOver[ButtonSlots];

    /// Bumped every time suppression ends. A button whose recorded generation is stale has
    /// not been looked at since, so its held-over state is decided the first time the game
    /// asks about it after the overlay closed.
    static unsigned int m_suppressGeneration;
    static unsigned int m_buttonGeneration[ButtonSlots];

    /// <summary>
    /// Messages waiting for the render thread, oldest first, under m_queueMutex.
    ///
    /// Bounded: a queue that grew without limit would turn "the overlay stopped drawing"
    /// into "the game ran out of memory". Dropping the oldest keeps the most recent input
    /// -- which is what a UI that has fallen behind should show -- and is logged once.
    /// </summary>
    static const size_t MaxQueuedMessages = 512;
};
