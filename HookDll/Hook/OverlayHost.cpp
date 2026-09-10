#include "stdafx.h"
#include "OverlayHost.h"
#include "Logger.h"
#include "OverlayInput.h"
#include "OverlayUi.h"
#include "OverlaySearch.h"
#include "GameContext.h"
#include "GrimTypes.h"

#include <string>
#include <detours.h>
#include <d3d11_1.h>

// Mangled names verified against the shipped Direct3D11.dll export table (dumpbin /exports),
// not inferred from the strings in the binary. If a game patch changes either signature the
// name changes with it, and the overlay then correctly reports itself unavailable.
#define D3D11_GET_DEVICE "?GetDevice@Direct3DDevice11@GAME@@QEBAPEAUID3D11Device@@XZ"
#define D3D11_PRESENT_SURFACE "?PresentSurface@Direct3DDevice11@GAME@@UEAAXPEAVRenderSurface@2@@Z"

OverlayHost::Direct3DDevice11_GetDevice OverlayHost::dll_GetDevice = nullptr;
OverlayHost::Direct3DDevice11_PresentSurface OverlayHost::dll_PresentSurface = nullptr;
bool OverlayHost::m_isAvailable = false;
bool OverlayHost::m_hasShownUnavailableNotice = false;
bool OverlayHost::m_isVisible = false;
bool OverlayHost::m_isBroken = false;
bool OverlayHost::m_isClientRunning = false;
unsigned long OverlayHost::m_lastClientCheckTick = 0;
bool OverlayHost::m_isWorldAlive = false;
unsigned long OverlayHost::m_lastWorldCheckTick = 0;
unsigned long long OverlayHost::m_framesSeen = 0;
unsigned long long OverlayHost::m_framesDrawn = 0;

OverlayHost::OverlayHost() = default;

OverlayHost::OverlayHost(DataQueue* dataQueue, HANDLE hEvent) {
    m_dataQueue = dataQueue;
    m_hEvent = hEvent;
}

bool OverlayHost::IsAvailable() {
    return m_isAvailable;
}

bool OverlayHost::IsRendererSupported() {
    // Which renderer the process loaded, asked of the loader rather than of a launch
    // argument: the argument is what the player typed, this is what the game did with it.
    return ::GetModuleHandleW(L"Direct3D11.dll") != nullptr;
}

void OverlayHost::ShowUnavailableNoticeOnce() {
    if (m_hasShownUnavailableNotice || IsRendererSupported()) {
        return;
    }

    GAME::Engine* engine = fnGetEngine();
    if (engine == nullptr || fnShowCinematicText == nullptr) {
        // Nothing to say it through yet. The flag is deliberately left clear so the next
        // world load tries again -- the caller is a world-load hook, not a frame, so this
        // cannot become a retry storm.
        return;
    }

    m_hasShownUnavailableNotice = true;

    const std::wstring header = L"Item Assistant";
    const std::wstring body =
        L"The in-game item browser needs the DirectX 11 renderer. "
        L"Grim Dawn is running DirectX 9, so the browser is unavailable this session. "
        L"Everything else Item Assistant does is unaffected.";

    GAME::Color color;
    color.r = 1;
    color.g = 1;
    color.b = 1;
    color.a = 1;

    fnShowCinematicText(engine, &header, &body, 5, &color, false);
    LogToFile(LogLevel::INFO, "Overlay: told the player the in-game browser is unavailable on this renderer.");
}

void* OverlayHost::ResolveQuietly(const wchar_t* dll, const char* procAddress) {
    HMODULE module = ::GetModuleHandleW(dll);
    if (module == nullptr) {
        return nullptr;
    }

    return ::GetProcAddress(module, procAddress);
}

