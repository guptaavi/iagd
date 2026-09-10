#include "stdafx.h"
#include "OverlayUi.h"
#include "Logger.h"
#include "OverlayInput.h"
#include "OverlayShell.h"

#include <string>
#include <vector>

#include <wincodec.h>

#include <RmlUi/Core.h>

// RmlUi's backend headers define UNICODE and _UNICODE unconditionally, and this project
// already sets both on the command line. The redefinition is identical and harmless, but
// it is a warning in our translation unit rather than in theirs, so it is silenced here.
#pragma warning(push)
#pragma warning(disable : 4005)
#include "RmlUi_Platform_Win32.h"
#include "RmlUi_Renderer_DX11.h"
#pragma warning(pop)

std::wstring GetIagdFolder();

namespace {

/// <summary>
/// Sends RmlUi's diagnostics to the hook log.
///
/// Without this they go to RmlUi's default sink, which on Windows is a debugger-only
/// OutputDebugString -- invisible to a player reporting a problem and invisible in the
/// file the rest of this DLL writes. Everything RmlUi has to say about a missing font, an
/// unparsable RCSS rule or a texture it could not load is worth having in the same place
/// as the hook's own account of the session.
///
/// SystemInterface_Win32 supplies the rest (the clock RmlUi animates from, the mouse
/// cursor, the clipboard), so only the logging is ours.
/// </summary>
class HookSystemInterface : public SystemInterface_Win32 {
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        LogLevel level = LogLevel::INFO;
        const char* prefix = "RmlUi: ";

        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            // Not FATAL: RmlUi calls an unloadable font or a bad property an error and
            // carries on. FATAL in this DLL means the game is about to be in trouble.
            level = LogLevel::WARNING;
            prefix = "RmlUi ERROR: ";
            break;
        case Rml::Log::LT_WARNING:
            level = LogLevel::WARNING;
            prefix = "RmlUi WARNING: ";
            break;
        default:
            break;
        }

        LogToFile(level, std::string(prefix) + message);

        // False would stop RmlUi's own default handling, which for LT_ASSERT is the
        // debug break. Nothing here wants to suppress that.
        return true;
    }
};

/// <summary>
/// Resolves RmlUi's paths against the Item Assistant client's folder.
///
/// RmlUi asks for files by relative path and has no notion of a working directory it can
/// trust -- and a hook has no say in what the game's working directory is, so the default
/// file interface would resolve "storage/x.png" against the Grim Dawn install. Rooting it
/// at the client's folder puts the icons the client already extracted, and anything else
/// the overlay ships alongside them, one relative path away.
///
/// Absolute paths are passed through untouched, which is how a font can be named directly.
/// </summary>
class HookFileInterface : public Rml::FileInterface {
public:
    explicit HookFileInterface(std::wstring root) : m_root(std::move(root)) {}

    Rml::FileHandle Open(const Rml::String& path) override {
        const std::wstring resolved = Resolve(path);

        FILE* file = nullptr;
        if (_wfopen_s(&file, resolved.c_str(), L"rb") != 0 || file == nullptr) {
            return 0;
        }

        return (Rml::FileHandle)file;
    }

    void Close(Rml::FileHandle file) override {
        if (file) {
            fclose((FILE*)file);
        }
    }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        return fread(buffer, 1, size, (FILE*)file);
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        return fseek((FILE*)file, offset, origin) == 0;
    }

    size_t Tell(Rml::FileHandle file) override {
        return (size_t)ftell((FILE*)file);
    }

private:
    /// A path is absolute if it names a drive or a UNC share. Everything else is taken as
    /// relative to the client's folder, including a leading '/' -- RmlUi generates those
    /// itself when it resolves one document-relative path against another.
    std::wstring Resolve(const Rml::String& path) const {
        std::wstring wide = RmlWin32::ConvertToUTF16(path);

        const bool isDrivePath = wide.size() > 1 && wide[1] == L':';
        const bool isUncPath = wide.size() > 1 && wide[0] == L'\\' && wide[1] == L'\\';
        if (isDrivePath || isUncPath) {
            return wide;
        }

        while (!wide.empty() && (wide[0] == L'/' || wide[0] == L'\\')) {
            wide.erase(0, 1);
        }

        return m_root + wide;
    }

    std::wstring m_root;
};

