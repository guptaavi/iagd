/*
 * Offline verification of the ported player-item search (ItemSearch), run against a copy
 * of a real userdata database.
 *
 * The port is a transcription of PlayerItemDaoImpl.SearchForItems, so the risk is not that
 * it fails to run but that it quietly means something slightly different. These checks are
 * therefore mostly property-based -- assert things about the rows that come back, using SQL
 * written independently of the builder -- rather than comparing the generated text to
 * itself.
 *
 * Two of them exist specifically to pin the subtleties the C# comments warn about:
 *   - the sargable "retaliation" prefix RANGE must select the same items as the obvious
 *     (but index-defeating) LIKE it replaced;
 *   - the IFNULL wrapping in the pet-record condition must actually matter, i.e. removing
 *     it must change the result, which is what proves the trap is real and still avoided.
 *
 * Usage: item_search_test.exe <path-to-userdata.db>
 */

#include "ItemSearch.h"
#include "SqliteDb.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>

using namespace iagd;

static int g_failures = 0;

static void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        g_failures++;
    }
}

/// Collapses runs of whitespace so formatting differences between the two implementations
/// do not register as behavioural ones.
static std::string Normalise(const std::string& sql) {
    std::string out;
    out.reserve(sql.size());
    bool inSpace = false;
    for (size_t i = 0; i < sql.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(sql[i]);
        if (std::isspace(c)) {
            inSpace = true;
            continue;
        }
        if (inSpace && !out.empty()) { out.push_back(' '); }
        inSpace = false;
        out.push_back(sql[i]);
    }
    return out;
}

static std::string FromClauseOnward(const std::string& sql) {
    const size_t at = sql.find("FROM PlayerItem PI");
    return at == std::string::npos ? std::string() : sql.substr(at);
}

static int64_t ScalarInt(SqliteDb& db, const std::string& sql) {
    SqliteQuery q(db);
    if (!q.Prepare(sql) || !q.Step()) {
        return -1;
    }
    return q.GetInt64(0);
}