void OverlayHost::EnableHook() {
    m_isAvailable = false;
    dll_GetDevice = nullptr;
    dll_PresentSurface = nullptr;

    // Which renderer the process loaded. Reported either way: "the overlay did not appear"
    // is otherwise indistinguishable from "the overlay crashed", and the answer is usually
    // just that the player launched with the DirectX 9 renderer.
    const bool hasD3D11 = IsRendererSupported();
    const bool hasD3D9 = ::GetModuleHandleW(L"Direct3D.dll") != nullptr;

    if (!hasD3D11) {
        LogToFile(LogLevel::INFO,
            std::string("Overlay: Direct3D11.dll is not loaded (Direct3D.dll ")
            + (hasD3D9 ? "is" : "is not")
            + " loaded). The in-game browser requires the DirectX 11 renderer and will stay disabled."
            " Every other hook is unaffected.");
        return;
    }

    dll_GetDevice = (Direct3DDevice11_GetDevice)ResolveQuietly(L"Direct3D11.dll", D3D11_GET_DEVICE);
    dll_PresentSurface = (Direct3DDevice11_PresentSurface)ResolveQuietly(L"Direct3D11.dll", D3D11_PRESENT_SURFACE);

    // Named individually: if a patch renames only one of them, the log should say which.
    LogToFile(LogLevel::INFO, std::string("Overlay: ") + D3D11_GET_DEVICE + (dll_GetDevice ? " resolved" : " MISSING"));
    LogToFile(LogLevel::INFO, std::string("Overlay: ") + D3D11_PRESENT_SURFACE + (dll_PresentSurface ? " resolved" : " MISSING"));

    if (dll_GetDevice == nullptr || dll_PresentSurface == nullptr) {
        LogToFile(LogLevel::WARNING,
            "Overlay: the Direct3D 11 renderer is loaded but its exports did not resolve, most likely a game"
            " patch that changed a signature. The in-game browser will stay disabled.");
        dll_GetDevice = nullptr;
        dll_PresentSurface = nullptr;
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach((PVOID*)&dll_PresentSurface, Hooked_PresentSurface);
    const LONG detourResult = DetourTransactionCommit();

    if (detourResult != NO_ERROR) {
        LogToFile(LogLevel::WARNING,
            "Overlay: failed to install the PresentSurface detour (" + std::to_string(detourResult)
            + "), the in-game browser will stay disabled.");
        dll_GetDevice = nullptr;
        dll_PresentSurface = nullptr;
        return;
    }

    m_isAvailable = true;
    LogToFile(LogLevel::INFO, "Overlay: Direct3D 11 renderer detected, in-game browser is available."
        " Press the open/close key (F9 by default, 'local.overlayHotkey' in settings.json) during play to show it.");
}

std::wstring GetIagdFolder();

/// Set once at attach, from the client's settings.json. Under Wine the client has no
/// window to find, so the "is the client running" test cannot be the one used everywhere
/// else -- see IsClientRunning.
extern bool g_isRunningInWine;

bool OverlayHost::IsClientRunning() {
    // The same window the worker thread posts captured items to. Its existence is what the
    // rest of the DLL already means by "the client is running".
    const unsigned long tick = ::GetTickCount();

    // Unsigned arithmetic, so this stays correct across the 49-day wrap without a special
    // case: the difference is what matters, not the ordering.
    if (m_lastClientCheckTick != 0 && (tick - m_lastClientCheckTick) < 1000) {
        return m_isClientRunning;
    }

    m_lastClientCheckTick = tick;

    if (g_isRunningInWine) {
        // No window to look for: the client runs outside the prefix and talks to the hook
        // through the linuxhack folder instead. Treated as running for the same reason
        // InventorySack_AddItem is left permanently active there.
        m_isClientRunning = true;
        return true;
    }

    m_isClientRunning = ::FindWindowW(L"GDIAWindowClass", nullptr) != nullptr;
    return m_isClientRunning;
}

bool OverlayHost::CanBeOpen() {
    // fnIsWorldAlive is the only honest answer to "is there a world": the individual state
    // getters do not go null on exit-to-menu, and GameContext's cache is not invalidated
    // there either -- it holds the world the player just left. Asking the game directly is
    // what distinguishes standing in Devil's Crossing from standing at the main menu.
    //
    // Called from the render thread, which OnDemandSeedInfo already does from its own
    // Engine::Render hook. Throttled, because fnGetGameEngine resolves an export every
    // call and the answer changes on the scale of a loading screen, not a frame.
    const unsigned long tick = ::GetTickCount();
    if (m_lastWorldCheckTick == 0 || (tick - m_lastWorldCheckTick) >= 250) {
        m_lastWorldCheckTick = tick;
        m_isWorldAlive = fnIsWorldAlive(fnGetGameEngine());
    }

    if (!m_isWorldAlive) {
        return false;
    }

    // The world's mod and difficulty decide what the overlay may show and where a transfer
    // would go, so an overlay that cannot read them has nothing to offer.
    std::wstring modName;
    bool isHardcore = false;
    if (!GameContext::TryGet(modName, isHardcore)) {
        return false;
    }

    return IsClientRunning();
}

/// <summary>
/// Everything the overlay's draw can disturb, saved before it draws and put back after.
///
/// RmlUi's DirectX 11 backend does save and restore state of its own, and it is not enough.
/// It never touches the pixel shader's constant buffers, and it backs up exactly one shader
/// resource slot. Grim Dawn does not re-set render state it believes is already bound, so
/// anything left changed is never set again: opening the overlay once left the game drawing
/// its HUD over a black world for the rest of the session, and closing the overlay did not
/// bring it back. That is the failure this class exists to prevent, and it is why task 2.3
/// recorded a full save/restore as owed the moment a real pipeline replaced the bring-up
/// ClearView.
///
/// The slot counts are the ones a scene pass plausibly uses rather than D3D's maxima --
/// restoring 128 shader resource slots every frame to protect the eight anyone binds is a
/// cost paid on every frame the overlay is open.
/// </summary>
namespace {

struct PipelineStateBackup {
    static const UINT kRenderTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;  // 8
    static const UINT kShaderResources = 16;
    static const UINT kConstantBuffers = 4;
    static const UINT kSamplers = 4;
    static const UINT kVertexBuffers = 4;

    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depthStencil = nullptr;
    UINT stencilRef = 0;

    ID3D11RenderTargetView* renderTargets[kRenderTargets] = {};
    ID3D11DepthStencilView* depthStencilView = nullptr;

    ID3D11InputLayout* inputLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    ID3D11Buffer* vertexBuffers[kVertexBuffers] = {};
    UINT vertexStrides[kVertexBuffers] = {};
    UINT vertexOffsets[kVertexBuffers] = {};

    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11GeometryShader* geometryShader = nullptr;

    ID3D11Buffer* vsConstantBuffers[kConstantBuffers] = {};
    ID3D11Buffer* psConstantBuffers[kConstantBuffers] = {};
    ID3D11ShaderResourceView* vsResources[kShaderResources] = {};
    ID3D11ShaderResourceView* psResources[kShaderResources] = {};
    ID3D11SamplerState* psSamplers[kSamplers] = {};

    void Capture(ID3D11DeviceContext* context) {
        context->RSGetScissorRects(&scissorCount, scissors);
        context->RSGetViewports(&viewportCount, viewports);
        context->RSGetState(&rasterizer);

        context->OMGetBlendState(&blend, blendFactor, &sampleMask);
        context->OMGetDepthStencilState(&depthStencil, &stencilRef);
        context->OMGetRenderTargets(kRenderTargets, renderTargets, &depthStencilView);

        context->IAGetInputLayout(&inputLayout);
        context->IAGetPrimitiveTopology(&topology);
        context->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        context->IAGetVertexBuffers(0, kVertexBuffers, vertexBuffers, vertexStrides, vertexOffsets);

        // Null for the class-instance arrays: this DLL does not use shader linkage, and
        // asking for instances would mean carrying 256 pointers per stage for nothing.
        context->VSGetShader(&vertexShader, nullptr, nullptr);
        context->PSGetShader(&pixelShader, nullptr, nullptr);
        context->GSGetShader(&geometryShader, nullptr, nullptr);

        context->VSGetConstantBuffers(0, kConstantBuffers, vsConstantBuffers);
        context->PSGetConstantBuffers(0, kConstantBuffers, psConstantBuffers);
        context->VSGetShaderResources(0, kShaderResources, vsResources);
        context->PSGetShaderResources(0, kShaderResources, psResources);
        context->PSGetSamplers(0, kSamplers, psSamplers);
    }

    void Restore(ID3D11DeviceContext* context) {
        context->RSSetScissorRects(scissorCount, scissors);
        context->RSSetViewports(viewportCount, viewports);
        context->RSSetState(rasterizer);

        context->OMSetBlendState(blend, blendFactor, sampleMask);
        context->OMSetDepthStencilState(depthStencil, stencilRef);
        context->OMSetRenderTargets(kRenderTargets, renderTargets, depthStencilView);

        context->IASetInputLayout(inputLayout);
        context->IASetPrimitiveTopology(topology);
        context->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);
        context->IASetVertexBuffers(0, kVertexBuffers, vertexBuffers, vertexStrides, vertexOffsets);

        context->VSSetShader(vertexShader, nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->GSSetShader(geometryShader, nullptr, 0);

        context->VSSetConstantBuffers(0, kConstantBuffers, vsConstantBuffers);
        context->PSSetConstantBuffers(0, kConstantBuffers, psConstantBuffers);
        context->VSSetShaderResources(0, kShaderResources, vsResources);
        context->PSSetShaderResources(0, kShaderResources, psResources);
        context->PSSetSamplers(0, kSamplers, psSamplers);
    }

    /// The Get calls above each add a reference. Setting a resource does not take one, so
    /// every one of them has to be given back or the overlay leaks a device object per frame.
    void Release() {
        ReleaseOne((IUnknown**)&rasterizer);
        ReleaseOne((IUnknown**)&blend);
        ReleaseOne((IUnknown**)&depthStencil);
        ReleaseOne((IUnknown**)&depthStencilView);
        ReleaseOne((IUnknown**)&inputLayout);
        ReleaseOne((IUnknown**)&indexBuffer);
        ReleaseOne((IUnknown**)&vertexShader);
        ReleaseOne((IUnknown**)&pixelShader);
        ReleaseOne((IUnknown**)&geometryShader);

        ReleaseArray((IUnknown**)renderTargets, kRenderTargets);
        ReleaseArray((IUnknown**)vertexBuffers, kVertexBuffers);
        ReleaseArray((IUnknown**)vsConstantBuffers, kConstantBuffers);
        ReleaseArray((IUnknown**)psConstantBuffers, kConstantBuffers);
        ReleaseArray((IUnknown**)vsResources, kShaderResources);
        ReleaseArray((IUnknown**)psResources, kShaderResources);
        ReleaseArray((IUnknown**)psSamplers, kSamplers);
    }

private:
    static void ReleaseOne(IUnknown** object) {
        if (*object != nullptr) {
            (*object)->Release();
            *object = nullptr;
        }
    }

    static void ReleaseArray(IUnknown** objects, UINT count) {
        for (UINT i = 0; i < count; i++) {
            ReleaseOne(&objects[i]);
        }
    }
};

}  // namespace

/// <summary>
/// The size of the image a render target view addresses. RmlUi needs it in pixels, and the
/// bound target is the only honest source: the game can change resolution or go borderless
/// without the hook being told, and a stale viewport puts the UI in the wrong place.
/// </summary>
static bool GetRenderTargetSize(ID3D11RenderTargetView* renderTarget, int& outWidth, int& outHeight, bool logDetails) {
    ID3D11Resource* resource = nullptr;
    renderTarget->GetResource(&resource);
    if (resource == nullptr) {
        return false;
    }

    ID3D11Texture2D* texture = nullptr;
    const bool gotTexture = SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture)) && texture != nullptr;
    resource->Release();

    if (!gotTexture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->Release();

    if (logDetails) {
        // Logged once. This is what identified EndFrame as the wrong hook point: the target
        // bound there carried D3D11_BIND_SHADER_RESOURCE, so the game read the overlay back
        // into its bloom pass. Worth being able to check again after a game patch.
        LogToFile(LogLevel::INFO,
            "Overlay/diag render target " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height)
            + " fmt=" + std::to_string((int)desc.Format)
            + " samples=" + std::to_string(desc.SampleDesc.Count)
            + " bind=" + std::to_string(desc.BindFlags)
            + " misc=" + std::to_string(desc.MiscFlags));
    }

    outWidth = (int)desc.Width;
    outHeight = (int)desc.Height;
    return outWidth > 0 && outHeight > 0;
}