/// <summary>
/// Adds PNG decoding to RmlUi's DirectX 11 backend.
///
/// The backend as shipped reads TGA and nothing else, and every icon the client extracted
/// from the game is a PNG. The decode is done with the Windows Imaging Component rather
/// than by vendoring an image library: design.md expected stb_image, but WIC is already
/// present on every machine this DLL runs on, adds no third-party source and no licence to
/// track, and Wine implements it.
///
/// Caching is RmlUi's: it keys textures by their source string and asks for each one once,
/// so a page of results with the same icon on twenty rows loads one texture.
/// </summary>
class HookRenderInterface : public RenderInterface_DX11 {
public:
    explicit HookRenderInterface(ID3D11Device* device) : RenderInterface_DX11(device) {}

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        std::vector<Rml::byte> file;
        if (!ReadWholeFile(source, file)) {
            LogToFile(LogLevel::WARNING, "Overlay UI: no image file at " + source + ", using the placeholder.");
            return Placeholder(dimensions);
        }

        std::vector<Rml::byte> pixels;
        if (DecodeToPremultipliedRgba(file, pixels, dimensions)) {
            return GenerateTexture(Rml::Span<const Rml::byte>(pixels.data(), pixels.size()), dimensions);
        }

        // A TGA would still be valid here, and RmlUi's own decoder is the one that knows
        // how to read it, so fall back to it rather than declaring the file broken.
        Rml::Vector2i tgaDimensions = dimensions;
        if (Rml::TextureHandle handle = RenderInterface_DX11::LoadTexture(tgaDimensions, source)) {
            dimensions = tgaDimensions;
            return handle;
        }

        LogToFile(LogLevel::WARNING, "Overlay UI: could not decode " + source + ", using the placeholder.");
        return Placeholder(dimensions);
    }

private:
    static bool ReadWholeFile(const Rml::String& source, std::vector<Rml::byte>& out) {
        Rml::FileInterface* files = Rml::GetFileInterface();
        if (files == nullptr) {
            return false;
        }

        Rml::FileHandle handle = files->Open(source);
        if (!handle) {
            return false;
        }

        files->Seek(handle, 0, SEEK_END);
        const size_t size = files->Tell(handle);
        files->Seek(handle, 0, SEEK_SET);

        if (size == 0) {
            files->Close(handle);
            return false;
        }

        out.resize(size);
        const size_t read = files->Read(out.data(), size, handle);
        files->Close(handle);

        return read == size;
    }

    /// <summary>
    /// The imaging factory, created once and kept.
    ///
    /// COM has to be initialised on this thread before the factory can be created, and the
    /// render thread is the game's, which may not have done it. Initialising it here is
    /// safe in both directions: an "already initialised, different mode" result is not an
    /// error for our purposes, and the reference is deliberately never released, because
    /// this thread goes on using COM for as long as the overlay exists.
    /// </summary>
    static IWICImagingFactory* Factory() {
        static IWICImagingFactory* factory = nullptr;
        static bool tried = false;

        if (tried) {
            return factory;
        }
        tried = true;

        const HRESULT initialised = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialised) && initialised != RPC_E_CHANGED_MODE) {
            LogToFile(LogLevel::WARNING, "Overlay UI: COM could not be initialised, item icons will be placeholders.");
            return nullptr;
        }

        if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory)))) {
            LogToFile(LogLevel::WARNING, "Overlay UI: no imaging factory, item icons will be placeholders.");
            factory = nullptr;
        }

        return factory;
    }

    /// <summary>
    /// Decodes to the layout RmlUi's renderer expects: 32-bit RGBA with the colour already
    /// multiplied by the alpha. The renderer's blend state is written for premultiplied
    /// alpha, so a straight-alpha texture draws with a dark halo around every icon.
    /// </summary>
    static bool DecodeToPremultipliedRgba(const std::vector<Rml::byte>& file, std::vector<Rml::byte>& out,
                                          Rml::Vector2i& dimensions) {
        IWICImagingFactory* factory = Factory();
        if (factory == nullptr) {
            return false;
        }

        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        bool ok = false;

        do {
            if (FAILED(factory->CreateStream(&stream)) || stream == nullptr) {
                break;
            }

            if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(file.data()), (DWORD)file.size()))) {
                break;
            }

            if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
                break;
            }

            if (FAILED(decoder->GetFrame(0, &frame)) || frame == nullptr) {
                break;
            }

            UINT width = 0;
            UINT height = 0;
            if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
                break;
            }

            if (FAILED(factory->CreateFormatConverter(&converter)) || converter == nullptr) {
                break;
            }

            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone,
                    nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                break;
            }

            const UINT stride = width * 4;
            out.resize((size_t)stride * height);

            if (FAILED(converter->CopyPixels(nullptr, stride, (UINT)out.size(), out.data()))) {
                break;
            }

            dimensions = Rml::Vector2i((int)width, (int)height);
            ok = true;
        } while (false);

        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();

        return ok;
    }

    /// <summary>
    /// What an item whose icon file is missing shows: a flat, muted square rather than
    /// nothing. A blank space reads as "this row failed to render"; a placeholder reads as
    /// "this item has no picture", which is the truth.
    /// </summary>
    Rml::TextureHandle Placeholder(Rml::Vector2i& dimensions) {
        const int size = 32;
        std::vector<Rml::byte> pixels((size_t)size * size * 4);

        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i + 0] = 60;
            pixels[i + 1] = 52;
            pixels[i + 2] = 40;
            pixels[i + 3] = 255;
        }

        dimensions = Rml::Vector2i(size, size);
        return GenerateTexture(Rml::Span<const Rml::byte>(pixels.data(), pixels.size()), dimensions);
    }
};

