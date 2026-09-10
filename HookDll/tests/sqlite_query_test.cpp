/*
 * Offline tests for the read-only SQLite wrapper (SqliteDb / SqliteQuery).
 *
 * Runs outside the game against a copy of a real userdata database, which is the whole
 * point of keeping that layer free of game and logging dependencies: the search can be
 * exercised and compared against the C# original without launching Grim Dawn.
 *
 * Build: HookDll\tests\build_tests.cmd
 * Usage: sqlite_query_test.exe <path-to-userdata.db>
 */

#include "SqliteDb.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("PASS  %s\n", what.c_str());
    } else {
        std::printf("FAIL  %s\n", what.c_str());
        g_failures++;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-userdata.db>\n", argv[0]);
        return 2;
    }

    iagd::SqliteDb db;
    Check(db.OpenReadOnly(argv[1]), "open read-only");
    if (!db.IsOpen()) {
        std::fprintf(stderr, "cannot continue: %s\n", db.LastError().c_str());
        return 1;
    }

    int version = 0;
    Check(db.ReadDataVersion(version), "PRAGMA data_version readable");

    // A scalar parameter, and the fact that binding actually filters.
    {
        iagd::SqliteQuery q(db);
        q.SetParam("minlevel", (int64_t)70);
        Check(q.Prepare("SELECT COUNT(*) FROM PlayerItem WHERE LevelRequirement >= :minlevel"),
              "prepare with a scalar parameter");
        Check(q.Step(), "scalar query returns a row");
        const int64_t high = q.GetInt64(0);

        iagd::SqliteQuery all(db);
        all.Prepare("SELECT COUNT(*) FROM PlayerItem");
        all.Step();
        const int64_t total = all.GetInt64(0);

        std::printf("      level>=70: %lld of %lld items\n", (long long)high, (long long)total);
        Check(high <= total, "filtered count does not exceed the total");
        Check(total > 0, "database actually contains items");
    }

    // List parameters, which is the piece SQLite has no native support for.
    {
        iagd::SqliteQuery q(db);
        std::vector<std::string> rarities;
        rarities.push_back("Legendary");
        rarities.push_back("Epic");
        q.SetParamList("rarities", rarities);
        Check(q.Prepare("SELECT COUNT(*) FROM PlayerItem WHERE Rarity IN ( :rarities )"),
              "prepare with a list parameter");
        Check(q.ExpandedSql().find("?,?") != std::string::npos,
              "list parameter expanded to positional placeholders");
        Check(q.Step(), "list query returns a row");
        std::printf("      legendary+epic: %lld items\n", (long long)q.GetInt64(0));
    }

    // The same name used twice must expand twice, or the second occurrence binds nothing.
    {
        iagd::SqliteQuery q(db);
        q.SetParam("n", (int64_t)5);
        Check(q.Prepare("SELECT :n + :n"), "a repeated parameter name expands each time");
        Check(q.Step() && q.GetInt64(0) == 10, "repeated parameter binds the same value twice");
    }

    // A colon inside a string literal is not a parameter.
    {
        iagd::SqliteQuery q(db);
        Check(q.Prepare("SELECT 'a:b' AS v"), "colon inside a string literal is left alone");
        Check(q.Step() && q.GetText(0) == "a:b", "string literal survives expansion intact");
    }

    // Failures must be reported, not guessed at.
    {
        iagd::SqliteQuery q(db);
        Check(!q.Prepare("SELECT :missing"), "missing parameter value is rejected");
        Check(q.LastError().find("missing") != std::string::npos, "error names the missing parameter");
    }
    {
        iagd::SqliteQuery q(db);
        q.SetParamList("empty", std::vector<std::string>());
        Check(!q.Prepare("SELECT 1 WHERE 1 IN ( :empty )"), "empty list parameter is rejected");
    }

    // The connection must be read-only in fact, not just by intent.
    {
        iagd::SqliteQuery q(db);
        const bool prepared = q.Prepare("CREATE TABLE _should_not_exist (x)");
        const bool stepped = prepared && q.Step();
        Check(!stepped, "writes are refused on the read-only connection");
    }

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
