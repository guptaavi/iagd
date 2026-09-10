#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace iagd {

class SqliteDb;
class SqliteQuery;

/// <summary>
/// A comparison against a pre-computed, seed-applied stat value, matching the client's
/// StatValueFilter. The listed fields are summed before the comparison, the same way the
/// checkbox that produced them contributes several stat names to one number.
/// </summary>
struct StatValueFilter {
    enum class Op { GreaterThan, GreaterOrEqual, LessThan, LessOrEqual, Equal };

    std::vector<std::string> fields;
    Op op = Op::GreaterOrEqual;
    double threshold = 0.0;
};

/// <summary>
/// The overlay's mirror of the client's ItemSearchRequest.
///
/// A deliberate subset: the overlay offers fewer filters than the client, but must never
/// interpret a shared one differently. Anything added here has to be added to
/// IAGrim/Database/Dto/ItemSearchRequest.cs's meaning, not just its name.
/// </summary>
struct ItemSearchRequest {
    std::string wildcard;

    /// Each entry is one checkbox: a set of stat names, any of which satisfies it.
    /// Separate entries are ANDed, matching the client.
    std::vector<std::vector<std::string>> filters;

    std::vector<StatValueFilter> statValueFilters;

    float minimumLevel = 0.0f;
    float maximumLevel = 0.0f;

    std::string rarity;
    int prefixRarity = 0;

    /// Equipment slots ("Class" text values). Several for things like two-handers.
    std::vector<std::string> slot;
    bool slotInverse = false;

    bool petBonuses = false;
    bool hasPetBonus = false;
    bool isRetaliation = false;
    bool duplicatesOnly = false;
    bool socketedOnly = false;
    bool recentOnly = false;
    bool withGrantSkillsOnly = false;
    bool withSummonerSkillOnly = false;

    std::vector<std::string> classes;

    /// Taken from the game, never chosen by the player: an item that cannot be transferred
    /// into the world they are standing in is not worth showing them.
    std::string mod;
    bool isHardcore = false;
};

/// One row of a search result. Only the columns the overlay actually displays.
struct ItemSearchRow {
    int64_t id = 0;
    std::string name;

    /// A missing name is distinct from an empty one: SQLite orders NULL before '', so the
    /// two are not interchangeable when reasoning about result order. The UI shows
    /// "Unknown" for either, matching the client's WebUI.
    bool nameIsNull = false;
    std::string rarity;
    std::string baseRecord;
    std::string prefixRecord;
    std::string suffixRecord;
    std::string materiaRecord;
    std::string mod;
    int64_t stackCount = 0;
    float levelRequirement = 0.0f;
    int64_t seed = 0;
    bool isHardcore = false;
};

/// <summary>
/// Builds and runs the player-item search.
///
/// This is a deliberate, literal port of PlayerItemDaoImpl.SearchForItems in
/// IAGrim/Database/DAO/PlayerItemDaoImpl.cs. The SQL text is kept as close to the original
/// as possible, including its ":name" placeholders, so that the two can be diffed by eye
/// when either changes. Three details in it are load-bearing and are called out where they
/// appear: the sargable retaliation prefix range, the IFNULL wrapping in the pet-record
/// condition, and the deterministic ordering that makes paging stable.
///
/// If you change this file, change the C# one too, and vice versa.
/// </summary>
class ItemSearch {
public:
    /// Matches PlayerItemDaoImpl.MaxSearchResults.
    static const int MaxSearchResults = 1000;

    /// Builds the SQL for a request. Exposed so the offline tests can inspect and compare
    /// it without running it.
    static std::string BuildSql(const ItemSearchRequest& request, int skip, bool orderByLevel);

    /// Binds the request's parameters onto a prepared-but-not-yet-prepared query. Call
    /// before SqliteQuery::Prepare, which is when list parameters are expanded.
    static void BindParams(SqliteQuery& query, const ItemSearchRequest& request);

    /// Runs the search. Returns false on failure, with the reason in outError.
    static bool Run(SqliteDb& db, const ItemSearchRequest& request, int skip, bool orderByLevel,
                    std::vector<ItemSearchRow>& outRows, bool& outWasTruncated, std::string& outError);

    /// <summary>
    /// Collapses identical items into stacks, ported from
    /// IAGrim/Database/DAO/Util/ItemOperationsUtility.MergeStackSize.
    ///
    /// Identity is base + prefix + suffix record, with a missing record treated as empty.
    /// The component (materia) is deliberately NOT part of it: the client merges on those
    /// three so that a stack of the same item with different components shows as one entry,
    /// and the transfer path has to match that or items with components are left behind.
    ///
    /// First-appearance order is preserved, so a result already sorted by name stays
    /// sorted after merging.
    /// </summary>
    static std::vector<std::vector<ItemSearchRow>> MergeStacks(const std::vector<ItemSearchRow>& rows);

private:
    static std::string ToSqlOperator(StatValueFilter::Op op);
    static std::string RecordStatSubquery(const std::string& dbsCondition, bool petOnly);
    static std::string ToLowerAscii(const std::string& value);
};

}  // namespace iagd
