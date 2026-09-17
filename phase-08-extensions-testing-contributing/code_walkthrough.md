# Phase 8 — Code Walkthrough: Extension Code

Source:
[`ext/misc/series.c`](https://github.com/sqlite/sqlite/blob/master/ext/misc/series.c) ·
[`src/dbstat.c`](https://github.com/sqlite/sqlite/blob/master/src/dbstat.c) ·
[`ext/misc/carray.c`](https://github.com/sqlite/sqlite/blob/master/ext/misc/carray.c) ·
[`ext/fts5/`](https://github.com/sqlite/sqlite/tree/master/ext/fts5) ·
[`ext/rtree/rtree.c`](https://github.com/sqlite/sqlite/blob/master/ext/rtree/rtree.c) ·
spec: [vtab.html](https://sqlite.org/vtab.html)

---

## Mental model for this phase

> **A virtual table is a cursor and a cost function.** `xFilter`/`xNext`/`xEof`/`xColumn`
> are the cursor; `xBestIndex` is the cost function. Everything else is lifecycle.

And the rule that decides whether your vtab is fast or useless:

> **`xBestIndex` is a negotiation, and `omit`/`orderByConsumed` are promises.** SQLite
> offers you constraints; you say which you will enforce and what it will cost. Lying
> produces wrong answers, not errors.

`ext/` code is the easiest code in the project to read — it is self-contained, each file is
one feature, and the file-top comments are tutorials.

---

## 1. `statBestIndex()` — a real `xBestIndex`, complete

From `src/dbstat.c` (the `dbstat` virtual table you used in Phase 3 to measure page fill).
Real code:

```c
static int statBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo){
  int i;
  int iSchema = -1;
  int iName = -1;
  int iAgg = -1;

  /* Look for a valid schema=? constraint.  If found, change the idxNum to
  ** 1 and request the value of that constraint be sent to xFilter.  And
  ** lower the cost estimate to encourage the constrained version to be
  ** used. */
  for(i=0; i<pIdxInfo->nConstraint; i++){
    if( pIdxInfo->aConstraint[i].op!=SQLITE_INDEX_CONSTRAINT_EQ ) continue;
    if( pIdxInfo->aConstraint[i].usable==0 ){
      /* Force DBSTAT table should always be the right-most table in a join */
      return SQLITE_CONSTRAINT;
    }
    switch( pIdxInfo->aConstraint[i].iColumn ){
      case 0:  { iName = i;   break; }   /* name */
      case 10: { iSchema = i; break; }   /* schema */
      case 11: { iAgg = i;    break; }   /* aggregate */
    }
  }
  i = 0;
  if( iSchema>=0 ){
    pIdxInfo->aConstraintUsage[iSchema].argvIndex = ++i;
    pIdxInfo->aConstraintUsage[iSchema].omit = 1;
    pIdxInfo->idxNum |= 0x01;
  }
  if( iName>=0 ){
    pIdxInfo->aConstraintUsage[iName].argvIndex = ++i;
    pIdxInfo->idxNum |= 0x02;
  }
  if( iAgg>=0 ){
    pIdxInfo->aConstraintUsage[iAgg].argvIndex = ++i;
    pIdxInfo->idxNum |= 0x04;
  }
  pIdxInfo->estimatedCost = 1.0;

  /* Records are always returned in ascending order of (name, path).
  ** If this will satisfy the client, set the orderByConsumed flag so that
  ** SQLite does not do an external sort. */
  if( ( pIdxInfo->nOrderBy==1
     && pIdxInfo->aOrderBy[0].iColumn==0
     && pIdxInfo->aOrderBy[0].desc==0 ) || ... ){
    pIdxInfo->orderByConsumed = 1;
  }
  return SQLITE_OK;
}
```

Line by line, against the contract in `internals_important.md`:

| Code | Contract point |
|---|---|
| `if( ...aConstraint[i].usable==0 ) return SQLITE_CONSTRAINT;` | **Refusing a plan.** `usable==0` means "this constraint's value comes from a table that would be *inside* your loop", i.e. dbstat would be the outer table of a join. Returning `SQLITE_CONSTRAINT` from `xBestIndex` tells the planner "this join order is impossible for me" — and the planner tries another. Most vtabs should do this rather than silently accept a plan they cannot execute. |
| `switch( pIdxInfo->aConstraint[i].iColumn )` | column numbers are **positions in the `CREATE TABLE` you passed to `sqlite3_declare_vtab()`**. Hidden columns are included in the numbering — `schema` is column 10 here. |
| `argvIndex = ++i;` | "send me this constraint's value as `argv[i-1]` in `xFilter`". The order is yours to choose; here it is schema, name, aggregate. |
| `omit = 1` **only on schema** | dbstat fully enforces `schema=?` (it only walks that database), so SQLite need not re-check it. `name` and `aggregate` get `argvIndex` but **not** `omit` — dbstat uses them as hints and lets the VDBE re-check. **This is the correct conservative default**: take the value, don't promise. |
| `idxNum \|= 0x01 / 0x02 / 0x04` | a bitmask plan id, exactly like your `myseries` — and exactly what you saw as `VIRTUAL TABLE INDEX 7` in Phase 8 Lab 8.1's `EXPLAIN QUERY PLAN`. |
| `estimatedCost = 1.0;` | dbstat always claims to be cheap. Fine for a diagnostic table; wrong for a vtab that might be scanned in an inner loop. |
| `orderByConsumed = 1;` guarded by an exact shape test | the promise is only made when the requested `ORDER BY` is *precisely* what dbstat naturally produces: one or two terms, specific columns, ascending. **Note how narrow the test is.** That is the correct level of paranoia for this flag. |

Compare this to your own `seriesvtab.c` from Lab 8.1: same structure, same four outputs
(`argvIndex`, `omit`, `idxNum`, `estimatedCost`), different policy.

---

## 2. Reading `ext/misc/series.c` — the reference vtab

Your `labs/phase08/seriesvtab.c` is a simplified cousin. Diff yours against the original and
you will find three things you skipped:

1. **Range constraints on `value`.** The real one handles
   `SQLITE_INDEX_CONSTRAINT_GT/GE/LT/LE` on the `value` column, not just equality on the
   hidden arguments — so `SELECT value FROM generate_series(1,1000000) WHERE value>999990`
   does not iterate a million times.
2. **Descending output.** It inspects `pIdxInfo->aOrderBy[0].desc` and sets a bit in
   `idxNum`, then `xFilter` starts at `stop` and steps backwards, allowing
   `orderByConsumed=1` for `ORDER BY value DESC`.
3. **Overflow and edge cases:** a zero or negative `step`, a `stop` below `start`, and
   integer overflow on `value += step`.

```sh
# read it next to yours
$EDITOR ~/Desktop/sqllite/sqlite-src/ext/misc/series.c \
        ~/Desktop/sqllite/labs/phase08/seriesvtab.c
```

The file-top comment in `series.c` is a complete tutorial on eponymous virtual tables and
hidden columns — better than most documentation.

---

## 3. `carray.c` — the pointer-passing pattern worth stealing

[Source](https://github.com/sqlite/sqlite/blob/master/ext/misc/carray.c). It solves "bind a
C array into SQL":

```c
/* application side */
sqlite3_prepare_v2(db, "SELECT * FROM t WHERE id IN (SELECT value FROM carray(?1,?2))", ...);
sqlite3_bind_pointer(pStmt, 1, aData, "carray", 0);
sqlite3_bind_int(pStmt, 2, nData);
```

The mechanism is `sqlite3_bind_pointer()` / `sqlite3_value_pointer()`: a pointer can be
bound to a parameter **only with a matching type string**, and `sqlite3_value_pointer()`
returns NULL unless the type string matches exactly. That type tag is what makes it safe —
SQL text can never forge a pointer, because there is no SQL syntax that produces one.

Read this file when you want to pass host-language data into a query without building a
temp table. The same pattern appears in `ext/misc/pointer` uses across the codebase.

---

## 4. FTS5 — mechanism only, and how to navigate 30k lines

Do **not** read FTS5 linearly. It is six files with clean responsibilities:

| File | Responsibility |
|---|---|
| `fts5_main.c` | the vtab interface: xCreate/xConnect/xBestIndex/xFilter/xUpdate, the `MATCH` operator, auxiliary functions (`bm25`, `highlight`, `snippet`) |
| `fts5_expr.c` | parses the MATCH query language (`a AND b`, `"phrase"`, `col:term`, `NEAR(...)`, `a*`) into an expression tree |
| `fts5_index.c` | **the actual inverted index**: segments, doclists, position lists, merging |
| `fts5_storage.c` | the `%_content` / `%_docsize` / `%_config` shadow tables |
| `fts5_tokenize.c` | tokenizers: `unicode61`, `porter`, `ascii`, `trigram` |
| `fts5_hash.c` | the in-memory hash of pending (unflushed) terms |

The data model in four sentences:

```
A document is tokenized into terms; for each term FTS5 stores a DOCLIST:
    term → [ (rowid, [positions]), (rowid, [positions]), ... ]   delta-encoded varints
Doclists live in SEGMENTS. New writes go to an in-memory hash, then flush to a new segment.
Segments are merged in levels (like an LSM tree) so reads don't degrade as writes accumulate.
Everything is stored as BLOBs in ordinary b-tree shadow tables — so Phases 1-3 still apply.
```

Prove that last sentence with tools you already built:

```sh
~/Desktop/sqllite/sqlite-src/build/sqlite3 fts.db \
  "CREATE VIRTUAL TABLE docs USING fts5(title, body);
   INSERT INTO docs VALUES('SQLite internals','the b-tree module lives in btree.c');
   SELECT name FROM sqlite_schema;"
# docs, docs_data, docs_idx, docs_content, docs_docsize, docs_config
~/Desktop/sqllite/labs/phase01/dbparse fts.db      # walk the shadow tables yourself
```

Where to start reading if you have a specific question:
- "why is my MATCH query slow?" → `fts5_index.c`, functions `fts5MultiIterNew` /
  `fts5SegIterNext` (the doclist merge), plus `fts5_expr.c` for how the query decomposed.
- "how does ranking work?" → `fts5_aux.c`, `fts5Bm25Function`.
- "what does the optimize command do?" → `fts5_index.c`, `sqlite3Fts5IndexOptimize` (merges
  all segments into one).

---

## 5. R-Tree — mechanism only

[`ext/rtree/rtree.c`](https://github.com/sqlite/sqlite/blob/master/ext/rtree/rtree.c).
An R-tree stored in a shadow table:

```
%_node    : each row is a BLOB holding one R-tree node: up to N entries of
            (rowid/child, bounding box) — the same slotted idea as btree.c, at
            a different scale
%_rowid   : rowid → leaf node
%_parent  : node → parent node

xBestIndex reports low cost when it sees bounding-box constraints (<=, >=) on the
coordinate columns, and encodes them into idxStr as a string of opcodes;
xFilter walks the tree pruning subtrees whose bounding box cannot match.
Insertion splits overfull nodes with a linear/quadratic split heuristic — the
same problem balance_nonroot() solves, with a different cost function (minimize
bounding-box overlap instead of byte balance).
```

Reading R-tree after b-tree is a good exercise: the same concepts (nodes, splits, parent
pointers, cursors) in an unfamiliar shape, which is how you find out whether Phase 3 stuck.

---

## 6. The extension entry point, exactly

```c
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1                       /* defines a static API pointer */

...your code, calling sqlite3_xxx() normally...

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_myseries_init(sqlite3 *db, char **pzErrMsg,
                          const sqlite3_api_routines *pApi){
  SQLITE_EXTENSION_INIT2(pApi);              /* rewires every sqlite3_ call */
  return sqlite3_create_module(db, "myseries", &seriesModule, 0);
}
```

What `SQLITE_EXTENSION_INIT1/2` actually do: in an extension build, `sqlite3ext.h`
`#define`s every public API name to a field of a function-pointer struct
(`sqlite3_create_module` → `sqlite3_api->create_module`). `INIT1` declares the static
pointer; `INIT2` assigns it from the host. That is why a loadable extension links against
*nothing* — it calls back into the host binary through that table, so one `.dylib` works
with any compatible SQLite.

Consequence to remember: **you cannot call a SQLite API before `SQLITE_EXTENSION_INIT2`
runs** — the pointer is NULL. Crashes in extension init are usually this.

---

## 7. Test code: read `tester.tcl` once, then test files are trivial

The public suite is TCL. You do not need to learn TCL; you need four procs:

```tcl
do_execsql_test  1.1 { SELECT 1+1; }            {2}
do_catchsql_test 1.2 { SELECT nosuch(); }       {1 {no such function: nosuch}}
do_test          1.3 { expr {2*2} }             {4}
ifcapable fts5 { ... }                          ;# skip unless compiled in
reset_db                                        ;# fresh database
```

Result-matching patterns:
```tcl
{/SCAN/}     the result must MATCH this regex
{~/SCAN/}    the result must NOT match this regex
```
Those two are how you assert on query plans — the technique for Phase 8 Task 4.

Corruption tests use two more tools worth knowing:
```tcl
hexio_write $file $offset $hexdata      ;# poke bytes into a database file
sqlite3_test_control SQLITE_TESTCTRL_IMPOSTER db main 1 $rootpage
                                        ;# declare a fake schema over a real root page,
                                        ;# so the test can write deliberately invalid content
```

```sh
grep -n "proc do_execsql_test" ~/Desktop/sqllite/sqlite-src/test/tester.tcl
sed -n '/proc do_execsql_test/,/^}/p' ~/Desktop/sqllite/sqlite-src/test/tester.tcl
```

---

## 8. Exercises against real source

Answer in `labs/phase08/source-questions.md` with `file` + function citations:

1. In `statBestIndex()`, explain the `usable==0 → SQLITE_CONSTRAINT` branch. Write a join
   that triggers it, and show the plan SQLite chooses instead.
2. Why does dbstat set `omit=1` for `schema` but not for `name`? What would break if it set
   `omit=1` for `name`?
3. Quote dbstat's `orderByConsumed` condition. Add one more `ORDER BY` shape it could
   legally accept, and one it must not.
4. Diff `ext/misc/series.c` against your `seriesvtab.c`. List every constraint the official
   version handles that yours does not, and add one of them.
5. In `carray.c`, find `sqlite3_value_pointer()` and explain why the type string makes
   pointer passing safe from SQL injection.
6. Create an FTS5 table, list its shadow tables, and use your Phase 1 `dbparse` to dump
   `docs_data`. Identify one doclist BLOB and explain its structure from `fts5_index.c`.
7. In `rtree.c`, find the node-split function. Compare its goal with `balance_nonroot()`'s
   goal in one paragraph.
8. Find `SQLITE_TESTCTRL_IMPOSTER` in `src/test1.c` (or `main.c`) and describe what a test
   can do with it that ordinary SQL cannot.
9. Write a TCL test that asserts a query uses a covering index (`{~/SCAN/}` plus a
   `{/COVERING INDEX/}` check) and run it under `testfixture`.
