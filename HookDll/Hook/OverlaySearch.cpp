#include "stdafx.h"
#include "OverlaySearch.h"
#include "GameContext.h"
#include "ItemIcons.h"
#include "Logger.h"
#include "SqliteDb.h"
#include "DataQueue.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

std::wstring GetIagdFolder();

namespace {

/// The database the client keeps the player's collection in. The DLL only ever reads it;
/// the client is the only writer.
const wchar_t* const kDatabaseRelativePath = L"data\\userdata.db";

/// How long the worker waits for a request before looking at PRAGMA data_version instead.
/// Fast enough that an item transferred in the client shows up in the overlay while the
/// player is still looking at it, slow enough to be nothing on a database connection.
const int kIdlePollMilliseconds = 1000;

/// <summary>
/// Everything the worker and the frame share. Function-local for the reason given in
/// dllmain.cpp: a mutex, a thread and a queue are all dynamically initialised, and static
/// initialisation order across translation units is unspecified.
/// </summary>
struct Worker {
    std::mutex mutex;
    std::condition_variable wakeUp;
    std::thread thread;

    bool isRunning = false;
    bool shouldStop = false;

    /// The request the worker should run next, and the generation it was given. A second
    /// request arriving before the first has run simply overwrites it: the player has
    /// typed another letter, and the older search is of no interest to anyone.
    bool hasPendingRequest = false;
    iagd::ItemSearchRequest pendingRequest;
    int pendingSkip = 0;
    bool pendingOrderByLevel = false;

    /// The item whose stat rows are wanted, or 0. Separate from the search request so
    /// that selecting an item does not cancel the list it was selected from.
    bool hasPendingDetail = false;
    int64_t pendingDetailId = 0;

    /// The item the player has asked to be handed. Separate again from the detail:
    /// reading an item's stat text and rebuilding it are different queries wanted at
    /// different moments, and neither should cancel the other.
    bool hasPendingTransfer = false;
    int64_t pendingTransferId = 0;

    std::atomic<unsigned long long> generation{0};
    std::atomic<unsigned long long> dataVersionChanges{0};

    /// Finished results, handed to the frame through the DLL's own queue type rather than
    /// a second hand-rolled one.
    BaseDataQueue<OverlaySearchResultPtr> results;
    BaseDataQueue<OverlayItemDetailPtr> details;
    BaseDataQueue<std::shared_ptr<OverlayTransferRequest>> transfers;