/// <summary>
/// Draws one frame of the overlay into whatever render target the game currently has bound.
///
/// The render target is recovered with OMGetRenderTargets rather than tracked from the swap
/// chain: Direct3DDevice11::CreateSwapChain is private and returns nothing, so there is no
/// exported way to reach the swap chain, and the target that matters is simply the one bound
/// at this point in the frame.
///
/// RmlUi's DirectX 11 backend saves and restores the pipeline state it changes, with one
/// gap: its EndFrame binds the render target it is given and passes no depth-stencil view,
/// so whatever the game had bound there is silently dropped. That one is restored here.
/// </summary>
void OverlayHost::RenderFrame(void* This) {
    ID3D11Device* device = dll_GetDevice(This);
    if (device == nullptr) {
        return;
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr) {
        return;
    }

    ID3D11RenderTargetView* renderTarget = nullptr;
    ID3D11DepthStencilView* depthStencil = nullptr;
    context->OMGetRenderTargets(1, &renderTarget, &depthStencil);

    if (renderTarget != nullptr) {
        int width = 0;
        int height = 0;

        if (GetRenderTargetSize(renderTarget, width, height, m_framesDrawn == 0)) {
            if (OverlayUi::EnsureInitialised(device, width, height)) {
                // Everything the game had bound, saved and put back around the draw. The
                // game does not re-set state it thinks is already there, so this is not
                // tidiness -- without it, one drawn frame is enough to lose the world for
                // the rest of the session.
                PipelineStateBackup backup;
                backup.Capture(context);

                OverlayUi::Render(renderTarget, width, height);

                backup.Restore(context);
                backup.Release();

                if (m_framesDrawn == 0) {
                    LogToFile(LogLevel::INFO, "Overlay: first frame drawn at PresentSurface.");
                }

                m_framesDrawn++;
            }
            else {
                // Bring-up failed and will not succeed on a later frame. Closing here is
                // what keeps the failure to one log line instead of one per frame.
                LogToFile(LogLevel::WARNING, "Overlay: the UI could not be brought up, closing the overlay for this session.");
                m_isBroken = true;
            }
        }

        renderTarget->Release();
    }
    else if (m_framesDrawn == 0) {
        // Worth saying once: it means the assumption about what is bound at PresentSurface
        // is wrong, which is a design problem rather than a transient one.
        LogToFile(LogLevel::WARNING, "Overlay: no render target bound at PresentSurface, nothing drawn.");
    }

    if (depthStencil != nullptr) {
        depthStencil->Release();
    }

    context->Release();
}

