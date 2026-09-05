/*
 * Smoke test for the vendored SQLite amalgamation.
 *
 * Checks the two things the hook actually depends on before any query code exists:
 *   1. the client's userdata.db can be opened read-only from a second process, and
 *   2. PRAGMA data_version can be read from it -- the change-detection mechanism the
 *      overlay uses to notice that the client has written to the database.
 *
 * Run it against a COPY of a real userdata.db, and against a live one while the client
 * is running. The database is in WAL mode, which is why (1) is worth testing rather
 * than assuming: a read-only connection to a WAL database still needs to map the
 * -shm file, and fails with SQLITE_CANTOPEN if it cannot.
 *
 * Build: HookDll\tests\build_sqlite_smoke.cmd
 * Usage: sqlite_smoke.exe <path-to-userdata.db>
 */

#include <stdio.h>
#include "sqlite3.h"

static int fail(const char *what, sqlite3 *db) {
    fprintf(stderr, "FAIL: %s: %s\n", what, db ? sqlite3_errmsg(db) : "(no handle)");
    if (db) sqlite3_close(db);
    return 1;
}

int main(int argc, char **argv) {
    sqlite3 *db = 0;
    sqlite3_stmt *stmt = 0;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-userdata.db>\n", argv[0]);
        return 2;
    }

    printf("sqlite %s\n", sqlite3_libversion());

    /* Exactly how the hook opens it: read-only, no create, no extension loading. */
    rc = sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, 0);
    if (rc != SQLITE_OK) return fail("open read-only", db);
    printf("PASS  opened read-only\n");

    /* Proves the file really is the client's database and not an empty one. */
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM PlayerItem", -1, &stmt, 0);
    if (rc != SQLITE_OK) return fail("prepare PlayerItem count", db);
    if (sqlite3_step(stmt) != SQLITE_ROW) return fail("step PlayerItem count", db);
    printf("PASS  PlayerItem rows: %lld\n", (long long)sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);

    /* The journal mode the read-only open above has to cope with. */
    rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, 0);
    if (rc != SQLITE_OK) return fail("prepare journal_mode", db);
    if (sqlite3_step(stmt) == SQLITE_ROW)
        printf("PASS  journal_mode: %s\n", sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);

    /* The overlay's change-detection signal. */
    rc = sqlite3_prepare_v2(db, "PRAGMA data_version", -1, &stmt, 0);
    if (rc != SQLITE_OK) return fail("prepare data_version", db);
    if (sqlite3_step(stmt) != SQLITE_ROW) return fail("step data_version", db);
    printf("PASS  data_version: %d\n", sqlite3_column_int(stmt, 0));
    sqlite3_finalize(stmt);

    /* Writes must be refused. The client is the only writer; see design.md. */
    rc = sqlite3_exec(db, "CREATE TABLE _should_not_exist (x)", 0, 0, 0);
    if (rc == SQLITE_OK) {
        fprintf(stderr, "FAIL: a write succeeded on a read-only connection\n");
        sqlite3_close(db);
        return 1;
    }
    printf("PASS  write refused (%s)\n", sqlite3_errmsg(db));

    sqlite3_close(db);
    printf("OK\n");
    return 0;
}