/// Where an on-disk document may override the one built into the DLL, relative to the
/// client's folder. Also the source URL the built-in one is loaded under, so a relative
/// path inside the document means the same thing either way.
const char* const kDocumentPath = "overlay/overlay.rml";

/// <summary>
/// Fonts, in the order they are tried.
///
/// design.md left open which font ships with the DLL and under what licence. Nothing is
/// shipped: the first entry lets a user drop one in beside the client's other files, and
/// the rest are faces Windows itself installs. That keeps a font binary out of the
/// repository and out of the licence surface, and it does not foreclose shipping one --
/// adding it as the first candidate is all that would change.
///
/// English-only for this change, so the small Latin faces below are sufficient.
/// </summary>
std::vector<std::wstring> FontCandidates() {
    std::vector<std::wstring> candidates;
    candidates.push_back(GetIagdFolder() + L"overlay\\overlay.ttf");

    wchar_t windowsFolder[MAX_PATH] = {};
    if (::GetWindowsDirectoryW(windowsFolder, MAX_PATH) != 0) {
        const std::wstring fonts = std::wstring(windowsFolder) + L"\\Fonts\\";
        candidates.push_back(fonts + L"segoeui.ttf");
        candidates.push_back(fonts + L"arial.ttf");
        candidates.push_back(fonts + L"tahoma.ttf");
    }

    return candidates;
}

HookSystemInterface* g_system = nullptr;
HookFileInterface* g_file = nullptr;
HookRenderInterface* g_render = nullptr;

/// RmlUi's Win32 IME handler. Not optional: WindowProcedure takes it by reference and
/// consults it for every mouse press, whether or not anything is being composed.
TextInputMethodEditor_Win32* g_textInput = nullptr;
Rml::Context* g_context = nullptr;
Rml::ElementDocument* g_document = nullptr;

bool g_isInitialised = false;

/// Set after a failed bring-up. Retrying every frame would fill the log with the same
/// line 60 times a second and never succeed for a different reason.
bool g_hasFailed = false;

int g_width = 0;
int g_height = 0;

/// Write time of the document the current one was loaded from, or zero when it came from
/// the copy built into the DLL. Only a document that came from disk is watched for edits.
unsigned long long g_documentFileTime = 0;

