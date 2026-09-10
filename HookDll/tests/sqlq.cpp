/*
 * Minimal read-only SQL runner over the vendored SQLite, for inspecting a userdata
 * database while developing the ported query.
 *
 * Usage: sqlq.exe <db> "<sql>"
 */

#include "SqliteDb.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <db> \"<sql>\"\n", argv[0]);
        return 2;
    }

    iagd::SqliteDb db;
    if (!db.OpenReadOnly(argv[1])) {
        std::fprintf(stderr, "open failed: %s\n", db.LastError().c_str());
        return 1;
    }

    iagd::SqliteQuery q(db);
    if (!q.Prepare(argv[2])) {
        std::fprintf(stderr, "prepare failed: %s\n", q.LastError().c_str());
        return 1;
    }

    bool headerWritten = false;
    int rows = 0;
    while (q.Step()) {
        if (!headerWritten) {
            for (int c = 0; c < q.ColumnCount(); c++) {
                std::printf("%s%s", c ? " | " : "", q.ColumnName(c).c_str());
            }
            std::printf("\n");
            headerWritten = true;
        }
        for (int c = 0; c < q.ColumnCount(); c++) {
            std::printf("%s%s", c ? " | " : "", q.IsNull(c) ? "<null>" : q.GetText(c).c_str());
        }
        std::printf("\n");
        if (++rows >= 200) {
            std::printf("... (truncated at 200 rows)\n");
            break;
        }
    }

    if (!q.LastError().empty()) {
        std::fprintf(stderr, "error: %s\n", q.LastError().c_str());
        return 1;
    }
    return 0;
}