    iagd::SqliteDb db;
    std::string startError;
    bool isReady = false;
};

Worker& worker() {
    static Worker instance;
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

/// <summary>
/// Collects the base, prefix and suffix records a result refers to, so their icons can be
/// resolved in one query instead of one per row.
/// </summary>
std::vector<std::string> RecordsIn(const std::vector<std::vector<iagd::ItemSearchRow>>& stacks) {
    std::vector<std::string> records;
    records.reserve(stacks.size());

    for (const auto& stack : stacks) {
        if (!stack.empty() && !stack.front().baseRecord.empty()) {
            records.push_back(stack.front().baseRecord);
        }
    }

    return records;
}

void RunOne(Worker& w, const iagd::ItemSearchRequest& request, int skip, bool orderByLevel, unsigned long long generation) {
    OverlaySearchResultPtr result(new OverlaySearchResult());
    result->generation = generation;

    std::vector<iagd::ItemSearchRow> rows;
    result->ok = iagd::ItemSearch::Run(w.db, request, skip, orderByLevel, rows, result->wasTruncated, result->error);

    if (result->ok) {
        result->stacks = iagd::ItemSearch::MergeStacks(rows);

        // Superseded while the query ran. The icon lookup is a second query, and running it
        // for a result nobody will draw is the one place this worker can waste real time.
        if (w.generation.load() != generation) {
            return;
        }

        result->icons = iagd::ItemIcons::Resolve(w.db, RecordsIn(result->stacks));
    }
    else {
        LogToFile(LogLevel::WARNING, "Overlay search failed: " + result->error);
    }

    if (w.generation.load() != generation) {
        return;
    }

    w.results.push(result);
}

/// <summary>
/// Reads one item's stored tooltip rows.
///
/// Ordered by the row's own id, which is the order the client inserted them and therefore
/// the order the game emitted them. Nothing else in the table records their sequence, and
/// the rows only make sense in sequence -- a stat sits under the header that introduces it.
/// </summary>
void RunDetail(Worker& w, int64_t playerItemId) {
    OverlayItemDetailPtr detail(new OverlayItemDetail());
    detail->playerItemId = playerItemId;

    iagd::SqliteQuery query(w.db);
    query.SetParam("playeritemid", playerItemId);

    if (!query.Prepare(
            "SELECT row.Type, row.Text FROM ReplicaItemRow row"
            " JOIN ReplicaItem2 replica ON replica.Id = row.replicaitemid"
            " WHERE replica.playeritemid = :playeritemid"
            " ORDER BY row.Id")) {
        LogToFile(LogLevel::WARNING, "Overlay detail failed: " + query.LastError());
        w.details.push(detail);
        return;
    }

    while (query.Step()) {
        iagd::ReplicaRow row;
        row.type = (int)query.GetInt64(0);
        row.text = query.GetText(1);
        detail->rows.push_back(row);
    }

    detail->ok = true;
    w.details.push(detail);
}

/// <summary>
/// Reads the columns the game needs to rebuild an item, in the shape ItemReplicaInfo
/// wants them.
///
/// The column list is the client's own PlayerItem table, and the mapping is the one
/// GAME::Deserialize applies to the CSV the client's deposit path writes -- the same
/// fields arriving by a different road, so an item transferred from the overlay is the
/// same item the client would have handed over.
/// </summary>
void RunTransferLookup(Worker& w, int64_t playerItemId) {
    std::shared_ptr<OverlayTransferRequest> request(new OverlayTransferRequest());
    request->playerItemId = playerItemId;

    iagd::SqliteQuery query(w.db);
    query.SetParam("id", playerItemId);

    if (!query.Prepare(
            "SELECT baserecord, IFNULL(PrefixRecord,''), IFNULL(SuffixRecord,''),"
            " IFNULL(ModifierRecord,''), IFNULL(MateriaRecord,''), IFNULL(RelicCompletionBonusRecord,''),"
            " IFNULL(EnchantmentRecord,''), IFNULL(TransmuteRecord,''),"
            " IFNULL(AscendantAffixNameRecord,''), IFNULL(AscendantAffix2hNameRecord,''),"
            " IFNULL(Seed,0), IFNULL(RelicSeed,0), IFNULL(EnchantmentSeed,0),"
            " IFNULL(RerollsUsed,0), IFNULL(AffixRerollsUsed,0), IFNULL(StackCount,1),"
            " IFNULL(Name,''), IFNULL(Mod,''), IsHardcore"
            " FROM PlayerItem WHERE Id = :id")) {
        LogToFile(LogLevel::WARNING, "Overlay transfer lookup failed: " + query.LastError());
        return;
    }

    if (!query.Step()) {
        // The client deleted it between the search and the click.
        LogToFile(LogLevel::WARNING, "Overlay transfer lookup: item " + std::to_string(playerItemId)
            + " is no longer in the database.");
        return;
    }

    int column = 0;
    request->baseRecord = query.GetText(column++);
    request->prefixRecord = query.GetText(column++);
    request->suffixRecord = query.GetText(column++);
    request->modifierRecord = query.GetText(column++);
    request->materiaRecord = query.GetText(column++);
    request->relicBonus = query.GetText(column++);
    request->enchantmentRecord = query.GetText(column++);
    request->transmuteRecord = query.GetText(column++);
    request->ascendant1 = query.GetText(column++);
    request->ascendant2 = query.GetText(column++);
    request->seed = (unsigned int)query.GetInt64(column++);
    request->relicSeed = (unsigned int)query.GetInt64(column++);
    request->enchantmentSeed = (unsigned int)query.GetInt64(column++);
    request->seedRerolls = (unsigned int)query.GetInt64(column++);
    request->affixRerolls = (unsigned int)query.GetInt64(column++);
    request->stackSize = (unsigned int)query.GetInt64(column++);
    request->name = query.GetText(column++);
    request->mod = query.GetText(column++);
    request->isHardcore = query.GetInt64(column++) != 0;

    w.transfers.push(request);
}

void PollDataVersion(Worker& w) {
    static int lastVersion = 0;
    static bool hasVersion = false;

    int version = 0;
    if (!w.db.ReadDataVersion(version)) {
        return;
    }

    if (!hasVersion) {
        hasVersion = true;
        lastVersion = version;
        return;
    }

    if (version != lastVersion) {
        lastVersion = version;
        w.dataVersionChanges.fetch_add(1);
        LogToFile(LogLevel::INFO, "Overlay search: the client has written to the database, the overlay will refresh.");
    }
}

void WorkerMain() {
    Worker& w = worker();

    for (;;) {
        iagd::ItemSearchRequest request;
        int skip = 0;
        bool orderByLevel = false;
        unsigned long long generation = 0;
        bool hasWork = false;
        bool hasDetail = false;
        int64_t detailId = 0;
        bool hasTransfer = false;
        int64_t transferId = 0;

        {
            std::unique_lock<std::mutex> lock(w.mutex);
            w.wakeUp.wait_for(lock, std::chrono::milliseconds(kIdlePollMilliseconds),
                [&w] { return w.shouldStop || w.hasPendingRequest || w.hasPendingDetail || w.hasPendingTransfer; });

            if (w.shouldStop) {
                return;
            }

            if (w.hasPendingRequest) {
                w.hasPendingRequest = false;
                request = w.pendingRequest;
                skip = w.pendingSkip;
                orderByLevel = w.pendingOrderByLevel;
                generation = w.generation.load();
                hasWork = true;
            }

            if (w.hasPendingDetail) {
                w.hasPendingDetail = false;
                detailId = w.pendingDetailId;
                hasDetail = true;
            }

            if (w.hasPendingTransfer) {
                w.hasPendingTransfer = false;
                transferId = w.pendingTransferId;
                hasTransfer = true;
            }
        }

        try {
            // The detail first: it is the small query, and it is the one the player is
            // waiting to read. A search behind it is still a search they have already seen
            // the beginning of.
            // The transfer first of all: the player has clicked a button and is waiting
            // to see an item appear in their stash.
            if (hasTransfer) {
                RunTransferLookup(w, transferId);
            }

            if (hasDetail) {
                RunDetail(w, detailId);
            }

            if (hasWork) {
                RunOne(w, request, skip, orderByLevel, generation);
            }
            else if (!hasDetail && !hasTransfer) {
                // Only while idle. A search is already a read of the database, so there is
                // nothing to learn from asking about its version in the same breath.
                PollDataVersion(w);
            }
        }
        catch (const std::exception& ex) {
            LogToFile(LogLevel::WARNING, std::string("Overlay search worker: ") + ex.what());
        }
        catch (...) {
            LogToFile(LogLevel::WARNING, "Overlay search worker: unknown error.");
        }
    }
}

}  // namespace

bool OverlaySearch::Start() {
    Worker& w = worker();

    if (w.isRunning) {
        return w.isReady;
    }

    const std::wstring path = GetIagdFolder() + kDatabaseRelativePath;

    if (!w.db.OpenReadOnly(ToUtf8(path))) {
        w.startError = w.db.LastError();
        LogToFile(LogLevel::WARNING, L"Overlay search: could not open " + path
            + L" -- the overlay will report that no items are available.");
        LogToFile(LogLevel::WARNING, "Overlay search: " + w.startError);
        return false;
    }

    w.shouldStop = false;
    w.isReady = true;
    w.isRunning = true;
    w.thread = std::thread(WorkerMain);

    LogToFile(LogLevel::INFO, L"Overlay search: reading " + path);
    return true;
}

void OverlaySearch::Stop() {
    Worker& w = worker();

    if (!w.isRunning) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(w.mutex);
        w.shouldStop = true;
    }
    w.wakeUp.notify_all();

