#include "ItemSearch.h"
#include "SqliteDb.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <map>

namespace iagd {

/// A PlayerItemRecord row is a "pet record" iff it isn't one of the item's own core records
/// (base/prefix/suffix/materia/ascendant affixes).
///
/// The IFNULL wrapping is required and must not be simplified away: Prefix/Suffix/Materia/
/// Ascendant records are NULL rather than '' on most items, and "x NOT IN (a, b, NULL)"
/// evaluates to NULL for every row once any operand is NULL, so the filter would match
/// nothing at all. Ported from PlayerItemDaoImpl.PetRecordCondition.
static const char* const kPetRecordCondition =
    "pir.record NOT IN ("
    " IFNULL(pi2.BaseRecord, ''), IFNULL(pi2.PrefixRecord, ''), IFNULL(pi2.SuffixRecord, ''),"
    " IFNULL(pi2.MateriaRecord, ''), IFNULL(pi2.AscendantAffixNameRecord, ''), IFNULL(pi2.AscendantAffix2hNameRecord, '')"
    ")";

/// Ported from ItemSkillDaoImpl.SkillGrantingRecordsQuery.
static const char* const kSkillGrantingRecordsQuery =
    "SELECT DISTINCT db.baserecord as PlayerItemRecord"
    " from itemskill_v2 s, itemskill_mapping map, DatabaseItem_v2 db "
    " where s.id_skill = map.id_skill "
    " and map.id_databaseitem = db.id_databaseitem ";

/// Ported from PlayerItemDaoImpl.SearchForItems's selectColumns. Trimmed to the columns the
/// overlay displays; PetRecord and ReplicaInfo are fetched separately for the visible page
/// only, exactly as the client does.
static const char* const kSelectColumns =
    "select PI.name as Name,"
    " PI.StackCount as StackCount,"
    " PI.rarity as Rarity,"
    " PI.levelrequirement as LevelRequirement,"
    " PI.baserecord as BaseRecord,"
    " PI.prefixrecord as PrefixRecord,"
    " PI.suffixrecord as SuffixRecord,"
    " PI.MateriaRecord as MateriaRecord,"
    " PI.Id as Id,"
    " PI.Mod as Mod,"
    " CAST(PI.IsHardcore as bit) as IsHardcore,"
    " PI.Seed as Seed ";

std::string ItemSearch::ToLowerAscii(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Ported from PlayerItemDaoImpl.ToSqlOperator.
std::string ItemSearch::ToSqlOperator(StatValueFilter::Op op) {
    switch (op) {
    case StatValueFilter::Op::GreaterThan:    return ">";
    case StatValueFilter::Op::GreaterOrEqual: return ">=";
    case StatValueFilter::Op::LessThan:       return "<";
    case StatValueFilter::Op::LessOrEqual:    return "<=";
    case StatValueFilter::Op::Equal:          return "=";
    default:                                  return ">=";
    }
}

/// Ported from PlayerItemDaoImpl.RecordStatSubquery.
///
/// Driven by joining PlayerItemRecord to databaseitem_v2 and databaseitemstat_v2 through
/// their indexes. The C# comment records that the alternative -- a correlated EXISTS over
/// all database items with an owned-records UNION -- was 20-25x slower on large
/// collections. Keep the join shape.
std::string ItemSearch::RecordStatSubquery(const std::string& dbsCondition, bool petOnly) {
    const std::string petJoin = petOnly ? "JOIN PlayerItem pi2 ON pi2.Id = pir.Playeritemid" : "";
    const std::string petFilter = petOnly ? (std::string("AND ") + kPetRecordCondition) : "";

    std::ostringstream sql;
    sql << " SELECT pir.Playeritemid FROM PlayerItemRecord pir "
        << petJoin
        << " JOIN databaseitem_v2 db ON db.baserecord = pir.record"
        << " JOIN databaseitemstat_v2 dbs ON dbs.id_databaseitem = db.id_databaseitem AND (" << dbsCondition << ") "
        << petFilter;
    return sql.str();
}

std::string ItemSearch::BuildSql(const ItemSearchRequest& request, int skip, bool orderByLevel) {
    std::vector<std::string> fragments;

    // --- wildcard -----------------------------------------------------------------
    if (!request.wildcard.empty()) {
        fragments.push_back(
            "(PI.namelowercase LIKE :name OR R.id IN (SELECT replicaitemid FROM replicaitemrow"
            " WHERE IFNULL(textlowercase, text) LIKE :wildcard))");
    }

    // --- mod / hardcore -----------------------------------------------------------
    // Scoped to the world the player is standing in. An empty mod means vanilla, which is
    // stored as NULL or '' depending on when the row was written.
    if (request.mod.empty()) {
        fragments.push_back("(PI.Mod IS NULL OR PI.Mod = '')");
    } else {
        fragments.push_back("LOWER(PI.Mod) = LOWER( :mod )");
    }

    fragments.push_back(request.isHardcore ? "PI.IsHardcore" : "NOT PI.IsHardcore");

    // --- simple column filters ----------------------------------------------------
    if (!request.rarity.empty()) {
        fragments.push_back("PI.Rarity = :rarity");
    }

    if (request.prefixRarity > 0) {
        fragments.push_back("PI.PrefixRarity >= :prefixRarity");
    }

    if (request.socketedOnly) {
        fragments.push_back("PI.MateriaRecord is not null and PI.MateriaRecord != ''");
    }

    if (request.duplicatesOnly) {
        const std::string hcSc = request.isHardcore ? "IsHardcore" : "NOT IsHardcore";
        const std::string modCondition = request.mod.empty()
            ? std::string("(Mod IS NULL OR Mod = '')")
            : std::string("LOWER(Mod) = LOWER( :mod )");

        fragments.push_back(
            "PI.BaseRecord IN (SELECT BaseRecord FROM ("
            " select baserecord || prefixrecord || suffixrecord as Records, count(*) as N, BaseRecord from PlayerItem"
            " WHERE " + modCondition +
            " AND " + hcSc +
            " group by Records"
            " HAVING N > 1"
            " order by N desc"
            "))");
    }

    if (request.minimumLevel > 0) {
        fragments.push_back("PI.LevelRequirement >= :minlevel");
    }

    if (request.maximumLevel < 120 && request.maximumLevel > 0) {
        fragments.push_back("PI.LevelRequirement <= :maxlevel");
    }

    if (request.recentOnly) {
        fragments.push_back("created_at > :filter_recentOnly");
    }

    if (request.withGrantSkillsOnly) {
        fragments.push_back(std::string("PI.baserecord IN (") + kSkillGrantingRecordsQuery + ")");
    }

    if (request.withSummonerSkillOnly) {
        fragments.push_back(
            "PI.baserecord IN (SELECT p.baserecord as PlayerItemRecord"
            " from itemskill_v2 s, itemskill_mapping map, DatabaseItem_v2 db,  playeritem p, DatabaseItemStat_v2 stat  "
            " where s.id_skill = map.id_skill "
            " and map.id_databaseitem = db.id_databaseitem  "
            " and db.baserecord = p.baserecord "
            " and stat.id_databaseitem = s.id_databaseitem"
            " and stat.stat = 'spawnObjects')");
    }

    // --- shared FROM/WHERE body ---------------------------------------------------
    // Shared verbatim between the row query and any COUNT over it, so a total can never
    // drift from what the paged query actually returns.
    std::ostringstream body;
    body << "FROM PlayerItem PI"
         << " LEFT OUTER JOIN ReplicaItem2 R ON PI.ID = R.playeritemid"
         << " WHERE ";
    for (size_t i = 0; i < fragments.size(); i++) {
        if (i > 0) {
            body << " AND ";
        }
        body << fragments[i];
    }

    // --- stat filters (CreateDatabaseStatQueryParams) ------------------------------
    // Pet-bonus target records store every stat under a "pet"-prefixed name, so scoping to
    // pets means renaming the stats rather than filtering rows.
    const std::string petPrefix = request.petBonuses ? "pet" : "";
    size_t statFragmentCount = 0;

    for (size_t f = 0; f < request.filters.size(); f++) {
        std::ostringstream param;
        param << "filter_" << f;
        body << " AND PI.Id IN (" << RecordStatSubquery("dbs.stat in ( :" + param.str() + " )", request.petBonuses) << ")";
        statFragmentCount++;
    }

    if (request.isRetaliation) {
        // A sargable prefix RANGE, not a LIKE. SQLite's LIKE is case-insensitive by default
        // and cannot use the (BINARY) index on DatabaseItemStat_v2.Stat, so "stat LIKE
        // 'retaliation%'" scans the stat rows instead. The upper bound is the prefix with
        // its last character incremented. Do not "simplify" this back to a LIKE.
        std::string lower = petPrefix + "retaliation";
        std::string upper = lower;
        upper[upper.size() - 1] = static_cast<char>(upper[upper.size() - 1] + 1);

        body << " AND PI.Id IN ("
             << RecordStatSubquery("(dbs.stat >= '" + lower + "' AND dbs.stat < '" + upper + "')", request.petBonuses)
             << ")";
        statFragmentCount++;
    }

    for (size_t c = 0; c < request.classes.size(); c++) {
        static const char* const kClassStats[] = {
            "augmentSkill1Extras", "augmentSkill2Extras", "augmentSkill3Extras", "augmentSkill4Extras",
            "augmentMastery1", "augmentMastery2", "augmentMastery3", "augmentMastery4"
        };

        std::ostringstream stats;
        for (size_t k = 0; k < sizeof(kClassStats) / sizeof(kClassStats[0]); k++) {
            if (k > 0) {
                stats << ",";
            }
            stats << "'" << petPrefix << kClassStats[k] << "'";
        }

        std::ostringstream condition;
        condition << "dbs.stat IN (" << stats.str() << ")"
                  << " AND dbs.TextValue = '" << request.classes[c] << "'";

        body << " AND PI.Id IN (" << RecordStatSubquery(condition.str(), request.petBonuses) << ")";
        statFragmentCount++;
    }

    // The plain "has a pet bonus" filter. Never pet-scoped, so it combines with ordinary
    // non-pet stat filters -- "has a pet bonus AND cold damage on the player".
    if (request.hasPetBonus) {
        body << " AND PI.Id IN (" << RecordStatSubquery("dbs.stat = 'petBonusName'", false) << ")";
    }

    // Pet scope selected with no other stat filter: just require a pet record at all.
    if (request.petBonuses && statFragmentCount == 0) {
        body << " AND PI.Id IN ( SELECT pir.Playeritemid FROM PlayerItemRecord pir"
             << " JOIN PlayerItem pi2 ON pi2.Id = pir.Playeritemid"
             << " WHERE " << kPetRecordCondition << ")";
    }

    // --- slot ---------------------------------------------------------------------
    if (!request.slot.empty()) {
        body << " AND PI.Id " << (request.slotInverse ? "NOT " : "")
             << "IN (" << RecordStatSubquery("dbs.stat = 'Class' AND dbs.TextValue in ( :class )", false) << ")";

        // ItemRelic is the component slot. Without this, searching for components returns
        // every item that merely has one socketed.
        if (request.slot.size() == 1 && request.slot[0] == "ItemRelic") {
            body << " AND PI.MateriaRecord = ''";
        }
    }

    // --- numeric stat filters -----------------------------------------------------
    // Read straight from the pre-computed table so the seed engine is not replayed here.
    // Items the background worker has not reached yet have no rows and are excluded, which
    // matches the client.
    if (!request.statValueFilters.empty() && !request.petBonuses) {
        for (size_t i = 0; i < request.statValueFilters.size(); i++) {
            std::ostringstream fieldsParam, thresholdParam;
            fieldsParam << "svf_fields_" << i;
            thresholdParam << "svf_threshold_" << i;

            body << " AND PI.Id IN ("
                 << " SELECT playeritemid FROM ComputedItemStat"
                 << " WHERE stat IN ( :" << fieldsParam.str() << " )"
                 << " GROUP BY playeritemid"
                 << " HAVING SUM(value) " << ToSqlOperator(request.statValueFilters[i].op)
                 << " :" << thresholdParam.str() << ")";
        }
    }

    // --- ordering and paging ------------------------------------------------------
    // Deterministic so LIMIT/OFFSET slices are stable across pages: without the Id
    // tiebreaker, rows with equal names can be skipped or repeated between batches.
    // Matches PlayerItem.CompareTo (Name, then Id).
    const std::string orderBy = orderByLevel
        ? " ORDER BY PI.levelrequirement, PI.name, PI.Id "
        : " ORDER BY PI.name, PI.Id ";

    std::ostringstream sql;
    sql << kSelectColumns << body.str() << orderBy
        << " LIMIT " << (MaxSearchResults + 1) << " OFFSET " << skip;
    return sql.str();
}

void ItemSearch::BindParams(SqliteQuery& query, const ItemSearchRequest& request) {
    if (!request.wildcard.empty()) {
        const std::string lowered = ToLowerAscii(request.wildcard);

        // Two different patterns on purpose, as in the C#: the name match treats spaces as
        // wildcards so "fire strike" finds "Fire-Touched Strike", while the stat-text match
        // does not.
        std::string spaced = lowered;
        std::replace(spaced.begin(), spaced.end(), ' ', '%');

        query.SetParam("name", "%" + spaced + "%");
        query.SetParam("wildcard", "%" + lowered + "%");
    }

    if (!request.mod.empty()) {
        query.SetParam("mod", request.mod);
    }

    if (!request.rarity.empty()) {
        query.SetParam("rarity", request.rarity);
    }

    if (request.prefixRarity > 0) {
        query.SetParam("prefixRarity", static_cast<int64_t>(request.prefixRarity));
    }

    if (request.minimumLevel > 0) {
        query.SetParam("minlevel", static_cast<double>(request.minimumLevel));
    }

    if (request.maximumLevel < 120 && request.maximumLevel > 0) {
        query.SetParam("maxlevel", static_cast<double>(request.maximumLevel));
    }

    const std::string petPrefix = request.petBonuses ? "pet" : "";
    for (size_t f = 0; f < request.filters.size(); f++) {
        std::vector<std::string> effective;
        effective.reserve(request.filters[f].size());
        for (size_t k = 0; k < request.filters[f].size(); k++) {
            effective.push_back(petPrefix + request.filters[f][k]);
        }

        std::ostringstream param;
        param << "filter_" << f;
        query.SetParamList(param.str(), effective);
    }

    if (!request.slot.empty()) {
        query.SetParamList("class", request.slot);
    }

    if (!request.statValueFilters.empty() && !request.petBonuses) {
        for (size_t i = 0; i < request.statValueFilters.size(); i++) {
            std::ostringstream fieldsParam, thresholdParam;
            fieldsParam << "svf_fields_" << i;
            thresholdParam << "svf_threshold_" << i;

            query.SetParamList(fieldsParam.str(), request.statValueFilters[i].fields);
            query.SetParam(thresholdParam.str(), request.statValueFilters[i].threshold);
        }
    }
}

bool ItemSearch::Run(SqliteDb& db, const ItemSearchRequest& request, int skip, bool orderByLevel,
                     std::vector<ItemSearchRow>& outRows, bool& outWasTruncated, std::string& outError) {
    outRows.clear();
    outWasTruncated = false;

    SqliteQuery query(db);
    BindParams(query, request);

    if (!query.Prepare(BuildSql(request, skip, orderByLevel))) {
        outError = query.LastError();
        return false;
    }

    while (query.Step()) {
        ItemSearchRow row;
        row.name = query.GetText(0);
        row.nameIsNull = query.IsNull(0);
        row.stackCount = query.GetInt64(1);
        row.rarity = query.GetText(2);
        row.levelRequirement = static_cast<float>(query.GetDouble(3));
        row.baseRecord = query.GetText(4);
        row.prefixRecord = query.GetText(5);
        row.suffixRecord = query.GetText(6);
        row.materiaRecord = query.GetText(7);
        row.id = query.GetInt64(8);
        row.mod = query.GetText(9);
        row.isHardcore = query.GetInt64(10) != 0;
        row.seed = query.GetInt64(11);
        outRows.push_back(row);
    }

    if (!query.LastError().empty() && outRows.empty()) {
        outError = query.LastError();
        return false;
    }

    // One row beyond the cap is fetched so "there are more" can be distinguished from
    // "there are exactly this many", the same trick the client uses.
    if (outRows.size() > static_cast<size_t>(MaxSearchResults)) {
        outRows.resize(MaxSearchResults);
        outWasTruncated = true;
    }

    return true;
}

std::vector<std::vector<ItemSearchRow>> ItemSearch::MergeStacks(const std::vector<ItemSearchRow>& rows) {
    std::vector<std::vector<ItemSearchRow>> stacks;

    // Maps an identity to its slot in the output, so first-appearance order survives.
    // The C# relies on Dictionary enumeration order for this; being explicit avoids
    // inheriting that assumption.
    std::map<std::string, size_t> indexByIdentity;

    for (size_t i = 0; i < rows.size(); i++) {
        const std::string identity = rows[i].baseRecord + rows[i].prefixRecord + rows[i].suffixRecord;

        const std::map<std::string, size_t>::const_iterator at = indexByIdentity.find(identity);
        if (at == indexByIdentity.end()) {
            indexByIdentity[identity] = stacks.size();
            stacks.push_back(std::vector<ItemSearchRow>(1, rows[i]));
        } else {
            stacks[at->second].push_back(rows[i]);
        }
    }

    return stacks;
}

}  // namespace iagd
