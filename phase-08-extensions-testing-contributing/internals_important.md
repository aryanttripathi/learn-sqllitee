# Phase 8 — Internals Reference: Extension APIs, Testing, Contribution

## Extension entry point (loadable module)

```c
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1                 /* declares the API pointer */

int sqlite3_<basename>_init(sqlite3 *db, char **pzErrMsg,
                            const sqlite3_api_routines *pApi){
  SQLITE_EXTENSION_INIT2(pApi);        /* wires sqlite3_* to the host's function table */
  ...register things...
  return SQLITE_OK;                    /* or SQLITE_OK_LOAD_PERMANENTLY */
}
```
The init function name must match the library basename (`myseries.dylib` →
`sqlite3_myseries_init`) or be given explicitly to `.load file entrypoint`.

Loading:
```sql
.load ./myseries
SELECT load_extension('./myseries');    -- needs sqlite3_enable_load_extension(db,1)
```
```c
sqlite3_enable_load_extension(db, 1);
sqlite3_load_extension(db, "./myseries", 0, &zErr);
sqlite3_auto_extension(xInit);          /* register for every new connection */
```
Apple's system `sqlite3` is built with extension loading **disabled** — build your own
shell from the amalgamation.

## Function registration
```c
int sqlite3_create_function_v2(sqlite3*, const char *zName, int nArg, int eTextRep,
    void *pApp, void (*xFunc)(sqlite3_context*,int,sqlite3_value**),
    void (*xStep)(sqlite3_context*,int,sqlite3_value**),
    void (*xFinal)(sqlite3_context*), void(*xDestroy)(void*));
int sqlite3_create_window_function(..., xStep, xFinal, xValue, xInverse, xDestroy);
```
`eTextRep` flags: `SQLITE_UTF8`, `SQLITE_UTF16`, `SQLITE_DETERMINISTIC`,
`SQLITE_DIRECTONLY`, `SQLITE_INNOCUOUS`, `SQLITE_SUBTYPE`, `SQLITE_RESULT_SUBTYPE`.

In-function API: `sqlite3_value_type/int64/double/text/blob/bytes/nochange/frombind`,
`sqlite3_result_*`, `sqlite3_aggregate_context`, `sqlite3_get_auxdata`/`set_auxdata`,
`sqlite3_user_data`, `sqlite3_context_db_handle`, `sqlite3_result_error(_code/_nomem/_toobig)`.

## Virtual table checklist

| Method | Required? | Notes |
|---|---|---|
| `xCreate` | for `CREATE VIRTUAL TABLE` | set to 0 (or = xConnect) for eponymous |
| `xConnect` | yes | must call `sqlite3_declare_vtab()` |
| `xBestIndex` | yes | the planner interface; see below |
| `xDisconnect` | yes | free the vtab object |
| `xDestroy` | if xCreate | drop persistent storage |
| `xOpen`/`xClose` | yes | cursor lifecycle |
| `xFilter` | yes | begin a scan with the chosen plan |
| `xNext`/`xEof`/`xColumn`/`xRowid` | yes | iteration |
| `xUpdate` | for writable tables | argc==1 ⇒ DELETE; argv[1]==NULL ⇒ INSERT; else UPDATE |
| `xBegin/xSync/xCommit/xRollback` | for transactional tables | |
| `xSavepoint/xRelease/xRollbackTo` | iVersion≥2 | |
| `xFindFunction` | optional | overload functions (e.g. `MATCH`) for this table |
| `xRename` | optional | ALTER TABLE RENAME |
| `xShadowName` | iVersion≥3 | declare shadow-table naming, for defensive mode |
| `xIntegrity` | iVersion≥4 | participate in `PRAGMA integrity_check` |

**`xBestIndex` rules**
```
- examine aConstraint[i]: {iColumn, op, usable}; SKIP any with usable==0
- ops: EQ GT LE LT GE MATCH LIKE GLOB REGEXP NE ISNOT ISNOTNULL ISNULL IS LIMIT OFFSET FUNCTION
- to consume a constraint: aConstraintUsage[i].argvIndex = k (1-based order for xFilter)
- omit = 1 means "SQLite will not re-check this" — only if you truly enforce it
- idxNum / idxStr are yours; they are passed verbatim to xFilter
- estimatedCost competes with b-tree costs (roughly: cost in "b-tree page reads")
- estimatedRows drives outer/inner loop choice
- orderByConsumed = 1 only if you emit exactly the requested order
- colUsed (u64 bitmask) tells you which columns are needed — use it to skip work
- helper APIs: sqlite3_vtab_distinct(), sqlite3_vtab_in(), sqlite3_vtab_rhs_value(),
  sqlite3_vtab_collation(), sqlite3_vtab_nochange(), sqlite3_vtab_on_conflict()
```

**Common vtab bugs**
1. `sqlite3_vtab`/`sqlite3_vtab_cursor` not the first struct member.
2. `omit=1` without actually enforcing the constraint ⇒ silent wrong answers.
3. `orderByConsumed=1` without producing that order ⇒ silent wrong answers.
4. Forgetting `sqlite3_declare_vtab()` in `xConnect`.
5. Returning a small `estimatedCost` for an unbounded scan ⇒ catastrophic join plans.
6. Allocating with `malloc` instead of `sqlite3_malloc` (the error-message string in
   `pVtab->zErrMsg` **must** come from `sqlite3_mprintf`).
7. Not handling `xFilter` being called repeatedly on the same cursor (inner loop of a join).

