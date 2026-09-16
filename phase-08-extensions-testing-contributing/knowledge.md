# Phase 8 — Extensions, Testing, and Actually Contributing

You now know the engine. This phase covers the two things that turn knowledge into
contribution: the **extension surfaces** (where you can add capability without touching the
core) and the **test infrastructure** (which is what the core actually is).

---

## 8.1 The five extension surfaces

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │  1. SQL functions      scalar / aggregate / window                       │
 │  2. Collating sequences                                                  │
 │  3. Virtual tables     a table backed by *your* code                     │
 │  4. VFS shims          storage, encryption, instrumentation, faults      │
 │  5. Hooks & callbacks  authorizer, trace, progress, commit/update/WAL     │
 └──────────────────────────────────────────────────────────────────────────┘
```

Everything a "SQLite extension" does is one of these. FTS5, R-Tree, JSON, the session
extension, `dbstat`, `bytecode()` — all of them.

### 1. Functions

```c
sqlite3_create_function_v2(db, "myfunc", nArg,
    SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS | SQLITE_DIRECTONLY,
    pUserData, xFunc, xStep, xFinal, xDestroy);
sqlite3_create_window_function(db, "mywin", nArg, flags, p,
    xStep, xFinal, xValue, xInverse, xDestroy);
```
Flags that matter:
- `SQLITE_DETERMINISTIC` — same args ⇒ same result; enables `OP_PureFunc` and hoisting out
  of loops, and allows the function in index expressions and partial-index WHERE clauses.
- `SQLITE_INNOCUOUS` — safe for use in triggers/views on untrusted schemas.
- `SQLITE_DIRECTONLY` — may only be called from top-level SQL, not from triggers/views.
  Use this for anything with side effects; it is a security boundary.

Inside: `sqlite3_value_*()` to read arguments, `sqlite3_result_*()` to return,
`sqlite3_aggregate_context()` for aggregate state, `sqlite3_get_auxdata()` /
`set_auxdata()` to cache per-argument work (e.g. a compiled regex) across rows.

### 2. Collations
```c
sqlite3_create_collation_v2(db, "MYCOLL", SQLITE_UTF8, p, xCompare, xDestroy);
sqlite3_collation_needed(db, p, xCollNeeded);   /* lazy registration */
```
Note: an index built with a collation is only usable by queries with the *same* collation,
and if you change a collation's behaviour you must `REINDEX`.

### 3. Virtual tables — the big one

A virtual table is a table whose rows are produced by your C code. The module interface:

```c
struct sqlite3_module {
  int iVersion;
  int (*xCreate)(sqlite3*, void*, int argc, const char*const*argv,
                 sqlite3_vtab**, char**);     /* CREATE VIRTUAL TABLE ... */
  int (*xConnect)(...);                        /* re-open an existing one */
  int (*xBestIndex)(sqlite3_vtab*, sqlite3_index_info*);   /* the planner interface */
  int (*xDisconnect)(sqlite3_vtab*);
  int (*xDestroy)(sqlite3_vtab*);              /* DROP TABLE */
  int (*xOpen)(sqlite3_vtab*, sqlite3_vtab_cursor**);
  int (*xClose)(sqlite3_vtab_cursor*);
  int (*xFilter)(sqlite3_vtab_cursor*, int idxNum, const char *idxStr,
                 int argc, sqlite3_value **argv);          /* start a scan */
  int (*xNext)(sqlite3_vtab_cursor*);
  int (*xEof)(sqlite3_vtab_cursor*);
  int (*xColumn)(sqlite3_vtab_cursor*, sqlite3_context*, int N);
  int (*xRowid)(sqlite3_vtab_cursor*, sqlite3_int64 *pRowid);
  int (*xUpdate)(sqlite3_vtab*, int argc, sqlite3_value **argv, sqlite3_int64*);
  int (*xBegin)(sqlite3_vtab*); int (*xSync)(...); int (*xCommit)(...); int (*xRollback)(...);
  int (*xFindFunction)(...);                   /* overload functions for this table */
  int (*xRename)(sqlite3_vtab*, const char *zNew);
  /* v2: */ int (*xSavepoint)(...), (*xRelease)(...), (*xRollbackTo)(...);
  /* v3: */ int (*xShadowName)(const char*);
  /* v4: */ int (*xIntegrity)(...);
};
```

**`xBestIndex` is the whole game.** SQLite hands you the constraints it would like you to
handle and asks for a cost:

```c
struct sqlite3_index_info {
  /* INPUTS */
  int nConstraint;
  struct sqlite3_index_constraint { int iColumn, op; unsigned char usable; int iTermOffset; } *aConstraint;
  int nOrderBy;
  struct sqlite3_index_orderby { int iColumn; unsigned char desc; } *aOrderBy;
  /* OUTPUTS */
  struct sqlite3_index_constraint_usage { int argvIndex; unsigned char omit; } *aConstraintUsage;
  int idxNum;                 /* your plan id, passed to xFilter */
  char *idxStr; int needToFreeIdxStr;
  int orderByConsumed;        /* 1 = I will produce rows in the requested order */
  double estimatedCost;
  sqlite3_int64 estimatedRows;
  int idxFlags;               /* SQLITE_INDEX_SCAN_UNIQUE */
  sqlite3_uint64 colUsed;     /* which columns the query needs */
};
```

Contract:
- `argvIndex = k` means "pass this constraint's value as `argv[k-1]` to `xFilter`".
- `omit = 1` means "don't re-check this constraint in the VDBE; I guarantee it".
  Lying here produces **wrong answers**, not errors.
- `estimatedCost` competes with real b-tree costs; return a huge number for plans you don't
  want (that's how `myseries` refuses an unbounded scan).
- `orderByConsumed = 1` removes the sorter — only claim it if you truly produce that order.

**Eponymous virtual tables** (`xCreate == 0` or `xCreate == xConnect`) can be used by name
with no `CREATE VIRTUAL TABLE`: `SELECT * FROM generate_series(1,10)`. Hidden columns
become the "arguments".

**Table-valued functions** are exactly eponymous vtabs with HIDDEN columns.

### 4. VFS — covered in Phase 2. Real uses: encryption (SEE, SQLCipher), compression
(ZIPVFS), in-memory, `unix-excl`, quota, test fault injection, network shims.

### 5. Hooks
```c
sqlite3_set_authorizer(db, xAuth, p);   /* per-action permission checks at PREPARE time */
sqlite3_trace_v2(db, mask, xTrace, p);  /* STMT, PROFILE, ROW, CLOSE events */
sqlite3_progress_handler(db, nOps, xProgress, p);   /* interruptibility */
sqlite3_commit_hook / sqlite3_rollback_hook / sqlite3_update_hook(db, ...);
sqlite3_wal_hook(db, ...);
sqlite3_preupdate_hook(db, ...);        /* needs SQLITE_ENABLE_PREUPDATE_HOOK */
sqlite3_collation_needed(db, ...);
```
The **authorizer** is the security surface: it is called during preparation for every table,
column, and action, and can return `SQLITE_DENY` / `SQLITE_IGNORE`. That is how you safely
run untrusted SQL.

---

## 8.2 The shipped extensions worth reading

| Extension | What to learn from it |
|---|---|
| `ext/misc/series.c` | the canonical minimal vtab (your `myseries` is a cousin) |
| `ext/misc/csv.c` | a vtab over a file; argument parsing, schema inference |
| `ext/misc/carray.c` | binding a C array into SQL — the `sqlite3_bind_pointer` pattern |
| `ext/misc/regexp.c` | a function with `auxdata` caching of the compiled pattern |
| `ext/misc/json.c` (now core `json.c`) | JSONB, parsing, path expressions |
| `ext/rtree/rtree.c` | a real index structure implemented as a vtab, with shadow tables |
| `ext/fts5/` | the biggest one: an inverted index, tokenizers, ranking (bm25), a query language |
| `ext/session/` | change tracking and changesets via `preupdate_hook` |
| `ext/rbu/` | resumable bulk update — incremental application of huge changes |
| `ext/expert/` | an index recommender that uses the planner's own machinery |
| `ext/misc/dbdump.c`, `tool/showdb.c`, `tool/showwal.c` | the format tools from Phases 1 & 7 |

**Shadow tables**: FTS5 and R-Tree store their real data in ordinary tables named
`<vtab>_data`, `<vtab>_idx`, `<vtab>_content`, `<vtab>_node`… Look at them with
`SELECT name FROM sqlite_schema` after creating an FTS5 table — the "virtual" table is a
thin layer over ordinary b-trees. `xShadowName` tells SQLite which names are shadows so
`PRAGMA writable_schema` and defensive mode can protect them.

### FTS5 in one page
```sql
CREATE VIRTUAL TABLE docs USING fts5(title, body, tokenize='porter unicode61');
INSERT INTO docs VALUES('SQLite internals','the b-tree module is in btree.c');
SELECT * FROM docs WHERE docs MATCH 'btree';
SELECT rank, * FROM docs WHERE docs MATCH 'btree' ORDER BY rank;      -- bm25
SELECT highlight(docs, 1, '[', ']') FROM docs WHERE docs MATCH 'btree';
SELECT * FROM docs('b*');                                             -- prefix query
```
Internals: an inverted index stored as a doclist per term, in b-tree pages, with
incremental merging (segments, like an LSM). `fts5_index.c` is where the merge logic lives;
`fts5_storage.c` is the content table; `fts5_expr.c` is the MATCH query parser.

### R-Tree in one page
```sql
CREATE VIRTUAL TABLE boxes USING rtree(id, minX, maxX, minY, maxY);
SELECT id FROM boxes WHERE minX<=10 AND maxX>=5 AND minY<=10 AND maxY>=5;
```
Internals: an R-tree whose nodes are BLOBs in a shadow table `boxes_node`. Splitting uses a
linear/quadratic split algorithm, and `xBestIndex` reports much lower costs for
bounding-box constraints.

---

## 8.3 How SQLite is actually tested

This is the part most engineers have never seen, and it is why SQLite is trusted.

```
  TCL tests (test/*.test)      ~ the public suite; tens of thousands of cases
  TH3                          ~ proprietary; 100% MC/DC branch coverage for embedded
  SQL Logic Test (SLT)         ~ millions of statements cross-checked against other DBs
  fuzzcheck + dbsqlfuzz        ~ mutates BOTH SQL text and database FILES
  OSS-Fuzz                     ~ continuous fuzzing, 24/7
  crash/IO-error/OOM injection ~ every single I/O call site and malloc can be made to fail
  valgrind / ASan / UBSan / MSan runs
```

Key numbers the project publishes: ~600× more test code than library code; 100% branch
coverage and 100% MC/DC coverage under TH3.

### The mechanisms you can use

**Fault injection** — the same machinery the suite uses:
```c
sqlite3_test_control(SQLITE_TESTCTRL_FAULT_INSTALL, xCallback);
sqlite3_test_control(SQLITE_TESTCTRL_PRNG_SEED, seed, db);
sqlite3_test_control(SQLITE_TESTCTRL_ALWAYS, x);
sqlite3_test_control(SQLITE_TESTCTRL_OPTIMIZATIONS, db, mask);
sqlite3_test_control(SQLITE_TESTCTRL_LOCALTIME_FAULT, onoff);
sqlite3_test_control(SQLITE_TESTCTRL_IMPOSTER, db, zDb, onOff, tnum);  /* corruption tests */
```
`SQLITE_TESTCTRL_IMPOSTER` is a gem: it lets a test declare a fake schema over an existing
root page, so you can write *deliberately corrupt* b-tree content and check that the
library rejects it.

**The TCL suite**
```sh
./testfixture ../test/select1.test
./testfixture ../test/btree01.test
./testfixture ../test/wal.test
./testfixture ../test/corrupt*.test
make test                # the "quick" suite
make fulltest            # much longer
make releasetest         # the full matrix of build configurations
```
A test file looks like:
```tcl
do_execsql_test 1.1 {
  CREATE TABLE t1(a,b);
  INSERT INTO t1 VALUES(1,2);
  SELECT * FROM t1;
} {1 2}

do_catchsql_test 1.2 {
  SELECT * FROM nosuchtable;
} {1 {no such table: nosuchtable}}
```
That is the entire format for 90% of tests. Adding a regression test is genuinely easy.

**Fuzzing**
```sh
make fuzzcheck
./fuzzcheck ../test/fuzzdata1.db          # replay a corpus
./fuzzcheck --help
# dbsqlfuzz (the mutation fuzzer that finds most modern bugs) lives outside the tree
```
A fuzzer finding is the highest-value bug report you can file, because it comes with a
minimal reproducible artifact.

**Coverage**
```sh
# gcov-based coverage of the TCL suite
make clean && make testfixture OPTS="-fprofile-arcs -ftest-coverage"
./testfixture ../test/all.test
gcov sqlite3.c | tail
```
Reading which branches are *not* covered by the public suite is an education in itself.

---

## 8.4 How a change actually lands in SQLite

Be realistic about this, because it determines where you spend effort.

**Facts:**
- SQLite is in the **public domain**. There is no CLA, and the project deliberately does
  **not** accept most outside code, precisely to keep the public-domain status clean.
- Development happens in **Fossil** at `sqlite.org/src`. GitHub is a read-only mirror; PRs
  there are not reviewed.
- Communication: the `sqlite-users` mailing list and the forum at `sqlite.org/forum`.
- Bug reports **are** genuinely welcome and are usually fixed within days.

**What works (ranked by value):**
1. **A minimal reproducible bug report.** Especially: wrong answers, crashes, corruption,
   or a fuzzer-found case. Use `tool/dbtotxt.c` to include the database as text, and
   reduce the SQL to the smallest failing case.
2. **A test case** demonstrating a subtle behaviour, posted to the forum.
3. **Documentation corrections** — the docs are in the Fossil repo too.
4. **Extensions**: publish your own vtab/VFS/function library. This is unbounded.
5. **Downstream forks that take PRs**: `libsql` (Turso), `sqlite-wasm`, language bindings
   (`rusqlite`, `better-sqlite3`, `python-sqlite3`), tooling (`sqlite-utils`, `datasette`).
6. **Other databases** where your new skills transfer directly and contribution is open:
   PostgreSQL, DuckDB, MySQL/InnoDB, RocksDB, ClickHouse, TiKV, libSQL/limbo.

**A good bug report contains:**
```
- SQLite version and PRAGMA compile_options
- the smallest SQL that reproduces it, runnable in the shell
- the database (as dbtotxt output) if it is data-dependent
- expected vs. actual output
- whether it reproduces on the latest trunk build
- for crashes: the assertion text or ASan report
```

---

## 8.5 A realistic contribution ladder

```
 week 1-2   run the test suite; read a failing area; write a new .test file that
            covers an under-tested branch (find it with gcov)
 week 3-4   build and publish a virtual table extension solving a real problem
 month 2    fuzz something (dbsqlfuzz, or your own mutation harness over your vtab);
            file any findings
 month 2-3  pick a downstream project (libsql / rusqlite / datasette) and fix a real
            issue there, using your internals knowledge as the advantage
 month 4+   pick a database with open contribution (DuckDB / Postgres) and take a
            "good first issue" in the storage or planner layer
```

The point of this curriculum was never to get a commit bit on `sqlite.org`. It was to make
you one of the small number of engineers who can *read a storage engine*. That skill is
what gets you into any database team.

---

## 8.6 Where to go deeper after this

- **Papers/books:** *Architecture of a Database System* (Hellerstein, Stonebraker, Hamilton);
  *Database Internals* (Petrov); *Designing Data-Intensive Applications* (Kleppmann);
  the ARIES recovery paper; the original B-tree papers (Bayer & McCreight).
- **Codebases in increasing difficulty:** SQLite → DuckDB (vectorized execution) →
  RocksDB/LevelDB (LSM) → PostgreSQL (MVCC, WAL, extensibility) → InnoDB.
- **Adjacent SQLite topics:** `sqlite3_deserialize`/`serialize`, `sqlite3_load_extension`
  security, the `session` extension for sync, `rbu` for incremental updates, WASM builds,
  `SQLITE_OMIT_*` minimal builds for embedded, and the `os_kv.c` key-value VFS.
