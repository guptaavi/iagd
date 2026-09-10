#include "stdafx.h"
#include "OverlayTransfer.h"
#include "GrimTypes.h"
#include "InventorySack_AddItem.h"
#include "Logger.h"

#include <codecvt>
#include <deque>
#include <fstream>
#include <mutex>
#include <objbase.h>
#include <sstream>

#include <boost/filesystem.hpp>

std::wstring GetIagdFolder();

namespace {

/// <summary>
/// The queue between the click and the game tick, and the one status line the overlay reads
/// back. Function-local for the reason given in dllmain.cpp: a mutex and a deque are
/// dynamically initialised, and this file's statics are reachable before its initialisers
/// have run.
/// </summary>
struct Pending {
    std::mutex mutex;
    std::deque<OverlayTransferRequest> queue;
    std::string status;

    /// So the "open your transfer stash" message is said once per request rather than once
    /// per game tick for as long as the player leaves it closed.
    bool hasAskedForStash = false;
};

Pending& pending() {
    static Pending instance;
    return instance;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }

    std::string utf8((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(), &utf8[0], size, nullptr, nullptr);
    return utf8;
}

std::wstring ToWide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }

    const int size = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }

    std::wstring wide((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), &wide[0], size);
    return wide;
}

void SetStatus(const std::string& text) {
    Pending& p = pending();
    std::lock_guard<std::mutex> guard(p.mutex);
    p.status = text;
}

/// A GUID as plain hex, which is what identifies an entry to the client so it can apply it
/// exactly once however many times it reads the folder.
std::string NewEntryId() {
    GUID guid = {};
    if (FAILED(::CoCreateGuid(&guid))) {
        return std::string();
    }

    wchar_t text[64] = {};
    if (::StringFromGUID2(guid, text, 64) == 0) {
        return std::string();
    }

    std::wstring id(text);

    // StringFromGUID2 wraps it in braces, which is one more thing for a file name and a
    // parser to disagree about.
    id.erase(std::remove(id.begin(), id.end(), L'{'), id.end());
    id.erase(std::remove(id.begin(), id.end(), L'}'), id.end());

    return ToUtf8(id);
}

/// <summary>
/// Turns a database row back into the structure the game builds items from.
///
/// The field order and meaning are GAME::Deserialize's, which is what reads the CSV the
/// client's own deposit path writes -- the same fields arriving by a different road.
/// stackSize defaults to 1 for the reason recorded there: an item whose GetStackSize() is
/// zero is invisible to the crafting system's ingredient count.
/// </summary>
GAME::ItemReplicaInfo ToReplica(const OverlayTransferRequest& request) {
    GAME::ItemReplicaInfo replica;

    replica.baseRecord = request.baseRecord;
    replica.prefixRecord = request.prefixRecord;
    replica.suffixRecord = request.suffixRecord;
    replica.modifierRecord = request.modifierRecord;
    replica.materiaRecord = request.materiaRecord;
    replica.relicBonus = request.relicBonus;
    replica.enchantmentRecord = request.enchantmentRecord;
    replica.transmuteRecord = request.transmuteRecord;
    replica.ascendant1 = request.ascendant1;
    replica.ascendant2 = request.ascendant2;

    replica.seed = request.seed;
    replica.relicSeed = request.relicSeed;
    replica.enchantmentSeed = request.enchantmentSeed;
    replica.seedRerolls = request.seedRerolls;
    replica.affixRerolls = request.affixRerolls;
    replica.stackSize = request.stackSize > 0 ? request.stackSize : 1;

    return replica;
}

/// <summary>
/// Publishes the journal entry that tells the client this item is now in the game and is to
/// be removed from the collection.
///
/// Written to a ".tmp" and moved into place, so the client either sees a whole entry or no
/// entry -- the same publish the client's own deposit path uses. Written only after the
/// game has confirmed the item was placed.
/// </summary>
bool WriteJournalEntry(const OverlayTransferRequest& request, const std::string& entryId) {
    const std::wstring folder = OverlayTransfer::JournalFolder();

    try {
        if (!boost::filesystem::is_directory(folder)) {
            boost::filesystem::create_directories(folder);
        }
    }
    catch (...) {
        LogToFile(LogLevel::FATAL, L"Transfer: could not create the journal folder " + folder);
        return false;
    }

    const std::wstring path = folder + L"\\" + ToWide(entryId) + L".txt";
    const std::wstring temporaryPath = path + L".tmp";

    {
        std::ofstream stream;
        stream.open(temporaryPath, std::ios::binary);
        if (!stream) {
            LogToFile(LogLevel::FATAL, L"Transfer: could not write " + temporaryPath);
            return false;
        }

        // A flat key=value file rather than the deposit path's CSV: this is not an item, it
        // is a note saying one was handed over, and it is read by different client code.
        stream << "\xEF\xBB\xBF";
        stream << "id=" << entryId << "\n";
        stream << "playeritemid=" << request.playerItemId << "\n";
        stream << "stacksize=" << (request.stackSize > 0 ? request.stackSize : 1) << "\n";
        stream << "baserecord=" << request.baseRecord << "\n";
        stream << "mod=" << request.mod << "\n";
        stream << "hardcore=" << (request.isHardcore ? 1 : 0) << "\n";
        stream << "source=ingame-browser\n";
        stream.flush();
    }

    if (!::MoveFileW(temporaryPath.c_str(), path.c_str())) {
        LogToFile(LogLevel::FATAL, L"Transfer: could not publish the journal entry " + path
            + L", error " + std::to_wstring(::GetLastError()));
        ::DeleteFileW(temporaryPath.c_str());
        return false;
    }

    LogToFile(LogLevel::INFO, L"Transfer: journalled " + path);
    return true;
}

}  // namespace