## Hooks
```c
sqlite3_set_authorizer(db, xAuth, p);        /* SQLITE_OK / SQLITE_DENY / SQLITE_IGNORE */
sqlite3_trace_v2(db, SQLITE_TRACE_STMT|PROFILE|ROW|CLOSE, xTrace, p);
sqlite3_progress_handler(db, nOps, xProgress, p);   /* return non-zero to interrupt */
sqlite3_commit_hook(db, xCommit, p);         /* non-zero return converts COMMIT to ROLLBACK */
sqlite3_rollback_hook(db, xRollback, p);
sqlite3_update_hook(db, xUpdate, p);         /* INSERT/UPDATE/DELETE row events */
sqlite3_preupdate_hook(db, xPreUpdate, p);   /* needs SQLITE_ENABLE_PREUPDATE_HOOK */
sqlite3_wal_hook(db, xWal, p);
sqlite3_interrupt(db);                       /* thread-safe cancel */
```
Authorizer action codes worth knowing: `SQLITE_CREATE_TABLE`, `SQLITE_DROP_TABLE`,
`SQLITE_INSERT`, `SQLITE_UPDATE`, `SQLITE_DELETE`, `SQLITE_SELECT`, `SQLITE_READ`
(per column!), `SQLITE_FUNCTION`, `SQLITE_PRAGMA`, `SQLITE_ATTACH`, `SQLITE_RECURSIVE`.

## Hardening surface
```c
sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, 0);      /* block writable_schema etc */
sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DDL, 0, 0);
sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, 0, 0);
sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, 0);
sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, 0);
sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, 0);
sqlite3_limit(db, SQLITE_LIMIT_LENGTH|SQL_LENGTH|COLUMN|EXPR_DEPTH|..., n);
```
`PRAGMA trusted_schema=OFF` + `DEFENSIVE` + an authorizer is the standard configuration for
opening a database file you did not create.

## Test infrastructure

```
test/*.test            TCL test files (tester.tcl provides the harness)
test/tester.tcl        do_execsql_test, do_catchsql_test, do_test, ifcapable, reset_db
test/permutations.test named test permutations (wal, inmemory, journaltest, ...)
tool/omittest.tcl      builds with each SQLITE_OMIT_* to verify they still compile
mptest/                multi-process concurrency tests
fuzzcheck.c            corpus-driven fuzzer runner
test/fuzzdata*.db      fuzz corpora (SQL + database files)
```
```sh
make test          # "quick" ~ minutes
make fulltest      # hours
make releasetest   # the full configuration matrix
make valgrindtest
./testfixture ../test/X.test
./testfixture ../test/permutations.test wal
```

Test file idioms:
```tcl
do_execsql_test  NAME { SQL } {expected results}
do_catchsql_test NAME { SQL } {1 {error message}}
do_test          NAME { tcl script } {expected}
ifcapable fts5 { ... }                 ;# skip unless compiled in
reset_db                                ;# fresh database
db eval {...}                           ;# run SQL from TCL
# result patterns:  {/regex/} must match,  {~/regex/} must NOT match
```

Fault-injection test controls:
```c
SQLITE_TESTCTRL_FAULT_INSTALL      SQLITE_TESTCTRL_BENIGN_MALLOC_HOOKS
SQLITE_TESTCTRL_PENDING_BYTE       SQLITE_TESTCTRL_ASSERT / _ALWAYS
SQLITE_TESTCTRL_OPTIMIZATIONS      SQLITE_TESTCTRL_IMPOSTER
SQLITE_TESTCTRL_PRNG_SEED          SQLITE_TESTCTRL_LOCALTIME_FAULT
SQLITE_TESTCTRL_ONCE_RESET_THRESHOLD  SQLITE_TESTCTRL_SEEK_COUNT
```
`SQLITE_TESTCTRL_SEEK_COUNT` is handy for performance assertions: it counts b-tree seeks,
so a test can assert an algorithm did not regress.

## Contribution facts
```
Source of truth : Fossil at https://sqlite.org/src   (GitHub = read-only mirror)
License         : public domain; no CLA; outside code is rarely accepted
Discussion      : https://sqlite.org/forum , sqlite-users mailing list
Bug reports     : very welcome, usually fixed fast
Best artifacts  : minimal reproducer + dbtotxt payload + trunk verification
Docs            : also in the Fossil repo; corrections welcome
```
Downstream projects that accept pull requests and need exactly these skills:
`libsql` / `limbo` (Turso), `rusqlite`, `better-sqlite3`, `sqlite-utils`, `datasette`,
`sqlean`, `sqlite-vec`, plus the wider database world (DuckDB, PostgreSQL, RocksDB).

## Bug-report template
```
SQLite version:   3.x.y (and: does it reproduce on trunk?)
Compile options:  <output of PRAGMA compile_options>
Platform:         macOS 15 arm64, clang 17

Minimal reproducer:
  CREATE TABLE t(...);
  ...
  SELECT ...;     -- expected: X   actual: Y

Database (if data-dependent):  <tool/dbtotxt output>
Notes: first bad version (from bisecting), assertion text, ASan output
```

## Gotchas
1. Extension init symbol name must match the file basename, or be passed explicitly.
2. `sqlite3_malloc`/`sqlite3_free` for anything SQLite will free; never mix allocators.
3. `pVtab->zErrMsg` must be allocated with `sqlite3_mprintf`.
4. Virtual-table modules are per-connection; use `sqlite3_auto_extension` for all
   connections.
5. `SQLITE_DIRECTONLY` on any function with side effects — otherwise a malicious schema can
   invoke it via a view or trigger.
6. Shadow tables are writable unless `DEFENSIVE` mode is on; an attacker-supplied FTS5
   database can otherwise be used to corrupt state — this is why `xShadowName` exists.
7. Your `xBestIndex` may be called many times for one query (once per candidate join
   order). Keep it cheap and side-effect free.
8. `xFilter` restarts the scan; do not assume it is called once.
