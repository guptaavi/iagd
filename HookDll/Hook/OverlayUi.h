#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11RenderTargetView;

namespace Rml {
    class Context;
    class ElementDocument;
}

/// <summary>
/// The overlay's RmlUi shell: the three interfaces RmlUi needs from its host, the context
/// and document it draws, and the per-frame call that draws them.
///
/// RmlUi is written for an application that owns its window, its device and its main loop.
/// A hook owns none of those, so the adaptation is entirely in what these interfaces are
/// given rather than in changes to RmlUi itself:
///
///   system  -- RmlUi's diagnostics go to the hook log rather than to a console nobody
///              sees, so a font that failed to load or an RCSS typo is answerable from the
///              same file as everything else the hook reports.
///   file    -- rooted at the Item Assistant client's folder, which is where the item
///              icons the overlay will show already live. Absolute paths pass through, so
///              a system font can still be named directly.
///   render  -- RmlUi's upstream DirectX 11 backend, handed the device the game is already
///              using and pointed at the render target the game already has bound.
///
/// Everything here runs on the game's render thread, called from OverlayHost's present
/// hook. RmlUi is not thread safe and nothing else may touch it.
/// </summary>
class OverlayUi {
public:
    /// <summary>
    /// Brings RmlUi up against the game's device the first time it is called, and does
    /// nothing on every call after that.
    ///
    /// Deferred to the first drawn frame rather than done at attach because the device and
    /// the frame's dimensions are only knowable from inside the present hook. A failure is
    /// permanent for the session: it returns false from then on without retrying, so a
    /// missing font cannot produce one log line per frame.
    /// </summary>
    static bool EnsureInitialised(ID3D11Device* device, int width, int height);

    /// Draws one frame into the render target the game has bound. Does nothing until
    /// EnsureInitialised has succeeded.
    static void Render(ID3D11RenderTargetView* renderTarget, int width, int height);

    /// Releases the context, the document and the interfaces. Safe to call when RmlUi was
    /// never brought up.
    static void Shutdown();

    static bool IsInitialised();
};
