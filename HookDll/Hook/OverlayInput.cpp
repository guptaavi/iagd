#include "stdafx.h"
#include "OverlayInput.h"
#include "Logger.h"
#include "OverlayHost.h"

#include <deque>
#include <mutex>
#include <string>
#include <detours.h>

// Verified against the shipped DirectInput.dll export table (dumpbin /exports).
#define DI_GET_NUM_KEY_EVENTS   "?GetNumKeyEvents@DirectInputDevice@GAME@@UEBAHXZ"
#define DI_GET_NUM_MOUSE_EVENTS "?GetNumMouseEvents@DirectInputDevice@GAME@@UEBAHXZ"
#define DI_IS_BUTTON_DOWN       "?IsButtonDown@DirectInputDevice@GAME@@UEBA_NW4Button@InputDevice@2@@Z"

OverlayInput::DirectInputDevice_GetNumEvents OverlayInput::dll_GetNumKeyEvents = nullptr;
OverlayInput::DirectInputDevice_GetNumEvents OverlayInput::dll_GetNumMouseEvents = nullptr;
OverlayInput::DirectInputDevice_IsButtonDown OverlayInput::dll_IsButtonDown = nullptr;
volatile bool OverlayInput::m_isSuppressing = false;
bool OverlayInput::m_isInstalled = false;
bool OverlayInput::m_isHeldOver[OverlayInput::ButtonSlots] = {};
unsigned int OverlayInput::m_suppressGeneration = 0;
unsigned int OverlayInput::m_buttonGeneration[OverlayInput::ButtonSlots] = {};
HWND OverlayInput::m_window = nullptr;
WNDPROC OverlayInput::m_originalWndProc = nullptr;

namespace {

/// Everything below is touched by the game's window thread and the render thread. Function
/// -local rather than namespace-scope for the reason given in dllmain.cpp: a mutex and a
/// deque are dynamically initialised, and this file's statics are reachable before this
/// translation unit's initialisers have run.
struct MessagePump {
    std::mutex mutex;
    std::deque<OverlayWindowMessage> queued;
    bool hasReportedOverflow = false;
};

MessagePump& pump() {
    static MessagePump instance;
    return instance;
}

/// F9 by default. Grim Dawn binds none of the function keys, and the client's settings.json
/// can name a different one -- see SettingsReader::GetOverlayHotkey.
volatile long g_toggleKey = VK_F9;

/// Presses seen but not yet acted on. Written by the window thread, taken by the render
/// thread, so it is interlocked rather than merely volatile.
volatile long g_togglePresses = 0;

/// Close requests seen but not yet acted on. Same ownership as g_togglePresses.
volatile long g_closeRequests = 0;

BOOL CALLBACK FindMainWindow(HWND window, LPARAM out) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);

    if (pid != ::GetCurrentProcessId() || ::GetWindow(window, GW_OWNER) != nullptr || !::IsWindowVisible(window)) {
        return TRUE;
    }

    *(HWND*)out = window;
    return FALSE;
}

/// The game's own top-level window: this process, visible, and owned by nothing. Grim Dawn
/// has exactly one by the time the hook attaches, since the attach waits for a live world.
HWND FindGameWindow() {
    HWND window = nullptr;
    ::EnumWindows(FindMainWindow, (LPARAM)&window);
    return window;
}

}  // namespace

OverlayInput::OverlayInput() = default;

OverlayInput::OverlayInput(DataQueue* dataQueue, HANDLE hEvent) {
    m_dataQueue = dataQueue;
    m_hEvent = hEvent;
}

bool OverlayInput::IsSuppressing() {
    return m_isSuppressing;
}

