#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ItemSearch.h"
#include "OverlayTransfer.h"
#include "ReplicaText.h"

/// <summary>
/// One finished search, as the overlay wants to draw it: stacks rather than rows, and the
/// icon each one resolves to already looked up.
/// </summary>
struct OverlaySearchResult {
    /// Which request this answers. A result whose generation is older than the newest
    /// request is stale and is dropped rather than drawn.
    unsigned long long generation = 0;

    /// Rows collapsed into stacks, in the database's own order. The first row of each
    /// stack is the one the overlay shows; the rest are the other copies it stands for.
    std::vector<std::vector<iagd::ItemSearchRow>> stacks;

    /// Base record -> the bitmap path stored in the database, for the records in this
    /// result that have one. Records with no bitmap are absent rather than mapped to a
    /// placeholder, so the caller can tell "no icon" from "this icon".
    std::map<std::string, std::string> icons;

    /// True when the database had more rows than the cap. The overlay says so rather than
    /// pretending the collection ends there.
    bool wasTruncated = false;

    bool ok = false;
    std::string error;
};

typedef std::shared_ptr<OverlaySearchResult> OverlaySearchResultPtr;

/// <summary>
/// The stat text one item carries, as the game's own renderer produced it.
///
/// Fetched separately from the search: a page of results is a thousand items and only one
/// of them is ever being read, so pulling every item's tooltip rows with the list would be
/// most of the query time spent on text nobody looks at.
/// </summary>
struct OverlayItemDetail {
    int64_t playerItemId = 0;
    std::vector<iagd::ReplicaRow> rows;
    bool ok = false;
};

typedef std::shared_ptr<OverlayItemDetail> OverlayItemDetailPtr;

/// <summary>
/// The stat text for every item currently on screen, so they can be read side by side.
///
/// The single-item detail above exists because only one item was ever being read. Showing
/// stats on the cards themselves changes that: comparing two items means seeing both at
/// once, which is how the client's own item grid works. This is still not "every match" --
/// it is the visible page, which is why the shell pages the grid rather than rendering a
/// thousand cards.
/// </summary>
struct OverlayPageDetails {
    /// Which search these belong to. Rows for a page the player has already left are
    /// dropped rather than written into whatever is on screen now.
    unsigned long long generation = 0;

    std::map<int64_t, std::vector<iagd::ReplicaRow>> byItem;
    bool ok = false;
};

typedef std::shared_ptr<OverlayPageDetails> OverlayPageDetailsPtr;

/// <summary>
/// Runs the overlay's searches away from the frame.
///
/// A search over a played account's database is tens of milliseconds of SQLite work, which
/// is several dropped frames if it happens inside the present hook. So the frame only ever
/// posts a request and picks up whatever has finished; everything that touches the database
/// happens on this worker.
///
/// The same worker watches PRAGMA data_version, which is how the overlay notices that the
/// client has changed the collection underneath it -- a transfer, an import, a deletion --
/// without polling a file or re-running a search that would return the same rows.
/// </summary>
class OverlaySearch {
public:
    /// Opens the client's database read-only and starts the worker. Safe to call more than
    /// once; only the first call does anything. False means no database, which the caller
    /// reports as "no items available" rather than treating as a failure.
    static bool Start();

    /// Stops the worker and closes the database. Safe when Start was never called or failed.
    static void Stop();

    static bool IsReady();

    /// Why Start failed, for the message the overlay shows in place of results.
    static const std::string& StartError();

    /// <summary>
    /// Posts a search, superseding any request that has not finished yet.
    ///
    /// The world's mod and hardcore flag are filled in here rather than by the caller: a
    /// search that is not scoped to the world the player is standing in would offer them
    /// items they cannot be given, and a caller that has to remember to scope it is a
    /// caller that will eventually forget.
    ///
    /// Returns the generation this request was given.
    /// </summary>
    static unsigned long long Submit(const iagd::ItemSearchRequest& request, int skip, bool orderByLevel);

    /// <summary>
    /// Takes the newest finished result, or null when nothing has finished since the last
    /// call. Results older than the newest request are discarded here rather than drawn.
    /// </summary>
    static OverlaySearchResultPtr TakeResult();

    /// <summary>
    /// Asks for one item's stat rows. A second request supersedes the first: the player has
    /// moved to another item and the one they left is of no interest.
    /// </summary>
    static void SubmitDetail(int64_t playerItemId);

    /// The newest finished detail, or null when nothing has finished since the last call.
    static OverlayItemDetailPtr TakeDetail();

    /// <summary>
    /// Asks for the stat rows of every item on the visible page, in one query.
    ///
    /// Posted after the page is drawn rather than as part of the search, so the grid
    /// appears immediately and fills in its stats a moment later instead of waiting on a
    /// second query before showing anything.
    /// </summary>
    static void SubmitPageDetails(const std::vector<int64_t>& playerItemIds, unsigned long long generation);

    /// The newest finished page of stat rows, or null when there is none.
    static OverlayPageDetailsPtr TakePageDetails();

    /// <summary>
    /// Reads everything the game needs to rebuild one item.
    ///
    /// A separate query from the search because the search deliberately selects only the
    /// columns the overlay displays, and rebuilding an item needs the ones it does not --
    /// the modifier and transmute records, the relic and enchantment seeds, the reroll
    /// counts. Transferring is rare; carrying those on every row of every page is not free.
    /// </summary>
    static void SubmitTransferLookup(int64_t playerItemId);

    /// <summary>
    /// The finished lookup, ready to hand to OverlayTransfer, or null when there is none.
    /// The second field is false when the row could not be read.
    /// </summary>
    static bool TakeTransferRequest(OverlayTransferRequest& out);

    /// <summary>
    /// How many times another connection has committed to the database since the worker
    /// started. A change means the client has written; the overlay re-runs its search when
    /// this number moves, and does nothing while it does not.
    /// </summary>
    static unsigned long long DataVersionChanges();

    /// The generation of the most recently submitted request. A caller that has drawn a
    /// result with an older generation knows a newer one is on its way.
    static unsigned long long CurrentGeneration();
};
