#include "ItemIcons.h"
#include "SqliteDb.h"

namespace iagd {

namespace {

/// The client's scores, from DatabaseItemStatDaoImpl.MapItemBitmaps. Changing any of these
/// makes the overlay show a different picture than the client for the same item.
struct BitmapStat {
    const char* name;
    int score;
};

const BitmapStat kBitmapStats[] = {
    { "bitmap",                    10 },
    { "relicBitmap",                8 },
    { "shardBitmap",                6 },
    { "artifactBitmap",             4 },
    { "noteBitmap",                 2 },
    { "artifactFormulaBitmapName",  0 },
};

const size_t kBitmapStatCount = sizeof(kBitmapStats) / sizeof(kBitmapStats[0]);

}  // namespace

int ItemIcons::ScoreFor(const std::string& statName) {
    for (size_t i = 0; i < kBitmapStatCount; i++) {
        if (statName == kBitmapStats[i].name) {
            return kBitmapStats[i].score;
        }
    }
    return -1;
}

std::string ItemIcons::ToImageFileName(const std::string& bitmapPath) {
    if (bitmapPath.empty()) {
        return std::string();
    }

    // Both separators appear in the game's data.
    const size_t slash = bitmapPath.find_last_of("/\\");
    const std::string leaf = (slash == std::string::npos) ? bitmapPath : bitmapPath.substr(slash + 1);

    if (leaf.empty()) {
        return std::string();
    }

    return leaf + ".png";
}

std::map<std::string, std::string> ItemIcons::Resolve(SqliteDb& db, const std::vector<std::string>& records) {
    std::map<std::string, std::string> resolved;
    if (records.empty()) {
        return resolved;
    }

    // Every candidate for every requested record in one query. Doing this per record would
    // be one round trip per item on screen.
    std::vector<std::string> statNames;
    for (size_t i = 0; i < kBitmapStatCount; i++) {
        statNames.push_back(kBitmapStats[i].name);
    }

    SqliteQuery q(db);
    q.SetParamList("records", records);
    q.SetParamList("stats", statNames);

    if (!q.Prepare("SELECT db.baserecord, dbs.stat, dbs.TextValue"
                   " FROM databaseitem_v2 db"
                   " JOIN databaseitemstat_v2 dbs ON dbs.id_databaseitem = db.id_databaseitem"
                   " WHERE db.baserecord IN ( :records ) AND dbs.stat IN ( :stats )")) {
        return resolved;
    }

    std::map<std::string, int> bestScore;

    while (q.Step()) {
        const std::string record = q.GetText(0);
        const std::string stat = q.GetText(1);
        const std::string value = q.GetText(2);

        if (value.empty()) {
            continue;
        }

        const int score = ScoreFor(stat);
        if (score < 0) {
            continue;
        }

        // Strictly greater, so the first candidate wins a tie. The C# sorts descending by
        // score and takes the first, which has the same effect for equal scores.
        const std::map<std::string, int>::const_iterator seen = bestScore.find(record);
        if (seen == bestScore.end() || score > seen->second) {
            bestScore[record] = score;
            resolved[record] = value;
        }
    }

    return resolved;
}

}  // namespace iagd