void __fastcall OverlayHost::Hooked_PresentSurface(void* This, void* renderSurface) {
    // Drawing happens before the original runs: PresentSurface hands the finished image to
    // the swap chain, so anything drawn afterwards would be too late to appear in it.
    try {
        if (This != nullptr && !m_isBroken && dll_GetDevice != nullptr) {
            // Proves the hook is reached at all, and at what rate. Without this, "the overlay
            // is off" and "the present hook is never called" produce an identical log.
            if (m_framesSeen == 0) {
                LogToFile(LogLevel::INFO, "Overlay: PresentSurface hook is live (first call).");
            }
            else if (m_framesSeen % 600 == 0) {
                LogToFile(LogLevel::INFO, "Overlay: present hook alive, frames seen="
                    + std::to_string(m_framesSeen) + " drawn=" + std::to_string(m_framesDrawn));
            }
            m_framesSeen++;

            const bool wasVisible = m_isVisible;

            // The key is counted where it arrives, on the window thread, and acted on here.
            // A GetAsyncKeyState poll was tried instead and dropped: it reads global key
            // state, so it saw the key pressed in any other window on the machine and
            // flapped the overlay on and off roughly once a second.
            //
            // An odd number of presses since the last frame is a toggle; an even number is
            // the player having changed their mind, and leaves the state alone.
            if ((OverlayInput::TakeTogglePresses() % 2) != 0) {
                m_isVisible = !m_isVisible;

                if (m_isVisible && !CanBeOpen()) {
                    // Refused rather than silently ignored: the player pressed a key and is
                    // owed an answer for why nothing happened.
                    m_isVisible = false;
                    LogToFile(LogLevel::INFO,
                        "Overlay: refusing to open -- there is no live world, or the Item Assistant client is not running.");
                }
            }

            // Escape and the close button, which mean "shut it" rather than "toggle it".
            // Consumed unconditionally so a request that arrives while the overlay is
            // already closed is discarded rather than left to close the next session.
            if (OverlayInput::TakeCloseRequests() > 0 && m_isVisible) {
                m_isVisible = false;
            }

            // The conditions can also stop holding under an overlay that is already up.
            if (m_isVisible && !CanBeOpen()) {
                m_isVisible = false;
                LogToFile(LogLevel::INFO, "Overlay: closing -- the world went away, or the Item Assistant client stopped.");
            }

            if (m_isVisible && !wasVisible) {
                // Started on the first open rather than at attach: a player who never
                // opens the overlay should not pay for a second connection to a database
                // that is a few hundred megabytes on a played account, nor for a thread
                // that would spend the session asleep.
                OverlaySearch::Start();
            }

            if (m_isVisible != wasVisible) {
                // Input follows visibility, so the game stops seeing input the moment the
                // overlay appears and gets it back the moment it closes.
                OverlayInput::SetSuppressing(m_isVisible);

                LogToFile(LogLevel::INFO, std::string("Overlay: toggled ") + (m_isVisible ? "on" : "off")
                    + " (frames drawn so far: " + std::to_string(m_framesDrawn) + ")");
            }

            // A closed overlay costs the game nothing beyond this branch.
            if (m_isVisible) {
                RenderFrame(This);
            }
        }
    }
    catch (const std::exception& ex) {
        LogToFile(LogLevel::FATAL, std::string("Overlay: exception while drawing, disabling. ") + ex.what());
        m_isBroken = true;
        m_isVisible = false;
    }
    catch (...) {
        LogToFile(LogLevel::FATAL, "Overlay: unknown exception while drawing, disabling.");
        m_isBroken = true;
        m_isVisible = false;
    }

    // The game's own present runs whatever happened above. Skipping it would drop the frame.
    dll_PresentSurface(This, renderSurface);
}

void OverlayHost::DisableHook() {
    // Stop drawing before the detour comes out, so no in-flight frame is still inside
    // RenderFrame while the trampoline is being restored.
    m_isVisible = false;
    m_isAvailable = false;

    // Before the detour comes out, so the worker is not still handing results to a frame
    // that is about to stop existing.
    OverlaySearch::Stop();

    if (dll_PresentSurface != nullptr) {
        Unhook((void**)&dll_PresentSurface, Hooked_PresentSurface);
        LogToFile(LogLevel::INFO, "Overlay: PresentSurface detour removed after "
            + std::to_string(m_framesDrawn) + " drawn frames.");
    }

    // Only after the detour is out, so no frame can still be inside Render while RmlUi is
    // releasing the device objects that frame is using.
    OverlayUi::Shutdown();

    dll_GetDevice = nullptr;
    dll_PresentSurface = nullptr;
}
