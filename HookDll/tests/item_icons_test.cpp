/*
 * Offline verification of icon resolution (ItemIcons).
 *
 * The check that matters is not that a name comes back, but that the name corresponds to a
 * file the client actually extracted -- an icon that resolves to nothing on disk is a
 * broken picture in the overlay. So this resolves icons for a large sample of real items
 * and then looks for each one in the client's storage folder.
 *
 * Usage: item_icons_test.exe <path-to-userdata.db> [path-to-storage-folder]
 */

#include "ItemIcons.h"
#include "SqliteDb.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace iagd;

static int g_failures = 0;

static void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        g_failures++;
    }
}

static bool FileExists(const std::string& path) {
    struct _stat64 info;
    return _stat64(path.c_str(), &info) == 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-userdata.db> [storage-folder]\n", argv[0]);
        return 2;
    }

    // --- the leaf-name mapping ---------------------------------------------------------
    Check(ItemIcons::ToImageFileName("items/gearrelics/tier3/tier3_relic_22.tex") == "tier3_relic_22.tex.png",
          "a stored bitmap path becomes the extracted file name");
    Check(ItemIcons::ToImageFileName("items\\gear\\thing.tex") == "thing.tex.png",
          "backslash separators are handled too");
    Check(ItemIcons::ToImageFileName("").empty(), "an empty path resolves to nothing");
    Check(ItemIcons::ToImageFileName("bare.tex") == "bare.tex.png", "a path with no directory works");

    // --- the scores must be the client's -----------------------------------------------
    Check(ItemIcons::ScoreFor("bitmap") == 10 && ItemIcons::ScoreFor("relicBitmap") == 8 &&
          ItemIcons::ScoreFor("shardBitmap") == 6 && ItemIcons::ScoreFor("artifactBitmap") == 4 &&
          ItemIcons::ScoreFor("noteBitmap") == 2 && ItemIcons::ScoreFor("artifactFormulaBitmapName") == 0,
          "bitmap scores match the client's");
    Check(ItemIcons::ScoreFor("offensiveFire") < 0, "a non-bitmap stat scores as not-a-bitmap");

    SqliteDb db;
    if (!db.OpenReadOnly(argv[1])) {
        std::fprintf(stderr, "cannot open: %s\n", db.LastError().c_str());
        return 1;
    }

    // --- resolve icons for a real sample -----------------------------------------------
    std::vector<std::string> records;
    {
        SqliteQuery q(db);
        q.Prepare("SELECT DISTINCT BaseRecord FROM PlayerItem WHERE BaseRecord IS NOT NULL LIMIT 400");
        while (q.Step()) { records.push_back(q.GetText(0)); }
    }
    Check(!records.empty(), "sampled real item records (" + std::to_string(records.size()) + ")");

    // Deliberately add every record that carries MORE THAN ONE candidate bitmap. Those are
    // the only ones where the scoring can be wrong, and a random sample of owned items
    // happens to contain none of them -- so without this the tie-break goes untested while
    // appearing to pass.
    size_t multiCandidateRecords = 0;
    {
        SqliteQuery q(db);
        q.Prepare("SELECT db.baserecord FROM databaseitem_v2 db"
                  " JOIN databaseitemstat_v2 dbs ON dbs.id_databaseitem = db.id_databaseitem"
                  " WHERE dbs.stat IN"
                  " ('bitmap','relicBitmap','shardBitmap','artifactBitmap','noteBitmap','artifactFormulaBitmapName')"
                  " GROUP BY db.baserecord HAVING COUNT(*) > 1");
        while (q.Step()) {
            records.push_back(q.GetText(0));
            multiCandidateRecords++;
        }
    }
    Check(multiCandidateRecords > 0, "found records with competing bitmap candidates ("
                                     + std::to_string(multiCandidateRecords) + ")");

    const std::map<std::string, std::string> icons = ItemIcons::Resolve(db, records);
    std::printf("      resolved %llu of %llu records to a bitmap\n",
                (unsigned long long)icons.size(), (unsigned long long)records.size());
    Check(!icons.empty(), "icons resolve for real records");

    // --- the highest-scoring candidate must win ------------------------------------------
    // Only 107 records in this data carry more than one candidate, so the tie-break is
    // easy to get wrong without noticing. Checked directly against the database.
    {
        size_t checked = 0, wrong = 0;
        for (std::map<std::string, std::string>::const_iterator it = icons.begin(); it != icons.end(); ++it) {
            SqliteQuery q(db);
            q.SetParam("rec", it->first);
            q.Prepare("SELECT dbs.stat, dbs.TextValue FROM databaseitem_v2 db"
                      " JOIN databaseitemstat_v2 dbs ON dbs.id_databaseitem = db.id_databaseitem"
                      " WHERE db.baserecord = :rec AND dbs.stat IN"
                      " ('bitmap','relicBitmap','shardBitmap','artifactBitmap','noteBitmap','artifactFormulaBitmapName')");

            int bestScore = -1;
            std::string bestValue;
            int candidates = 0;
            while (q.Step()) {
                const int score = ItemIcons::ScoreFor(q.GetText(0));
                if (q.GetText(1).empty()) { continue; }
                candidates++;
                if (score > bestScore) { bestScore = score; bestValue = q.GetText(1); }
            }

            if (candidates > 1) {
                checked++;
                if (bestValue != it->second) { wrong++; }
            }
        }
        std::printf("      %llu sampled records had more than one candidate\n", (unsigned long long)checked);
        Check(wrong == 0, "the highest-scoring candidate is the one chosen ("
                          + std::to_string(wrong) + " wrong)");
    }

    // --- the resolved names must exist on disk -------------------------------------------
    if (argc >= 3) {
        const std::string storage = argv[2];
        size_t present = 0, missing = 0;
        std::string firstMissing;

        for (std::map<std::string, std::string>::const_iterator it = icons.begin(); it != icons.end(); ++it) {
            const std::string file = ItemIcons::ToImageFileName(it->second);
            if (file.empty()) { continue; }

            if (FileExists(storage + "\\" + file)) {
                present++;
            } else {
                if (missing++ == 0) { firstMissing = file + "  (from " + it->second + ")"; }
            }
        }

        std::printf("      icon files: %llu present, %llu missing\n",
                    (unsigned long long)present, (unsigned long long)missing);
        if (!firstMissing.empty()) {
            std::printf("      first missing: %s\n", firstMissing.c_str());
        }
        Check(present > 0, "resolved icons correspond to files the client extracted");
        // Not demanding zero missing: the client's extraction can lag a game update, and
        // the overlay is specified to fall back to a placeholder rather than fail.
        Check(missing * 10 < present, "the overwhelming majority of resolved icons exist on disk");
    } else {
        std::printf("NOTE  no storage folder given, skipping the on-disk check\n");
    }

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