std::wstring OverlayTransfer::JournalFolder() {
    // Under itemqueue, beside the folders the client already watches, so a reader looking
    // for what the hook hands back finds all of it in one place.
    return GetIagdFolder() + L"itemqueue\\journal";
}

void OverlayTransfer::Request(const OverlayTransferRequest& request) {
    Pending& p = pending();

    {
        std::lock_guard<std::mutex> guard(p.mutex);
        p.queue.push_back(request);
        p.hasAskedForStash = false;
        p.status = "Waiting for the transfer stash...";
    }

    LogToFile(LogLevel::INFO, "Transfer: queued item " + std::to_string(request.playerItemId)
        + " (" + request.baseRecord + ")");
}

bool OverlayTransfer::HasPending() {
    Pending& p = pending();
    std::lock_guard<std::mutex> guard(p.mutex);
    return !p.queue.empty();
}

std::string OverlayTransfer::TakeStatus() {
    Pending& p = pending();
    std::lock_guard<std::mutex> guard(p.mutex);

    std::string status;
    status.swap(p.status);
    return status;
}

void OverlayTransfer::ProcessPending(GAME::GameEngine* gameEngine) {
    Pending& p = pending();

    {
        std::lock_guard<std::mutex> guard(p.mutex);
        if (p.queue.empty()) {
            return;
        }
    }

    // The world can go away between the click and this tick.
    if (!fnIsWorldAlive(gameEngine)) {
        return;
    }

    if (!InventorySack_AddItem::IsTransferStashOpen()) {
        bool shouldAsk = false;
        {
            std::lock_guard<std::mutex> guard(p.mutex);
            shouldAsk = !p.hasAskedForStash;
            p.hasAskedForStash = true;
            p.status = "Open your transfer stash to receive the item.";
        }

        if (shouldAsk) {
            // Held, not dropped: the player asked for the item and will get it as soon as
            // there is somewhere to put it.
            InventorySack_AddItem::ShowMessage(
                L"Open your transfer stash and the item will be placed in it", L"Item Assistant");
            LogToFile(LogLevel::INFO, "Transfer: holding a request until the transfer stash is opened.");
        }

        return;
    }

    if (!InventorySack_AddItem::CanPlaceItems() || fnCreateItem == nullptr) {
        SetStatus("The game's item creation is unavailable in this build.");
        LogToFile(LogLevel::WARNING, "Transfer: the game's item calls did not resolve, dropping the request.");

        std::lock_guard<std::mutex> guard(p.mutex);
        p.queue.clear();
        return;
    }

    GAME::InventorySack* sack = InventorySack_AddItem::GetSackToDepositTo(gameEngine);
    if (sack == nullptr) {
        SetStatus("The stash tab to transfer into could not be found.");
        return;
    }

    OverlayTransferRequest request;
    {
        std::lock_guard<std::mutex> guard(p.mutex);
        if (p.queue.empty()) {
            return;
        }
        request = p.queue.front();
        p.queue.pop_front();
    }

    try {
        GAME::ItemReplicaInfo replica = ToReplica(request);
        GAME::Item* item = fnCreateItem(&replica);

        if (item == nullptr) {
            // The usual cause is a mod item being created in a world that does not have
            // that mod's records, which the search is scoped to prevent -- so if this
            // happens the scope is wrong, and saying so is more useful than a silent drop.
            SetStatus("The game could not create that item here.");
            InventorySack_AddItem::ShowMessage(L"Could not create that item", L"Item Assistant");
            LogToFile(LogLevel::WARNING, "Transfer: fnCreateItem returned null for " + request.baseRecord);
            return;
        }

        GAME::Rect position;
        if (!InventorySack_AddItem::PlaceInSack(sack, item, &position)) {
            SetStatus("That stash tab is full.");
            InventorySack_AddItem::ShowMessage(L"The stash tab is full", L"Item Assistant");
            LogToFile(LogLevel::INFO, "Transfer: no free position in the destination sack.");
            return;
        }

        // Placed. Only now is it safe to tell the client the item has left the collection.
        const std::string entryId = NewEntryId();
        if (entryId.empty() || !WriteJournalEntry(request, entryId)) {
            // The player has the item; the client has not been told. That is the failure
            // this ordering deliberately prefers -- a duplicate they can delete rather than
            // an item that no longer exists anywhere.
            SetStatus("Transferred, but the removal could not be recorded -- the item may still show in the client.");
            LogToFile(LogLevel::FATAL, "Transfer: placed the item but could not journal it, the client will still list it.");
            return;
        }

        SetStatus("Transferred to your stash.");
        InventorySack_AddItem::ShowMessage(
            request.name.empty() ? L"An item was transferred" : (ToWide(request.name) + L" was transferred"),
            L"Item Assistant");
    }
    catch (const std::exception& ex) {
        SetStatus("The transfer failed.");
        LogToFile(LogLevel::FATAL, std::string("Transfer: exception while placing an item. ") + ex.what());
    }
    catch (...) {
        SetStatus("The transfer failed.");
        LogToFile(LogLevel::FATAL, "Transfer: unknown exception while placing an item.");
    }
}