/// Frames since the override was last looked at. A file probe is cheap but not free, and
/// nothing about editing a stylesheet needs a sub-second turnaround.
unsigned int g_framesSinceFileCheck = 0;

/// The write time of the on-disk override, or zero when there is no such file. Zero is
/// also what an unreadable file reports, which is the right answer for both: there is
/// nothing to load, and the built-in document stands.
unsigned long long DocumentFileTime() {
    const std::wstring path = GetIagdFolder() + L"overlay\\overlay.rml";

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return 0;
    }

    return ((unsigned long long)attributes.ftLastWriteTime.dwHighDateTime << 32)
        | attributes.ftLastWriteTime.dwLowDateTime;
}

/// <summary>
/// Loads the overlay document, preferring the file at kDocumentPath over the copy built
/// into the DLL.
///
/// The override is what makes the look of the overlay iterable: RCSS and RML can be edited
/// and seen in the running game, where the alternative is a rebuild, a game restart and a
/// re-injection for every changed margin. Players never have the file, so they always get
/// the built-in document.
/// </summary>
Rml::ElementDocument* LoadOverlayDocument(unsigned long long& outFileTime) {
    outFileTime = DocumentFileTime();

    if (outFileTime != 0) {
        Rml::ElementDocument* fromDisk = g_context->LoadDocument(kDocumentPath);
        if (fromDisk != nullptr) {
            LogToFile(LogLevel::INFO, L"Overlay UI: using the document at " + GetIagdFolder() + L"overlay\\overlay.rml");
            return fromDisk;
        }

        // Already logged in detail through the system interface; say what happens next.
        LogToFile(LogLevel::WARNING, "Overlay UI: the document on disk could not be loaded, falling back to the built-in one.");
        outFileTime = 0;
    }

    return g_context->LoadDocumentFromMemory(OverlayShell::DocumentRml(), kDocumentPath);
}

/// <summary>
/// Hands the window messages captured since the last frame to RmlUi.
///
/// They are replayed here rather than handled where they arrived because RmlUi lives on
/// this thread alone, and because a window procedure that waited for the overlay's layout
/// would be stalling the game's message pump to do it.
///
/// Two consequences of the thread move, both deliberate:
///
///   Modifier state is read by RmlUi's backend with GetKeyState, which reports the calling
///   thread's view of the keyboard -- and this thread never pumps messages, so it always
///   reads as "nothing held". Typing is unaffected, because WM_CHAR carries the character
///   the layout already produced, shift and all. Only shortcuts that ask about a modifier
///   directly would notice, and the overlay has none.
///
///   SetCapture inside the backend's button handling silently does nothing off the window
///   thread. Capture is not needed here: the overlay covers the game's own window, so the
///   pointer cannot leave it without leaving the game.
///
/// Mouse positions are scaled from the window's client area into the render target's
/// pixels. They are usually the same, but the game is free to render at a resolution that
/// is not the window's size, and then every click would land somewhere else.
/// </summary>
/// How far one notch of the wheel moves a pane. Three lines is the Windows default and
/// reads as a crawl against cards this tall, so this is nearer a third of a card.
const float kWheelStepPixels = 120.0f;

/// <summary>
/// Scrolls the innermost clipping pane under the pointer.
///
/// Walks up from the hovered element rather than addressing #results by name: the pointer
/// may be over the sidebar or the stat pane, and each of those is as much a scrolling pane
/// as the results are. Only elements that actually clip are considered, so the walk cannot
/// end up scrolling the document itself.
/// </summary>
void ScrollUnderPointer(float deltaPixels) {
    for (Rml::Element* element = g_context->GetHoverElement();
         element != nullptr;
         element = element->GetParentNode()) {

        if (element->GetComputedValues().overflow_y() == Rml::Style::Overflow::Visible) {
            continue;
        }

        const float range = element->GetScrollHeight() - element->GetClientHeight();
        if (range <= 1.0f) {
            continue;
        }

        float target = element->GetScrollTop() + deltaPixels;
        target = target < 0.0f ? 0.0f : (target > range ? range : target);

        element->SetScrollTop(target);
        return;
    }
}