void OverlayInput::SetSuppressing(bool suppress) {
    if (m_isSuppressing == suppress) {
        return;
    }

    m_isSuppressing = suppress;

    if (!suppress) {
        // Anything captured but not yet drawn belongs to the session that just ended.
        // Replaying it into the next one would deliver a click the player made a minute ago.
        MessagePump& p = pump();
        {
            std::lock_guard<std::mutex> guard(p.mutex);
            p.queued.clear();
        }

        // Whatever is still held now must not reach the game as a fresh press. Bumping the
        // generation makes every button re-evaluate itself the next time it is queried,
        // which is where the actual held state can be read (it needs the device pointer,
        // which only the hook has).
        m_suppressGeneration++;
    }

    LogToFile(LogLevel::INFO, std::string("Overlay input: ")
        + (suppress ? "withholding input from the game" : "returning input to the game"));
}

HWND OverlayInput::Window() {
    return m_window;
}

void OverlayInput::SetToggleKey(int virtualKey) {
    if (virtualKey > 0 && virtualKey <= 0xFF) {
        ::InterlockedExchange(&g_toggleKey, virtualKey);
    }
}

int OverlayInput::ToggleKey() {
    return (int)::InterlockedCompareExchange(&g_toggleKey, 0, 0);
}

unsigned int OverlayInput::TakeTogglePresses() {
    return (unsigned int)::InterlockedExchange(&g_togglePresses, 0);
}

void OverlayInput::RequestClose() {
    ::InterlockedIncrement(&g_closeRequests);
}

unsigned int OverlayInput::TakeCloseRequests() {
    return (unsigned int)::InterlockedExchange(&g_closeRequests, 0);
}

bool OverlayInput::PopMessage(OverlayWindowMessage& out) {
    MessagePump& p = pump();
    std::lock_guard<std::mutex> guard(p.mutex);

    if (p.queued.empty()) {
        return false;
    }

    out = p.queued.front();
    p.queued.pop_front();
    return true;
}

bool OverlayInput::IsOverlayInputMessage(UINT message) {
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_MOUSEWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_UNICHAR:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK OverlayInput::Hooked_WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    bool swallow = false;

    try {
        // Bit 30 of lParam is set on an auto-repeat, so holding the key does not toggle
        // the overlay sixty times.
        const bool isFirstPress = (lParam & 0x40000000) == 0;

        if (message == WM_KEYDOWN && (int)wParam == ToggleKey() && isFirstPress) {
            ::InterlockedIncrement(&g_togglePresses);

            // The key that opens the overlay is not also typed into it, and the game never
            // sees it through this path either.
            swallow = true;
        }
        else if (m_isSuppressing && message == WM_KEYDOWN && (int)wParam == VK_ESCAPE) {
            // Escape closes the overlay, the way it closes every other window in the game.
            // It must not also reach the game, or closing the overlay would open the game
            // menu behind it in the same keystroke.
            RequestClose();
            swallow = true;
        }
        else if (m_isSuppressing && IsOverlayInputMessage(message)) {
            MessagePump& p = pump();
            std::lock_guard<std::mutex> guard(p.mutex);

            if (p.queued.size() >= MaxQueuedMessages) {
                p.queued.pop_front();

                if (!p.hasReportedOverflow) {
                    p.hasReportedOverflow = true;
                    LogToFile(LogLevel::WARNING,
                        "Overlay input: the overlay is not consuming window messages fast enough, dropping the oldest."
                        " This is only reported once.");
                }
            }

            OverlayWindowMessage copy;
            copy.message = message;
            copy.wParam = wParam;
            copy.lParam = lParam;
            p.queued.push_back(copy);

            // The game reads gameplay input from DirectInput, which is already suppressed,
            // so this is belt and braces -- but anything of the game's that does read the
            // window queue must not act on what the player is typing into a search box.
            swallow = true;
        }

        // The key that releases the toggle belongs to the overlay too, or the game sees a
        // release for a press it never got.
        if (message == WM_KEYUP && (int)wParam == ToggleKey()) {
            swallow = true;
        }

        // Same for Escape: the game must not see a release for a press it never got.
        if (m_isSuppressing && message == WM_KEYUP && (int)wParam == VK_ESCAPE) {
            swallow = true;
        }
    }
    catch (...) {
        // A window procedure that throws takes the game's message pump with it. Nothing
        // above should, but this one is not the place to find out.
        swallow = false;
    }

    if (swallow) {
        return 0;
    }

    return ::CallWindowProcW(m_originalWndProc, window, message, wParam, lParam);
}