    if (w.thread.joinable()) {
        w.thread.join();
    }

    // Only once the worker has stopped: it is the only thing that touches the connection,
    // and closing it underneath a running query is how a read-only connection still
    // manages to crash a process.
    w.db.Close();
    w.isRunning = false;
    w.isReady = false;

    LogToFile(LogLevel::INFO, "Overlay search: worker stopped and the database closed.");
}

bool OverlaySearch::IsReady() {
    return worker().isReady;
}

const std::string& OverlaySearch::StartError() {
    return worker().startError;
}

unsigned long long OverlaySearch::Submit(const iagd::ItemSearchRequest& request, int skip, bool orderByLevel) {
    Worker& w = worker();

    iagd::ItemSearchRequest scoped = request;

    // Taken from the game, never from the caller. An item from another mod, or from the
    // other side of the hardcore line, cannot be transferred into the world the player is
    // standing in, so showing it would only be an invitation to try.
    std::wstring modName;
    bool isHardcore = false;
    if (GameContext::TryGet(modName, isHardcore)) {
        scoped.mod = ToUtf8(modName);
        scoped.isHardcore = isHardcore;
    }

    const unsigned long long generation = w.generation.fetch_add(1) + 1;

    {
        std::lock_guard<std::mutex> guard(w.mutex);
        w.pendingRequest = scoped;
        w.pendingSkip = skip;
        w.pendingOrderByLevel = orderByLevel;
        w.hasPendingRequest = true;
    }
    w.wakeUp.notify_all();

    return generation;
}