void DrainInput() {
    HWND window = OverlayInput::Window();
    if (window == nullptr) {
        return;
    }

    float scaleX = 1.0f;
    float scaleY = 1.0f;

    RECT client = {};
    if (::GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
        scaleX = (float)g_width / (float)client.right;
        scaleY = (float)g_height / (float)client.bottom;
    }

    OverlayWindowMessage message;
    while (OverlayInput::PopMessage(message)) {
        LPARAM lParam = message.lParam;

        // WM_MOUSEMOVE is the only one of these whose lParam is a position the backend
        // reads; the button messages carry one too but it is not used, and WM_MOUSEWHEEL's
        // is in screen coordinates and equally unused.
        if (message.message == WM_MOUSEMOVE && (scaleX != 1.0f || scaleY != 1.0f)) {
            const int x = (int)((short)LOWORD(lParam) * scaleX);
            const int y = (int)((short)HIWORD(lParam) * scaleY);
            lParam = MAKELPARAM(x, y);
        }

        const bool wasConsumed = !RmlWin32::WindowProcedure(
            g_context, *g_textInput, window, message.message, message.wParam, lParam);

        // The panes clip rather than scroll (see the overflow comment in OverlayShell), so
        // RmlUi has no scrollbar to work the wheel against and leaves the message
        // unconsumed. Turning it into a scroll offset is this side's job.
        if (message.message == WM_MOUSEWHEEL && !wasConsumed) {
            const float notches = (float)(short)HIWORD(message.wParam) / (float)WHEEL_DELTA;
            ScrollUnderPointer(-notches * kWheelStepPixels);
        }
    }
}

/// <summary>
/// Swaps in an edited document while the game keeps running.
///
/// Only a document that was loaded from disk is watched: the built-in one cannot change,
/// and probing for a file that has never existed on every frame would be a cost paid by
/// every player for a convenience only a developer uses. Creating the file therefore takes
/// effect on the next time the overlay is opened, not immediately.
/// </summary>
void ReloadIfChanged() {
    if (g_documentFileTime == 0) {
        return;
    }

    if (++g_framesSinceFileCheck < 120) {
        return;
    }
    g_framesSinceFileCheck = 0;

    const unsigned long long current = DocumentFileTime();
    if (current == 0 || current == g_documentFileTime) {
        return;
    }

    // Style sheets are cached by file name, so an edited RCSS would otherwise come back
    // from the cache unchanged and the reload would appear to do nothing.
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();

    if (g_document != nullptr) {
        OverlayShell::Unbind();
        g_document->Close();
        g_document = nullptr;
    }

    g_document = LoadOverlayDocument(g_documentFileTime);
    if (g_document != nullptr) {
        g_document->Show();
        OverlayShell::Bind(g_document);
        LogToFile(LogLevel::INFO, "Overlay UI: reloaded the document after an edit.");
    }
    else {
        LogToFile(LogLevel::WARNING, "Overlay UI: the edited document could not be loaded, the overlay is now empty.");
    }
}

bool LoadFont() {
    // A fixed family name, so the RCSS below does not depend on which of the candidates
    // was actually found -- the family a face reports is its own ("Segoe UI", "Arial"),
    // and styling against that would break the moment the fallback changed.
    for (const std::wstring& candidate : FontCandidates()) {
        if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }

        if (Rml::LoadFontFace(RmlWin32::ConvertToUTF8(candidate), "overlay-ui", Rml::Style::FontStyle::Normal)) {
            LogToFile(LogLevel::INFO, L"Overlay UI: using font " + candidate);
            return true;
        }

        LogToFile(LogLevel::WARNING, L"Overlay UI: found but could not load font " + candidate);
    }

    LogToFile(LogLevel::WARNING,
        L"Overlay UI: no usable font was found, so the overlay would draw no text. Place a TrueType font at "
        + GetIagdFolder() + L"overlay\\overlay.ttf to supply one.");
    return false;
}

}  // namespace

bool OverlayUi::IsInitialised() {
    return g_isInitialised;
}