int __fastcall OverlayInput::Hooked_GetNumKeyEvents(void* This) {
    // An empty queue rather than a filtered one: the game never indexes past the count it
    // is given, so GetKeyEvent is not called at all and its layout never matters.
    if (m_isSuppressing) {
        return 0;
    }

    return dll_GetNumKeyEvents(This);
}

int __fastcall OverlayInput::Hooked_GetNumMouseEvents(void* This) {
    if (m_isSuppressing) {
        return 0;
    }

    return dll_GetNumMouseEvents(This);
}

bool __fastcall OverlayInput::Hooked_IsButtonDown(void* This, int button) {
    // Held state has to be suppressed as well as queued events. Without this, a button
    // that was already down when the overlay opened reads as still held, and the game
    // keeps acting on it for as long as the overlay is up.
    if (m_isSuppressing) {
        return false;
    }

    const bool isReallyDown = dll_IsButtonDown(This, button);

    // Outside the bound we know nothing about the enum, so pass it through untouched
    // rather than guess.
    if (button < 0 || button >= ButtonSlots) {
        return isReallyDown;
    }

    // First look at this button since suppression ended: if it is down right now, the
    // press began while the game was blind and must stay hidden until it is released.
    if (m_buttonGeneration[button] != m_suppressGeneration) {
        m_buttonGeneration[button] = m_suppressGeneration;
        m_isHeldOver[button] = isReallyDown;
    }

    if (m_isHeldOver[button]) {
        if (isReallyDown) {
            return false;
        }

        // Released at last. From the next press onwards the game sees it normally.
        m_isHeldOver[button] = false;
    }

    return isReallyDown;
}

