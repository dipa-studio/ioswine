/* Host-independent test program for libsqlite3 via dyld shared cache. */

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define SQLITE_OK          0
#define SQLITE_ROW         100
#define SQLITE_DONE        101

#define SQLITE_OPEN_READONLY         0x00000001
#define SQLITE_OPEN_READWRITE        0x00000002
#define SQLITE_OPEN_CREATE           0x00000004
#define SQLITE_OPEN_FILEPROTECTION_COMPLETEUNTILFIRSTUSERAUTHENTICATION 0x00300000

int sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
int sqlite3_close_v2(sqlite3 *db);
int sqlite3_close(sqlite3 *db);
int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg, char **errmsg);
int sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
int sqlite3_step(sqlite3_stmt *pStmt);
int sqlite3_finalize(sqlite3_stmt *pStmt);
const unsigned char *sqlite3_column_text(sqlite3_stmt *pStmt, int iCol);
int sqlite3_column_int(sqlite3_stmt *pStmt, int iCol);
const char *sqlite3_errmsg(sqlite3 *db);

int puts(const char *s);
int printf(const char *fmt, ...);
int unlink(const char *pathname);
void exit(int status);

int main(void) {
    const char *db_path = "/data/local/tmp/ioswine/sqltest.db";
    unlink(db_path);

    sqlite3 *db = 0;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FILEPROTECTION_COMPLETEUNTILFIRSTUSERAUTHENTICATION;
    int rc = sqlite3_open_v2(db_path, &db, flags, 0);
    if (rc != SQLITE_OK) {
        printf("sqlite3_open_v2 failed: %d (errmsg: %s)\n", rc, db ? sqlite3_errmsg(db) : "null db");
        exit(1);
    }

    char *errmsg = 0;
    rc = sqlite3_exec(db, "CREATE TABLE test_tab (id INTEGER PRIMARY KEY, msg TEXT);", 0, 0, &errmsg);
    if (rc != SQLITE_OK) {
        printf("CREATE TABLE failed: %d (errmsg: %s)\n", rc, errmsg ? errmsg : sqlite3_errmsg(db));
        exit(1);
    }

    rc = sqlite3_exec(db, "INSERT INTO test_tab (id, msg) VALUES (1, 'sqlite_test: OK');", 0, 0, &errmsg);
    if (rc != SQLITE_OK) {
        printf("INSERT failed: %d (errmsg: %s)\n", rc, errmsg ? errmsg : sqlite3_errmsg(db));
        exit(1);
    }

    sqlite3_stmt *stmt = 0;
    rc = sqlite3_prepare_v2(db, "SELECT msg FROM test_tab WHERE id = 1;", -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        printf("PREPARE failed: %d (errmsg: %s)\n", rc, sqlite3_errmsg(db));
        exit(1);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("STEP failed: %d (errmsg: %s)\n", rc, sqlite3_errmsg(db));
        exit(1);
    }

    const unsigned char *text = sqlite3_column_text(stmt, 0);
    if (!text) {
        puts("column_text returned NULL");
        exit(1);
    }

    puts((const char *)text);

    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);

    return 0;
}
