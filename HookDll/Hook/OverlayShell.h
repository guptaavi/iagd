#pragma once

namespace Rml {
    class ElementDocument;
}

/// <summary>
/// The overlay's application: the search bar, the filter sidebar, the result grid and the
/// stat pane, and the listeners that connect them to the search worker.
///
/// This is the only code that knows RmlUi exists as a UI toolkit rather than as a place to
/// draw. Everything it shows comes from the core layer -- the ported query, the ported
/// icon lookup, the ported replica row parser -- and none of that knows about RmlUi. The
/// seam is deliberate: the toolkit is the part most likely to be replaced.
///
/// Everything here runs on the game's render thread, called from OverlayUi.
/// </summary>
class OverlayShell {
public:
    /// The document built into the DLL. Overridden by a file on disk when there is one;
    /// see OverlayUi.
    static const char* DocumentRml();

    /// <summary>
    /// Attaches to a freshly loaded document: fills in the parts that are generated from
    /// the ported filter tables rather than written out in the RML, wires the listeners,
    /// and runs the first search.
    /// </summary>
    static void Bind(Rml::ElementDocument* document);

    /// Detaches from the document before it is closed. The listeners belong to this class
    /// and outlive no document.
    static void Unbind();

    /// Once per frame: picks up whatever the worker has finished and notices when the
    /// client has changed the collection underneath the overlay.
    static void Update();
};
