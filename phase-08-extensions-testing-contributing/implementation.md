# Phase 8 — Implementation Labs: Build Extensions, Break Things, Contribute

A complete, verified virtual-table extension is checked in at
**`labs/phase08/seriesvtab.c`** (builds and runs on this machine).

---

## Lab 8.1 — ⭐ A working virtual table, end to end

```sh
cd ~/Desktop/sqllite/labs/phase08
A=~/Desktop/sqllite/sqlite-amalgamation-3500400

# 1. build a shell that CAN load extensions
#    (Apple's /usr/bin/sqlite3 is built with extension loading DISABLED)
cc -O1 -I$A -o sqlite3-ext $A/shell.c $A/sqlite3.c -lpthread -ldl -lm

# 2. build the extension
cc -fPIC -shared -I$A -o myseries.dylib seriesvtab.c      # .so on Linux

# 3. use it
./sqlite3-ext :memory: ".load ./myseries" \
  "SELECT value FROM myseries(1,10,3);" \
  "SELECT count(*) FROM myseries(1,100,1);" \
  "EXPLAIN QUERY PLAN SELECT value FROM myseries(1,10,1);" \
  "SELECT value FROM myseries WHERE start=5 AND stop=8;"
```

### Verified output
```
1
4
7
10
100
QUERY PLAN
`--SCAN myseries VIRTUAL TABLE INDEX 7:
5
6
7
8
```

`VIRTUAL TABLE INDEX 7` is your own `idxNum` (bits 1|2|4 = start, stop and step all
supplied) echoed back by the planner. That is the feedback loop for vtab development: EQP
shows you exactly which plan `xBestIndex` chose.

### The pieces, and why each exists

```c
/* the cursor MUST start with sqlite3_vtab_cursor */
struct SeriesCursor { sqlite3_vtab_cursor base; sqlite3_int64 iRowid, value, start, stop, step; };

/* declare the schema — HIDDEN columns become the function-style arguments */
sqlite3_declare_vtab(db, "CREATE TABLE x(value INTEGER, start HIDDEN, stop HIDDEN, step HIDDEN)");

/* xBestIndex: accept EQ constraints, ask for their values, and PROMISE to enforce them */
pIdxInfo->aConstraintUsage[i].argvIndex = ++nArg;   /* value arrives as argv[nArg-1] */
pIdxInfo->aConstraintUsage[i].omit      = 1;        /* "don't re-check this in the VDBE" */
pIdxInfo->idxNum = idxNum;                          /* my plan id, handed to xFilter */
pIdxInfo->estimatedCost = (bounded) ? 2.0 : 2147483647.0;
pIdxInfo->orderByConsumed = 1;                      /* I emit ascending values */

/* xFilter: start a scan using those values */
p->start = (idxNum & 1) ? sqlite3_value_int64(argv[0]) : 0;
```

**The `omit=1` trap:** setting it tells SQLite *not* to re-check the constraint. If your
`xFilter` then ignores it, you return wrong answers with no error. Test both ways.

**The eponymous trick:** `xCreate == 0` in the module makes the table usable by name with
no `CREATE VIRTUAL TABLE`.

### Exercises (do at least three)

1. Add a `WHERE value > ?` constraint to `xBestIndex` so ranges are pushed down.
   Prove it works by counting `xNext` calls (add a static counter and expose it via a
   function).
2. Make `orderByConsumed` conditional: honour `ORDER BY value DESC` by stepping backwards.
   Verify the sorter disappears from `EXPLAIN`.
3. Return a wrong `estimatedCost` (say 1.0 for the unbounded case) and watch a join plan
   become terrible. This is the vtab equivalent of bad statistics.
4. Write a **CSV virtual table**: `CREATE VIRTUAL TABLE t USING mycsv('file.csv')`, using
   `xCreate` + `xConnect` with arguments in `argv[3..]`.
5. Write a **writable** vtab by implementing `xUpdate` (argc/argv encoding: 1 arg = DELETE,
   argv[0]==NULL = INSERT, otherwise UPDATE) backed by an in-memory array.
6. Write a vtab over a real external source (a directory listing, `/proc`, an HTTP JSON
   endpoint cached in memory).

---

## Lab 8.2 — Read the shipped extensions

```sh
cd ~/Desktop/sqllite/sqlite-src
wc -l ext/misc/series.c ext/misc/csv.c ext/misc/carray.c ext/misc/regexp.c
wc -l ext/rtree/rtree.c ext/fts5/*.c | tail -3
```

Compare `ext/misc/series.c` with your `seriesvtab.c` line by line and write
`labs/phase08/series-diff.md`: what does the official one handle that yours does not?
(Hint: `SQLITE_INDEX_CONSTRAINT_GT/LE` on `value`, descending order, overflow, `colUsed`.)

Then the shadow-table reveal:
```sh
./build/sqlite3 fts.db "CREATE VIRTUAL TABLE docs USING fts5(title, body);
  INSERT INTO docs VALUES('SQLite internals','the b-tree module lives in btree.c');
  SELECT name,type FROM sqlite_schema;"
./build/sqlite3 fts.db "SELECT * FROM docs WHERE docs MATCH 'btree';"
./build/sqlite3 fts.db "SELECT rank,* FROM docs WHERE docs MATCH 'btree' ORDER BY rank;"
./build/sqlite3 fts.db "SELECT quote(block) FROM docs_data LIMIT 3;"   -- the real storage
```

`labs/phase08/fts5-notes.md`: list every shadow table FTS5 created and say what each holds.
Then use your Phase 1 `dbparse` on the database and find the FTS5 b-trees.

Do the same for R-Tree:
```sh
./build/sqlite3 rt.db "CREATE VIRTUAL TABLE boxes USING rtree(id,minX,maxX,minY,maxY);
  INSERT INTO boxes VALUES(1,0,10,0,10),(2,5,15,5,15);
  SELECT name FROM sqlite_schema;
  EXPLAIN QUERY PLAN SELECT id FROM boxes WHERE minX<=6 AND maxX>=6;"
```

---

## Lab 8.3 — Authorizer: run untrusted SQL safely

```c
/* labs/phase08/authz.c — build: cc -o authz authz.c -lsqlite3 */
#include <stdio.h>
#include <string.h>
#include "sqlite3.h"

