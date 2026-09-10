#pragma once

#include <cstdint>
#include <string>

namespace GAME {
    // struct, not class: GrimTypes.h declares it that way and the two spellings are a
    // warning in every translation unit that sees both.
    struct GameEngine;
}

/// <summary>
/// One item the player has asked the in-game browser to hand them, carrying everything the
/// game needs to build it.
///
/// The fields are the columns of the client's PlayerItem row, in the shape
/// GAME::ItemReplicaInfo wants them. They are read on the search worker rather than on the
/// game thread, because reading them means reading the database.
/// </summary>
struct OverlayTransferRequest {
    int64_t playerItemId = 0;

    std::string baseRecord;
    std::string prefixRecord;
    std::string suffixRecord;
    std::string modifierRecord;
    std::string materiaRecord;
    std::string relicBonus;
    std::string enchantmentRecord;
    std::string transmuteRecord;
    std::string ascendant1;
    std::string ascendant2;

    unsigned int seed = 0;
    unsigned int relicSeed = 0;
    unsigned int enchantmentSeed = 0;
    unsigned int seedRerolls = 0;
    unsigned int affixRerolls = 0;
    unsigned int stackSize = 1;

    /// For the journal the client consumes, and for what the player is told.
    std::string name;
    std::string mod;
    bool isHardcore = false;
};

/// <summary>
/// Moves an item from the overlay into the player's transfer stash, and records that it
/// happened so the client can take it out of the collection.
///
/// Three threads meet here and each does only what it may:
///
///   the search worker reads the item's columns out of the database,
///   the render thread posts the request when the player clicks,
///   the game thread creates the item, places it, and writes the journal entry.
///
/// The ordering is deliberate and is the one design.md argues for: place the item first,
/// journal it second. A journal entry written before a placement that then fails costs the
/// player an item; an item placed but not journalled leaves them a duplicate they can
/// delete. The window is one atomic file publish wide.
/// </summary>
class OverlayTransfer {
public:
    /// Queues a request. Safe from any thread; the work happens on the next game tick.
    static void Request(const OverlayTransferRequest& request);

    /// <summary>
    /// Creates and places whatever is queued, then journals it.
    ///
    /// GAME THREAD ONLY. Does nothing when there is nothing queued, and holds the queue
    /// rather than emptying it when the world or the stash is not ready.
    /// </summary>
    static void ProcessPending(GAME::GameEngine* gameEngine);

    /// Whether anything is waiting. Read by the shell so it can say so.
    static bool HasPending();

    /// <summary>
    /// The last thing that happened to a transfer, for the overlay to show, or an empty
    /// string when there is nothing new. Taking it clears it.
    /// </summary>
    static std::string TakeStatus();

    /// Where journal entries are written, for the client to find them.
    static std::wstring JournalFolder();
};