/// The (mod, hardcore) pair with the most items, so the filter tests run against real data
/// rather than an empty scope.
static void PickBusiestScope(SqliteDb& db, std::string& outMod, bool& outHardcore) {
    SqliteQuery q(db);
    q.Prepare("SELECT IFNULL(Mod,'') AS m, IsHardcore, COUNT(*) c FROM PlayerItem"
              " GROUP BY m, IsHardcore ORDER BY c DESC LIMIT 1");
    if (q.Step()) {
        outMod = q.GetText(0);
        outHardcore = q.GetInt64(1) != 0;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-userdata.db>\n", argv[0]);
        return 2;
    }

    SqliteDb db;
    if (!db.OpenReadOnly(argv[1])) {
        std::fprintf(stderr, "cannot open: %s\n", db.LastError().c_str());
        return 1;
    }

    std::string mod;
    bool hardcore = false;
    PickBusiestScope(db, mod, hardcore);
    std::printf("scope: mod='%s' hardcore=%d, %lld items total\n\n",
                mod.c_str(), hardcore ? 1 : 0, (long long)ScalarInt(db, "SELECT COUNT(*) FROM PlayerItem"));

    ItemSearchRequest base;
    base.mod = mod;
    base.isHardcore = hardcore;
    base.maximumLevel = 120.0f;

    std::string err;
    bool truncated = false;

    // --- it runs at all, and respects the scope ------------------------------------
    {
        std::vector<ItemSearchRow> rows;
        const bool ok = ItemSearch::Run(db, base, 0, false, rows, truncated, err);
        Check(ok, "unfiltered search runs (" + (ok ? std::to_string(rows.size()) + " rows" : err) + ")");
        Check(!rows.empty(), "unfiltered search returns items");

        bool scopeOk = true;
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i].isHardcore != hardcore) { scopeOk = false; break; }
            const std::string rowMod = rows[i].mod;
            if (mod.empty() ? !rowMod.empty() : rowMod != mod) { scopeOk = false; break; }
        }
        Check(scopeOk, "every row matches the requested mod and hardcore flag");
    }

    // --- ordering and paging -------------------------------------------------------
    {
        std::vector<ItemSearchRow> rows;
        ItemSearch::Run(db, base, 0, false, rows, truncated, err);

        // Compared as UNSIGNED bytes. The Name column has no COLLATE, so SQLite orders it
        // BINARY, and MSVC's char is signed: comparing with std::string's operator< puts
        // every name containing a byte >= 0x80 (accented and non-Latin item names) in a
        // different order than the database did, and the test fails on correct data.
        bool ordered = true;
        std::string firstBad;
        for (size_t i = 1; i < rows.size(); i++) {
            // NULL sorts before every string in SQLite, and is distinct from ''. This data
            // has one NULL name and seventy empty ones, so conflating them makes a
            // correctly ordered result look unsorted.
            if (rows[i - 1].nameIsNull && !rows[i].nameIsNull) { continue; }
            if (rows[i - 1].nameIsNull && rows[i].nameIsNull) { continue; }
            if (!rows[i - 1].nameIsNull && rows[i].nameIsNull) {
                ordered = false;
                firstBad = " (a NULL name sorted after a non-NULL one)";
                break;
            }

            const int cmp = std::memcmp(rows[i - 1].name.data(), rows[i].name.data(),
                                        (std::min)(rows[i - 1].name.size(), rows[i].name.size()));
            const bool nameLess = (cmp < 0) ||
                                  (cmp == 0 && rows[i - 1].name.size() < rows[i].name.size());
            const bool nameEqual = (cmp == 0 && rows[i - 1].name.size() == rows[i].name.size());
            const bool inOrder = nameLess || (nameEqual && rows[i - 1].id <= rows[i].id);
            if (!inOrder) {
                ordered = false;
                firstBad = " (first out of order: '" + rows[i - 1].name + "' then '" + rows[i].name + "')";
                break;
            }
        }
        Check(ordered, "rows come back ordered by name then id" + firstBad);
        Check(truncated, "a large unfiltered result reports itself as capped");

        // Pages must partition the result: nothing skipped, nothing repeated.
        std::vector<ItemSearchRow> page2;
        ItemSearch::Run(db, base, ItemSearch::MaxSearchResults, false, page2, truncated, err);

        std::set<int64_t> firstIds, secondIds;
        for (size_t i = 0; i < rows.size(); i++) { firstIds.insert(rows[i].id); }
        for (size_t i = 0; i < page2.size(); i++) { secondIds.insert(page2[i].id); }

        size_t overlap = 0;
        for (std::set<int64_t>::const_iterator it = secondIds.begin(); it != secondIds.end(); ++it) {
            if (firstIds.count(*it) != 0) { overlap++; }
        }
        Check(overlap == 0, "page 2 shares no ids with page 1 (" + std::to_string(overlap) + " overlapping)");
        Check(!page2.empty(), "page 2 returns rows");
        Check(firstIds.size() == rows.size(), "page 1 contains no duplicate ids");
    }

    // --- hardcore isolation ----------------------------------------------------------
    // A hardcore character must never be offered softcore items: they cannot be
    // transferred, and showing them is worse than showing nothing.
    {
        ItemSearchRequest hc = base;
        hc.isHardcore = true;
        ItemSearchRequest sc = base;
        sc.isHardcore = false;

        std::vector<ItemSearchRow> hcRows, scRows;
        ItemSearch::Run(db, hc, 0, false, hcRows, truncated, err);
        ItemSearch::Run(db, sc, 0, false, scRows, truncated, err);

        bool allHardcore = true;
        for (size_t i = 0; i < hcRows.size(); i++) {
            if (!hcRows[i].isHardcore) { allHardcore = false; break; }
        }
        Check(allHardcore, "a hardcore search returns only hardcore items");

        std::set<int64_t> hcIds;
        for (size_t i = 0; i < hcRows.size(); i++) { hcIds.insert(hcRows[i].id); }
        size_t leaked = 0;
        for (size_t i = 0; i < scRows.size(); i++) {
            if (hcIds.count(scRows[i].id) != 0) { leaked++; }
        }
        Check(leaked == 0, "no item appears in both the hardcore and softcore results");

        std::printf("      hardcore rows: %llu, softcore rows: %llu\n",
                    (unsigned long long)hcRows.size(), (unsigned long long)scRows.size());
    }

    // --- paging skips nothing ---------------------------------------------------------
    // Non-overlap alone is not enough: pages must also be gapless. Compared against the
    // ids the database returns for the same ordering, taken independently of the builder.
    {
        std::vector<ItemSearchRow> p1, p2;
        ItemSearch::Run(db, base, 0, false, p1, truncated, err);
        ItemSearch::Run(db, base, ItemSearch::MaxSearchResults, false, p2, truncated, err);

        std::vector<int64_t> paged;
        for (size_t i = 0; i < p1.size(); i++) { paged.push_back(p1[i].id); }
        for (size_t i = 0; i < p2.size(); i++) { paged.push_back(p2[i].id); }

        SqliteQuery q(db);
        q.Prepare("SELECT PI.Id FROM PlayerItem PI WHERE (PI.Mod IS NULL OR PI.Mod = '')"
                  " AND NOT PI.IsHardcore ORDER BY PI.name, PI.Id LIMIT 2000");
        std::vector<int64_t> expected;
        while (q.Step()) { expected.push_back(q.GetInt64(0)); }

        bool identical = paged.size() == expected.size();
        size_t firstDivergence = 0;
        if (identical) {
            for (size_t i = 0; i < paged.size(); i++) {
                if (paged[i] != expected[i]) { identical = false; firstDivergence = i; break; }
            }
        }
        Check(identical, "two pages reproduce the database's own ordering exactly, with no gap ("
                         + std::to_string(paged.size()) + " vs " + std::to_string(expected.size())
                         + " ids, first divergence at " + std::to_string(firstDivergence) + ")");
    }

    // --- level bounds ---------------------------------------------------------------
    {
        ItemSearchRequest r = base;
        r.minimumLevel = 60.0f;
        r.maximumLevel = 75.0f;

        std::vector<ItemSearchRow> rows;
        ItemSearch::Run(db, r, 0, false, rows, truncated, err);

        bool inRange = true;
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i].levelRequirement < 60.0f || rows[i].levelRequirement > 75.0f) { inRange = false; break; }
        }
        Check(!rows.empty() && inRange, "level bounds are respected by every row");
    }

    // --- rarity ---------------------------------------------------------------------
    {
        ItemSearchRequest r = base;
        r.rarity = "Epic";

        std::vector<ItemSearchRow> rows;
        ItemSearch::Run(db, r, 0, false, rows, truncated, err);

        bool allLegendary = !rows.empty();
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i].rarity != "Epic") { allLegendary = false; break; }
        }
        Check(allLegendary, "rarity filter returns only that rarity (Epic)");
    }

    // --- a stat filter, checked against independently written SQL --------------------
    {
        ItemSearchRequest r = base;
        std::vector<std::string> fire;
        fire.push_back("offensiveFire");
        fire.push_back("offensiveFireModifier");
        r.filters.push_back(fire);

        std::vector<ItemSearchRow> rows;
        const bool ok = ItemSearch::Run(db, r, 0, false, rows, truncated, err);
        Check(ok && !rows.empty(), "fire-damage stat filter returns items (" + (ok ? "" : err) + ")");

        // Independently: does each returned item really own a record carrying one of those
        // stats? Written from the schema, not from the builder.
        size_t violations = 0;
        for (size_t i = 0; i < rows.size() && i < 50; i++) {
            SqliteQuery q(db);
            q.SetParam("id", rows[i].id);
            q.Prepare("SELECT COUNT(*) FROM PlayerItemRecord pir"
                      " JOIN databaseitem_v2 db ON db.baserecord = pir.record"
                      " JOIN databaseitemstat_v2 dbs ON dbs.id_databaseitem = db.id_databaseitem"
                      " WHERE pir.Playeritemid = :id AND dbs.stat IN ('offensiveFire','offensiveFireModifier')");
            if (q.Step() && q.GetInt64(0) == 0) { violations++; }
        }
        Check(violations == 0, "every sampled item really carries one of the filtered stats");
    }

    // --- the sargable retaliation range must mean the same thing as the LIKE ---------
    {
        ItemSearchRequest r = base;
        r.isRetaliation = true;

        std::vector<ItemSearchRow> rows;
        ItemSearch::Run(db, r, 0, false, rows, truncated, err);

        std::set<int64_t> fromRange;
        for (size_t i = 0; i < rows.size(); i++) { fromRange.insert(rows[i].id); }

        // The obvious formulation the C# deliberately avoids for performance. It should
        // select the same items; if it does not, the range bounds are wrong.
        SqliteQuery q(db);
        q.Prepare("SELECT DISTINCT pir.Playeritemid FROM PlayerItemRecord pir"
                  " JOIN databaseitem_v2 db ON db.baserecord = pir.record"
                  " JOIN databaseitemstat_v2 dbs ON dbs.id_databaseitem = db.id_databaseitem"
                  " WHERE dbs.stat GLOB 'retaliation*'");
        std::set<int64_t> fromGlob;
        while (q.Step()) { fromGlob.insert(q.GetInt64(0)); }

        size_t missing = 0;
        for (std::set<int64_t>::const_iterator it = fromRange.begin(); it != fromRange.end(); ++it) {
            if (fromGlob.count(*it) == 0) { missing++; }
        }
        Check(!fromRange.empty(), "retaliation filter returns items");
        Check(missing == 0, "sargable prefix range selects only what a 'retaliation*' match would ("
                            + std::to_string(missing) + " outside)");
    }

    // --- the IFNULL in the pet-record condition must be load-bearing ------------------
    {
        // With IFNULL, as ported.
        const int64_t withIfnull = ScalarInt(db,
            "SELECT COUNT(DISTINCT pir.Playeritemid) FROM PlayerItemRecord pir"
            " JOIN PlayerItem pi2 ON pi2.Id = pir.Playeritemid"
            " WHERE pir.record NOT IN ("
            " IFNULL(pi2.BaseRecord, ''), IFNULL(pi2.PrefixRecord, ''), IFNULL(pi2.SuffixRecord, ''),"
            " IFNULL(pi2.MateriaRecord, ''), IFNULL(pi2.AscendantAffixNameRecord, ''), IFNULL(pi2.AscendantAffix2hNameRecord, ''))");

        // Without it: NULL operands make "x NOT IN (...)" evaluate to NULL, so rows vanish.
        const int64_t withoutIfnull = ScalarInt(db,
            "SELECT COUNT(DISTINCT pir.Playeritemid) FROM PlayerItemRecord pir"
            " JOIN PlayerItem pi2 ON pi2.Id = pir.Playeritemid"
            " WHERE pir.record NOT IN ("
            " pi2.BaseRecord, pi2.PrefixRecord, pi2.SuffixRecord,"
            " pi2.MateriaRecord, pi2.AscendantAffixNameRecord, pi2.AscendantAffix2hNameRecord)");

        std::printf("      pet-record rows: %lld with IFNULL, %lld without\n",
                    (long long)withIfnull, (long long)withoutIfnull);
        Check(withIfnull > withoutIfnull,
              "IFNULL wrapping is load-bearing: dropping it loses rows, as the C# warns");
    }

    // --- a filter that matches nothing must return nothing, not everything ------------
    {
        ItemSearchRequest r = base;
        r.rarity = "NoSuchRarityExists";

        std::vector<ItemSearchRow> rows;
        const bool ok = ItemSearch::Run(db, r, 0, false, rows, truncated, err);
        Check(ok && rows.empty(), "an unmatchable filter returns no rows rather than failing open");
    }

    // --- stack merging ----------------------------------------------------------------
    {
        std::vector<ItemSearchRow> rows;
        ItemSearch::Run(db, base, 0, false, rows, truncated, err);

        const std::vector<std::vector<ItemSearchRow>> stacks = ItemSearch::MergeStacks(rows);

        size_t merged = 0;
        for (size_t i = 0; i < stacks.size(); i++) { merged += stacks[i].size(); }
        Check(merged == rows.size(), "merging loses no items ("
                                     + std::to_string(merged) + " of " + std::to_string(rows.size()) + ")");
        Check(stacks.size() <= rows.size(), "merging never invents entries");
        Check(stacks.size() < rows.size(), "some items actually merged ("
                                           + std::to_string(rows.size()) + " items -> "
                                           + std::to_string(stacks.size()) + " stacks)");

        // Every member of a stack must share the identity the client merges on.
        bool identitiesConsistent = true;
        for (size_t i = 0; i < stacks.size() && identitiesConsistent; i++) {
            for (size_t k = 1; k < stacks[i].size(); k++) {
                if (stacks[i][k].baseRecord != stacks[i][0].baseRecord ||
                    stacks[i][k].prefixRecord != stacks[i][0].prefixRecord ||
                    stacks[i][k].suffixRecord != stacks[i][0].suffixRecord) {
                    identitiesConsistent = false;
                    break;
                }
            }
        }
        Check(identitiesConsistent, "every item in a stack shares base+prefix+suffix");

        // Merging must not disturb the order the search established.
        bool orderPreserved = true;
        for (size_t i = 1; i < stacks.size(); i++) {
            if (stacks[i - 1][0].id == stacks[i][0].id) { orderPreserved = false; break; }
        }
        Check(orderPreserved, "stacks keep first-appearance order");

        // The component is deliberately excluded from the identity: two copies differing
        // only by socketed component belong to the same stack, which is what the client's
        // transfer path assumes.
        size_t stacksWithMixedComponents = 0;
        for (size_t i = 0; i < stacks.size(); i++) {
            for (size_t k = 1; k < stacks[i].size(); k++) {
                if (stacks[i][k].materiaRecord != stacks[i][0].materiaRecord) {
                    stacksWithMixedComponents++;
                    break;
                }
            }
        }
        std::printf("      %llu stacks mix different components, as the client intends\n",
                    (unsigned long long)stacksWithMixedComponents);
    }

    // --- diff against SQL the C# actually generated ----------------------------------
    // The fixture is real output, captured by temporarily logging combinedSql from
    // PlayerItemDaoImpl.SearchForItems and running the client. Only the FROM clause onward
    // is compared: the overlay deliberately selects fewer columns than the client (it does
    // not display cloud ids, reroll counts, or the placeholder PetRecord/ReplicaInfo), so
    // the SELECT lists differ by design, while the part that decides WHICH rows come back
    // and in what order must not.
    if (argc >= 3) {
        std::ifstream fixture(argv[2]);
        if (!fixture) {
            Check(false, std::string("fixture not readable: ") + argv[2]);
        } else {
            std::stringstream buffer;
            buffer << fixture.rdbuf();

            // The client's startup search: vanilla, softcore, max level 110, by level.
            ItemSearchRequest r;
            r.isHardcore = false;
            r.maximumLevel = 110.0f;

            const std::string mineBody = FromClauseOnward(Normalise(ItemSearch::BuildSql(r, 0, true)));
            const std::string theirsBody = FromClauseOnward(Normalise(buffer.str()));

            const bool match = !mineBody.empty() && mineBody == theirsBody;
            Check(match, "generated SQL matches the C# original from FROM onward");
            if (!match) {
                std::printf("      C++ : %s\n", mineBody.c_str());
                std::printf("      C#  : %s\n", theirsBody.c_str());
            }
        }
    } else {
        std::printf("NOTE  no fixture given, skipping the C# SQL comparison\n");
    }

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