void OverlayInput::EnableHook() {
    m_isInstalled = false;
    m_isSuppressing = false;

    // Nothing to feed. On an unsupported renderer the overlay can never open, so installing
    // the detours would only take the open key away from the player and give nothing back --
    // the window procedure swallows that key, and there would be no overlay to show for it.
    if (!OverlayHost::IsRendererSupported()) {
        LogToFile(LogLevel::INFO,
            "Overlay input: the renderer does not support the in-game browser, so the game's input is left alone.");
        return;
    }

    HMODULE module = ::GetModuleHandleW(L"DirectInput.dll");
    if (module == nullptr) {
        LogToFile(LogLevel::INFO,
            "Overlay input: DirectInput.dll is not loaded, so the game's input cannot be withheld."
            " The overlay will stay disabled rather than open over a game that still responds to clicks.");
        return;
    }

    dll_GetNumKeyEvents = (DirectInputDevice_GetNumEvents)::GetProcAddress(module, DI_GET_NUM_KEY_EVENTS);
    dll_GetNumMouseEvents = (DirectInputDevice_GetNumEvents)::GetProcAddress(module, DI_GET_NUM_MOUSE_EVENTS);
    dll_IsButtonDown = (DirectInputDevice_IsButtonDown)::GetProcAddress(module, DI_IS_BUTTON_DOWN);

    LogToFile(LogLevel::INFO, std::string("Overlay input: ") + DI_GET_NUM_KEY_EVENTS + (dll_GetNumKeyEvents ? " resolved" : " MISSING"));
    LogToFile(LogLevel::INFO, std::string("Overlay input: ") + DI_GET_NUM_MOUSE_EVENTS + (dll_GetNumMouseEvents ? " resolved" : " MISSING"));
    LogToFile(LogLevel::INFO, std::string("Overlay input: ") + DI_IS_BUTTON_DOWN + (dll_IsButtonDown ? " resolved" : " MISSING"));

    // All three or none. A partial install would suppress some input and not the rest,
    // which is worse than not offering the overlay: the player would type into a search
    // box while their character kept acting on the half that still got through.
    if (dll_GetNumKeyEvents == nullptr || dll_GetNumMouseEvents == nullptr || dll_IsButtonDown == nullptr) {
        LogToFile(LogLevel::WARNING,
            "Overlay input: the game's input exports did not all resolve, most likely a game patch."
            " The overlay will stay disabled rather than open without being able to withhold input.");
        dll_GetNumKeyEvents = nullptr;
        dll_GetNumMouseEvents = nullptr;
        dll_IsButtonDown = nullptr;
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach((PVOID*)&dll_GetNumKeyEvents, Hooked_GetNumKeyEvents);
    DetourAttach((PVOID*)&dll_GetNumMouseEvents, Hooked_GetNumMouseEvents);
    DetourAttach((PVOID*)&dll_IsButtonDown, Hooked_IsButtonDown);
    const LONG result = DetourTransactionCommit();

    if (result != NO_ERROR) {
        LogToFile(LogLevel::WARNING, "Overlay input: failed to install the input detours ("
            + std::to_string(result) + "), the overlay will stay disabled.");
        dll_GetNumKeyEvents = nullptr;
        dll_GetNumMouseEvents = nullptr;
        dll_IsButtonDown = nullptr;
        return;
    }

    // The window subclass is what the overlay itself is driven by, and it is deliberately
    // installed after the detours: with the detours in place but no subclass the overlay
    // simply never opens, whereas the other order would leave a window procedure pointing
    // at code that cannot take input away from the game.
    m_window = FindGameWindow();
    if (m_window == nullptr) {
        LogToFile(LogLevel::WARNING,
            "Overlay input: the game's window was not found, so the overlay has no way to receive input."
            " The overlay will stay disabled; every other hook is unaffected.");
    }
    else {
        m_originalWndProc = (WNDPROC)::SetWindowLongPtrW(m_window, GWLP_WNDPROC, (LONG_PTR)Hooked_WndProc);

        if (m_originalWndProc == nullptr) {
            LogToFile(LogLevel::WARNING, "Overlay input: could not subclass the game's window ("
                + std::to_string(::GetLastError()) + "), the overlay will stay disabled.");
        }
        else {
            LogToFile(LogLevel::INFO, "Overlay input: watching the game's window, open/close key is virtual-key "
                + std::to_string(ToggleKey()) + ".");
        }
    }

    m_isInstalled = true;
    LogToFile(LogLevel::INFO, "Overlay input: installed, passing input through until the overlay opens.");
}

void OverlayInput::DisableHook() {
    // Input goes back to the game before the detours come out, so no frame can observe
    // suppression through a half-removed hook.
    SetSuppressing(false);

    // The window procedure goes first: it is the only one of these that the game can still
    // be executing after this DLL is unmapped, and it points into this module.
    if (m_window != nullptr && m_originalWndProc != nullptr) {
        ::SetWindowLongPtrW(m_window, GWLP_WNDPROC, (LONG_PTR)m_originalWndProc);
        m_originalWndProc = nullptr;
        m_window = nullptr;
        LogToFile(LogLevel::INFO, "Overlay input: the game's window procedure has been restored.");
    }

    if (!m_isInstalled) {
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach((PVOID*)&dll_GetNumKeyEvents, Hooked_GetNumKeyEvents);
    DetourDetach((PVOID*)&dll_GetNumMouseEvents, Hooked_GetNumMouseEvents);
    DetourDetach((PVOID*)&dll_IsButtonDown, Hooked_IsButtonDown);
    DetourTransactionCommit();

    m_isInstalled = false;
    dll_GetNumKeyEvents = nullptr;
    dll_GetNumMouseEvents = nullptr;
    dll_IsButtonDown = nullptr;
    LogToFile(LogLevel::INFO, "Overlay input: detours removed.");
}