static int xAuth(void *pUser, int op, const char *z1, const char *z2,
                 const char *zDb, const char *zTrigger){
  (void)pUser; (void)zDb; (void)zTrigger;
  switch( op ){
    case SQLITE_SELECT:
    case SQLITE_READ:
    case SQLITE_FUNCTION:
      return SQLITE_OK;
    case SQLITE_PRAGMA:
      return SQLITE_DENY;                 /* no pragmas from untrusted SQL */
    default:
      printf("  [denied op=%d %s %s]\n", op, z1?z1:"", z2?z2:"");
      return SQLITE_DENY;                 /* read-only sandbox */
  }
}

int main(void){
  sqlite3 *db; char *err=0; sqlite3_stmt *st;
  const char *aTest[] = {
    "SELECT count(*) FROM t",
    "INSERT INTO t VALUES(99)",
    "DROP TABLE t",
    "PRAGMA journal_mode=WAL",
    "ATTACH '/etc/passwd' AS x",
  };
  int i;
  sqlite3_open(":memory:", &db);
  sqlite3_exec(db, "CREATE TABLE t(a); INSERT INTO t VALUES(1),(2);", 0,0,&err);
  sqlite3_set_authorizer(db, xAuth, 0);
  for(i=0; i<5; i++){
    printf("%-40s -> ", aTest[i]);
    if( sqlite3_prepare_v2(db, aTest[i], -1, &st, 0)!=SQLITE_OK ){
      printf("REJECTED: %s\n", sqlite3_errmsg(db));
    }else{
      printf("allowed\n");
      sqlite3_finalize(st);
    }
  }
  sqlite3_close(db);
  return 0;
}
```

Extend it into a real policy: allow reads only from specific tables, deny specific columns
(`SQLITE_READ` gives you table and column names — return `SQLITE_IGNORE` to make the column
read as NULL instead of failing). Deliverable: `labs/phase08/authz.c` + a policy table.

Also explore the hardening surface:
```sql
PRAGMA trusted_schema=OFF;
```
```c
sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, 0);
sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, 0);
sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, 0, 0);
sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 1000000);
sqlite3_progress_handler(db, 10000, xInterrupt, 0);   /* kill runaway queries */
```

---

## Lab 8.4 — Run the real test suite and read a test

```sh
cd ~/Desktop/sqllite/sqlite-src/build
make testfixture
./testfixture ../test/select1.test  2>&1 | tail -3
./testfixture ../test/btree01.test  2>&1 | tail -3
./testfixture ../test/wal.test      2>&1 | tail -3
./testfixture ../test/corruptC.test 2>&1 | tail -3
./testfixture ../test/fuzzcheck.test 2>&1 | tail -3 || true
```

Read `test/btree01.test` and `test/corruptC.test` and write `labs/phase08/test-anatomy.md`
explaining `do_execsql_test`, `do_catchsql_test`, `do_test`, `ifcapable`, and how corruption
tests build a broken file (look for `SQLITE_TESTCTRL_IMPOSTER` and `hexio_write`).

---

## Lab 8.5 — ⭐ Write and submit-quality a regression test

Pick a behaviour you verified in an earlier phase (e.g. "an index on a BINARY column is not
used for a NOCASE comparison", or "`INSERT OR FAIL` leaves prior rows"). Write it as a
proper TCL test file:

```tcl
# labs/phase08/mytest.test
set testdir [file dirname $argv0]
source $testdir/tester.tcl
set testprefix mytest

do_execsql_test 1.0 {
  CREATE TABLE t1(a TEXT, b INT);
  CREATE INDEX i1 ON t1(a);
  INSERT INTO t1 VALUES('ABC',1),('abc',2);
} {}

do_execsql_test 1.1 {
  EXPLAIN QUERY PLAN SELECT * FROM t1 WHERE a='abc';
} {~/SCAN/}                      ;# must use the index