OverlaySearchResultPtr OverlaySearch::TakeResult() {
    Worker& w = worker();

    OverlaySearchResultPtr newest;
    while (!w.results.empty()) {
        OverlaySearchResultPtr candidate = w.results.pop();

        // A result the player has already typed past. Dropping it here rather than drawing
        // it is what keeps a slow query from flashing an old list over a new one.
        if (candidate != nullptr && candidate->generation >= w.generation.load()) {
            newest = candidate;
        }
    }

    return newest;
}

void OverlaySearch::SubmitDetail(int64_t playerItemId) {
    Worker& w = worker();

    {
        std::lock_guard<std::mutex> guard(w.mutex);
        w.pendingDetailId = playerItemId;
        w.hasPendingDetail = true;
    }
    w.wakeUp.notify_all();
}

OverlayItemDetailPtr OverlaySearch::TakeDetail() {
    Worker& w = worker();

    OverlayItemDetailPtr newest;
    while (!w.details.empty()) {
        newest = w.details.pop();
    }

    return newest;
}

void OverlaySearch::SubmitTransferLookup(int64_t playerItemId) {
    Worker& w = worker();

    {
        std::lock_guard<std::mutex> guard(w.mutex);
        w.pendingTransferId = playerItemId;
        w.hasPendingTransfer = true;
    }
    w.wakeUp.notify_all();
}

bool OverlaySearch::TakeTransferRequest(OverlayTransferRequest& out) {
    Worker& w = worker();

    if (w.transfers.empty()) {
        return false;
    }

    std::shared_ptr<OverlayTransferRequest> request = w.transfers.pop();
    if (request == nullptr) {
        return false;
    }

    out = *request;
    return true;
}

unsigned long long OverlaySearch::DataVersionChanges() {
    return worker().dataVersionChanges.load();
}

unsigned long long OverlaySearch::CurrentGeneration() {
    return worker().generation.load();
}