bool OverlayUi::EnsureInitialised(ID3D11Device* device, int width, int height) {
    if (g_isInitialised) {
        return true;
    }

    if (g_hasFailed || device == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    // Pessimistic from the start: every failure path below leaves it set, so the only way
    // out is the success at the bottom.
    g_hasFailed = true;

    g_system = new HookSystemInterface();
    g_system->SetWindow(OverlayInput::Window());
    g_file = new HookFileInterface(GetIagdFolder());
    g_render = new HookRenderInterface(device);
    g_render->SetViewport(width, height);

    // Order matters: all three have to be in place before Initialise, which is when RmlUi
    // builds the font engine and the style sheet machinery that use them.
    g_textInput = new TextInputMethodEditor_Win32();

    Rml::SetSystemInterface(g_system);
    Rml::SetFileInterface(g_file);
    Rml::SetRenderInterface(g_render);
    Rml::SetTextInputHandler(g_textInput);

    if (!Rml::Initialise()) {
        LogToFile(LogLevel::WARNING, "Overlay UI: Rml::Initialise failed, the overlay will stay disabled.");
        Shutdown();
        return false;
    }

    // Not fatal on its own -- an empty panel is still evidence the render path works, and
    // the log already says why it is empty.
    LoadFont();

    g_context = Rml::CreateContext("overlay", Rml::Vector2i(width, height));
    if (g_context == nullptr) {
        LogToFile(LogLevel::WARNING, "Overlay UI: could not create the RmlUi context, the overlay will stay disabled.");
        Shutdown();
        return false;
    }

    g_document = LoadOverlayDocument(g_documentFileTime);
    if (g_document == nullptr) {
        LogToFile(LogLevel::WARNING, "Overlay UI: could not load the overlay document, the overlay will stay disabled.");
        Shutdown();
        return false;
    }

    g_document->Show();
    OverlayShell::Bind(g_document);

    g_width = width;
    g_height = height;
    g_hasFailed = false;
    g_isInitialised = true;

    LogToFile(LogLevel::INFO, "Overlay UI: RmlUi is up at "
        + std::to_string(width) + "x" + std::to_string(height) + ".");
    return true;
}

void OverlayUi::Render(ID3D11RenderTargetView* renderTarget, int width, int height) {
    if (!g_isInitialised || renderTarget == nullptr || width <= 0 || height <= 0) {
        return;
    }

    // The game can go to a different resolution or into a borderless window without
    // telling anyone; the render target it hands us is the authority on the size.
    if (width != g_width || height != g_height) {
        g_render->SetViewport(width, height);
        g_context->SetDimensions(Rml::Vector2i(width, height));
        g_width = width;
        g_height = height;
        LogToFile(LogLevel::INFO, "Overlay UI: resized to "
            + std::to_string(width) + "x" + std::to_string(height) + ".");
    }

    DrainInput();
    ReloadIfChanged();
    OverlayShell::Update();

    // Update before Render, and both inside the frame: Update settles layout and runs
    // animations, Render only submits what Update decided.
    g_context->Update();

    // Deliberately no Clear() -- the upstream backend clears because it owns the swap
    // chain and draws the whole window. Here the game's finished frame is already in that
    // target and clearing it would replace the game with a black screen. RmlUi draws into
    // its own layers and EndFrame composites them over what is there.
    g_render->BeginFrame();
    g_context->Render();
    g_render->EndFrame(renderTarget);
}

void OverlayUi::Shutdown() {
    // The shell holds a pointer to the document, so it lets go before RmlUi destroys it.
    OverlayShell::Unbind();

    // Documents and contexts belong to RmlUi; Shutdown takes them with it, and touching
    // them afterwards is a use-after-free.
    if (g_context != nullptr || g_isInitialised) {
        Rml::Shutdown();
    }

    g_document = nullptr;
    g_context = nullptr;

    // The interfaces have to outlive Rml::Shutdown -- it calls back into them while
    // releasing textures and fonts -- so they are destroyed only now.
    delete g_render;
    g_render = nullptr;
    delete g_file;
    g_file = nullptr;
    delete g_system;
    g_system = nullptr;
    delete g_textInput;
    g_textInput = nullptr;

    g_isInitialised = false;
    g_width = 0;
    g_height = 0;
}