do_execsql_test 1.2 {
  EXPLAIN QUERY PLAN SELECT * FROM t1 WHERE a='abc' COLLATE NOCASE;
} {/SCAN/}                       ;# must NOT use the index

finish_test
```

```sh
/bin/cp mytest.test ~/Desktop/sqllite/sqlite-src/test/
cd ~/Desktop/sqllite/sqlite-src/build && ./testfixture ../test/mytest.test
```

Deliverable: a passing test file. **This is a genuinely submittable artifact** — a test that
pins down a subtle behaviour is welcome on the SQLite forum.

---

## Lab 8.6 — Coverage archaeology

```sh
cd ~/Desktop/sqllite/sqlite-src
mkdir -p cov && cd cov
../configure --enable-debug CFLAGS="-g -O0 -fprofile-arcs -ftest-coverage"
make testfixture
./testfixture ../test/select1.test
gcov -o . sqlite3.c 2>/dev/null | tail -5
grep -n "#####" sqlite3.c.gcov | head -40      # uncovered lines
```

Find 10 uncovered lines in a module you know (btree/pager/where) and, for each, write down
what input would reach it. Turn the three easiest into test cases.

`labs/phase08/coverage.md`: the list, your inputs, and which ones you managed to cover.
This is the single most useful "first contribution" exercise for any database project.

---

## Lab 8.7 — Fuzz your own code

Fuzz the virtual table you wrote (or your `dbparse`/`walparse` tools, which parse
attacker-controlled bytes — a far more realistic target).

```sh
cd ~/Desktop/sqllite/labs/phase08
# libFuzzer harness for walparse's decoder
cat > fuzz_wal.c <<'EOF2'
#include <stdint.h>
#include <stddef.h>
int walparse_buffer(const unsigned char *p, long n);  /* refactor walparse to expose this */
int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size){
  walparse_buffer(Data, (long)Size);
  return 0;
}
EOF2
# cc -g -fsanitize=fuzzer,address -o fuzz_wal fuzz_wal.c walparse_lib.c
# ./fuzz_wal -max_total_time=60 corpus/
```

Also run SQLite's own fuzzer:
```sh
cd ~/Desktop/sqllite/sqlite-src/build
make fuzzcheck
./fuzzcheck ../test/fuzzdata1.db
./fuzzcheck ../test/fuzzdata8.db
```

`labs/phase08/fuzzing.md`: what you fuzzed, how many executions, any crashes, and the
minimized reproducer for each. Every crash in *your* parser is a lesson in how paranoid
`btree.c` has to be.

---

## Lab 8.8 — Session extension: change tracking

```sh
cd ~/Desktop/sqllite/sqlite-src/build
./sqlite3 :memory: ".help" >/dev/null   # ensure session is compiled in (--enable-all)
```
```c
/* labs/phase08/sess.c (sketch) — build against a session-enabled build */
sqlite3session_create(db, "main", &pSession);
sqlite3session_attach(pSession, NULL);             /* all tables */
/* ... do some INSERT/UPDATE/DELETE ... */
sqlite3session_changeset(pSession, &nChange, &pChange);
/* apply to another database: */
sqlite3changeset_apply(db2, nChange, pChange, 0, xConflict, 0);
```

Build a tiny two-database sync demo and write `labs/phase08/session.md` on: what a changeset
contains, how conflicts are reported, and why this is built on `preupdate_hook`.

---

## Lab 8.9 — Produce a submission-quality bug report

Even if you have no bug yet, practise the artifact:

```sh
cd ~/Desktop/sqllite/sqlite-src
cc -o /tmp/dbtotxt tool/dbtotxt.c
/tmp/dbtotxt ~/Desktop/sqllite/labs/phase01/mini.db > /tmp/mini.txt
head -20 /tmp/mini.txt
```

Write `labs/phase08/bug-report-template.md` containing a filled-in example with: version,
compile options, minimal SQL, `dbtotxt` payload, expected vs actual, and trunk verification
steps. Keep it; when you find a real bug you will be ready in five minutes instead of an
afternoon.

---

## Lab 8.10 — Capstone: pick one and ship it

Choose **one** and finish it properly (README, tests, build instructions):

1. **A useful virtual table** — e.g. over Parquet/CSV/JSONL, a system resource, or an API.
2. **A VFS** — encryption, compression, quota, or a fault-injection VFS you can use to test
   *other people's* SQLite applications.
3. **A diagnostic tool** — combine your `dbparse` + `walparse` into a single
   `sqlite-forensics` CLI that dumps a database, its WAL, and reports on free space,
   fill factor, and recoverable deleted rows.
4. **A planner analyzer** — a tool that runs a query workload, captures plans, flags
   `SCAN`/`AUTOMATIC INDEX`/temp b-trees, and recommends indexes (compare yours with
   `ext/expert/`).
5. **A contribution to a downstream project** — find an open issue in `libsql`, `rusqlite`,
   `datasette`, or `sqlite-utils` that needs internals knowledge, and fix it.

Deliverable: a public repository, plus `labs/phase08/capstone.md` describing what you built,
what you learned, and what you would do next.
