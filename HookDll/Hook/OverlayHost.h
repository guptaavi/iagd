#pragma once

#include "BaseMethodHook.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

/// <summary>
/// Owns the overlay's foothold in the game's renderer: finding the Direct3D 11 device the
/// game is already using, and (later) drawing into the frame the game has just finished.
///
/// Grim Dawn ships two renderers, selected by the /d3d9 and /d3d11 launch arguments, and
/// loads exactly one of them. The overlay supports Direct3D 11 only, so this class is
/// expected to find nothing on a /d3d9 launch. That is a supported configuration and not
/// an error: every other hook installs as usual and the overlay disables itself.
///
/// The device does not have to be stolen the way it does in most overlays. Grim Dawn's
/// Direct3D11.dll exports an accessor for it:
///
///   ?GetDevice@Direct3DDevice11@GAME@@QEBAPEAUID3D11Device@@XZ
///
/// and EndFrame is an exported virtual on the same object, so the "this" handed to the
/// hook is the very object GetDevice must be called on. That removes the usual dance of
/// creating a throwaway device, walking its vtable and detouring IDXGISwapChain::Present.
/// </summary>
class OverlayHost : public BaseMethodHook {
public:
    OverlayHost();
    OverlayHost(DataQueue* dataQueue, HANDLE hEvent);

    void EnableHook() override;
    void DisableHook() override;

    /// <summary>
    /// Whether the overlay can run against the renderer this process actually loaded.
    /// False on /d3d9, and on any future renderer whose exports do not resolve.
    /// Callers must treat false as "offer no overlay", never as a failure.
    /// </summary>
    static bool IsAvailable();

    /// <summary>
    /// Whether the process loaded a renderer the overlay can draw into.
    ///
    /// Answerable before any hook is installed, and separate from IsAvailable, which also
    /// requires the exports to have resolved and the detour to have taken. The input hook
    /// asks this before installing anything of its own: on an unsupported renderer there is
    /// no overlay for it to feed, and a window procedure that swallowed the open key there
    /// would take that key away from the player and give nothing back.
    /// </summary>
    static bool IsRendererSupported();

    /// <summary>
    /// Tells the player, once per session, that the in-game browser is unavailable on this
    /// renderer. Does nothing on a supported one, and nothing on later calls.
    ///
    /// GAME THREAD ONLY. The overlay has no hook of its own on an unsupported renderer, so
    /// this is called from one that installs regardless of which renderer is loaded.
    /// </summary>
    static void ShowUnavailableNoticeOnce();

private:
    /// Both are __thiscall on an object we never construct, so they are declared as taking
    /// the instance explicitly. GetDevice is const (QEBA) and EndFrame is a virtual (UEAA);
    /// neither distinction matters at the ABI level here, but the mangled names encode them.
    typedef ID3D11Device* (__thiscall* Direct3DDevice11_GetDevice)(void* This);
    typedef void (__thiscall* Direct3DDevice11_PresentSurface)(void* This, void* renderSurface);

    static Direct3DDevice11_GetDevice dll_GetDevice;
    static Direct3DDevice11_PresentSurface dll_PresentSurface;

    /// <summary>
    /// Runs on the game's render thread with the finished image about to be presented, so
    /// anything drawn here lands on top of everything the game drew for that frame.
    ///
    /// EndFrame was tried first and is the wrong point: the target bound there carries
    /// D3D11_BIND_SHADER_RESOURCE because the game reads it back for post-processing, so a
    /// rectangle drawn into it fed the bloom pass and smeared across the whole screen.
    /// </summary>
    static void __fastcall Hooked_PresentSurface(void* This, void* renderSurface);

    /// Draws the overlay for one frame. Separated from the hook body so the hook itself is
    /// only the guard rails: everything that touches Direct3D is in here.
    static void RenderFrame(void* This);

    /// <summary>
    /// Whether it is safe to have the overlay open at all, independent of what the player
    /// last asked for.
    ///
    /// Two conditions, and both can stop holding while the overlay is already up:
    ///
    ///   a live world -- the overlay reads the world's mod and difficulty to decide what
    ///   it may show and where a transfer would go, and a game that is loading or tearing
    ///   down has neither. An overlay left up over that would also be taking input away
    ///   from a game that is trying to change scene.
    ///
    ///   the client running -- it is the only writer of the item database and the only
    ///   consumer of a completed transfer. Without it an item handed to the player in-game
    ///   would never be removed from their collection, and could be handed out again.
    /// </summary>
    static bool CanBeOpen();

    /// Whether the Item Assistant client's window exists. Cached, because this is asked
    /// once a frame and answered by a window-list walk.
    static bool IsClientRunning();

    /// Frames seen by the present hook, whether or not the overlay drew. Distinguishes
    /// "the overlay is off" from "the hook is never called", which look identical in a log.
    static unsigned long long m_framesSeen;

    /// Whether the overlay is currently drawing. Written and read only on the render thread.
    static bool m_isVisible;

    /// Set after a failure inside the render path. The overlay closes and stays closed
    /// rather than throwing once per frame for the rest of the session.
    static bool m_isBroken;

    /// Whether the client's window was there at the last check, and when that was. A game
    /// runs at hundreds of frames a second and the client does not come and go that fast.
    static bool m_isClientRunning;
    static unsigned long m_lastClientCheckTick;

    /// The same, for whether the game has a live world. Checked more often than the client
    /// because it is the condition that changes underneath a player who is using the
    /// overlay, rather than one they would have to alt-tab away to change.
    static bool m_isWorldAlive;
    static unsigned long m_lastWorldCheckTick;

    /// Frames drawn, logged once so "is it running at all" is answerable from the log
    /// without asking the player to describe what they saw.
    static unsigned long long m_framesDrawn;

    /// Set once the renderer's exports have been resolved. Read from the game's render
    /// thread, so it is only ever written during attach, before any hook is installed.
    static bool m_isAvailable;

    /// Whether the player has already been told the browser is unavailable. Once is the
    /// requirement: a message every frame, or every world load, would be worse than silence.
    static bool m_hasShownUnavailableNotice;

    /// Resolves an export without the FATAL-level logging the general helper does. A
    /// missing export here is the normal /d3d9 case, and logging it as a fatal error
    /// would put a false alarm in every such user's log.
    static void* ResolveQuietly(const wchar_t* dll, const char* procAddress);
};
