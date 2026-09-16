# Phase 6 — Internals Reference: The Planner

## Files
| File | Contents |
|---|---|
| `where.c` | `sqlite3WhereBegin/End`, `whereLoopAddAll`, `whereLoopAddBtree`, `whereLoopAddVirtual`, `whereLoopAddOr`, `wherePathSolver`, cost functions |
| `whereexpr.c` | `sqlite3WhereSplit`, `exprAnalyze`, derived/virtual terms, LIKE and transitive optimizations |
| `wherecode.c` | `sqlite3WhereCodeOneLoopStart` — emits bytecode for one loop |
| `whereInt.h` | `WhereInfo`, `WhereLoop`, `WherePath`, `WhereTerm`, `WhereClause`, `WhereMaskSet`, `WhereLevel` |
| `analyze.c` | `ANALYZE` implementation, `sqlite3_stat1/stat4` loading, `sqlite3DefaultRowEst` |
| `select.c` | `flattenSubquery`, `pushDownWhereTerms`, `sqlite3Select` optimizations |
| `util.c` | `sqlite3LogEst`, `sqlite3LogEstAdd`, `sqlite3LogEstToInt`, `sqlite3LogEstFromDouble` |
| `vtab.c` | virtual-table planning glue (`xBestIndex`) |

## LogEst quick table
```
value :      1    2    3    4    5   10   20   50  100  1e3  1e4  1e5  1e6  1e9
LogEst:      0   10   16   20   23   33   43   56   66   99  132  166  199  299

rules:  LogEst(x*y) = LogEst(x) + LogEst(y)
        LogEst(x/y) = LogEst(x) - LogEst(y)
        +10 ⇒ ×2      +33 ⇒ ×10      -33 ⇒ ÷10
```

## Default estimates (no ANALYZE) — `sqlite3DefaultRowEst()`
```
Table rows                    LogEst 200  (≈1,048,576)
Index aiRowLogEst[0]          = table rows
aiRowLogEst[1..]              33, 32, 30, 28, 26, ... (each extra equality column
                                divides the row count by roughly 10, then less)
UNIQUE index, all columns eq  0 (exactly one row)
Range constraint (one-sided)  nOut - 20   (÷4)
Range constraint (two-sided)  nOut - 60   (÷64)
IS NULL                       treated like equality
Full table scan rRun          rSize + 16
```
Those `+16` / `-20` / `-60` constants are worth remembering: they are the entire
un-analyzed cost model.

## Key functions, in call order
```
sqlite3WhereBegin()
 ├ whereClauseInit / sqlite3WhereSplit / exprAnalyzeAll   (whereexpr.c)
 ├ whereLoopAddAll()
 │   ├ whereLoopAddBtree()      real tables
 │   │    ├ whereLoopAddBtreeIndex()   one candidate per index prefix
 │   │    ├ whereLoopResize/whereLoopInsert  (dominance pruning)
 │   │    └ constructAutomaticIndex()  transient index decision
 │   ├ whereLoopAddVirtual()    calls the vtab's xBestIndex
 │   └ whereLoopAddOr()         multi-index OR
 ├ wherePathSolver()            the N-nearest-neighbour DP
 ├ whereShortCut()              fast path: single table, unique index ⇒ skip the solver
 └ sqlite3WhereCodeOneLoopStart()  ×nLoop     (wherecode.c)
sqlite3WhereEnd()               emits OP_Next / OP_VNext / closes cursors
```

`whereShortCut()` is worth knowing: for `SELECT ... WHERE rowid=?` or a full unique-index
equality, the planner skips the whole solver. Most OLTP statements take this path.

## `WhereLevel` — the runtime side of a loop
```c
struct WhereLevel {
  int iLeftJoin;      /* register holding "did we match a row?" for LEFT JOIN */
  int iTabCur, iIdxCur;
  int addrBrk, addrNxt, addrCont, addrFirst, addrBody;
  u32 iLikeRepCntr;
  WhereLoop *pWLoop;
  Bitmask notReady;
  union { struct { int nIn; InLoop *aInLoop; } in;  Index *pCoveringIdx; } u;
};
```
`addrBrk` / `addrCont` / `addrNxt` are the jump targets that make `break`/`continue`
semantics work across nested loops — read `sqlite3WhereCodeOneLoopStart` with those in mind.

## `eOperator` values (`WhereTerm`)
```
WO_IN 0x0001  WO_EQ 0x0002  WO_LT 0x0004  WO_LE 0x0008  WO_GT 0x0010  WO_GE 0x0020
WO_AUX 0x0040 (vtab-only op)  WO_IS 0x0080  WO_ISNULL 0x0100  WO_OR 0x0200
WO_AND 0x0400 WO_EQUIV 0x0800  WO_NOOP 0x1000   WO_ALL 0x3fff  WO_SINGLE 0x01ff
```

## `wtFlags`
```
TERM_DYNAMIC   pExpr is owned by the WhereClause and must be freed
TERM_VIRTUAL   derived by the optimizer, not in the original SQL
TERM_CODED     already emitted
TERM_COPIED    a copy exists (transitive closure)
TERM_ORINFO / TERM_ANDINFO   extra structures attached
TERM_OR_OK     usable by the OR optimization
TERM_VNULL     a "column IS NOT NULL" term synthesized for a vtab
TERM_LIKEOPT / TERM_LIKECOND / TERM_LIKE  LIKE-optimization bookkeeping
TERM_IS        an IS (not ==) comparison
TERM_SLICE / TERM_HEURTRUTH / TERM_HIGHTRUTH  selectivity bookkeeping
```

## The statistics tables
```sql
CREATE TABLE sqlite_stat1(tbl,idx,stat);
--  stat: "<nRow> <avgEqCol1> <avgEqCol1,2> ..."   plus optional tokens:
--        unordered      -> do not use this index to satisfy ORDER BY
--        sz=NNN         -> average row size in bytes
--        noskipscan     -> never skip-scan this index

CREATE TABLE sqlite_stat4(tbl,idx,neq,nlt,ndlt,sample);   -- SQLITE_ENABLE_STAT4
--  up to ~24 samples per index; for each sample key:
--    neq  = rows equal to the sample, per column prefix
--    nlt  = rows less than the sample
--    ndlt = distinct values less than the sample
```
Loaded by `loadAnalysis()` / `loadStatTbl()` in `analyze.c` into `Index.aiRowLogEst[]` and
`Index.aSample[]`.

`PRAGMA analysis_limit=N` caps how many index rows ANALYZE examines per index (0 = no
limit). `PRAGMA optimize` runs ANALYZE only where it looks stale — the intended production
call, typically before closing a long-lived connection.

## Optimization kill-switches (`SQLITE_TESTCTRL_OPTIMIZATIONS`)
```
SQLITE_QueryFlattener  SQLITE_WindowFunc   SQLITE_GroupByOrder   SQLITE_FactorOutConst
SQLITE_DistinctOpt     SQLITE_CoverIdxScan SQLITE_OrderByIdxJoin SQLITE_Transitive
SQLITE_OmitNoopJoin    SQLITE_CountOfView  SQLITE_CursorHints    SQLITE_Stat4
SQLITE_PushDown        SQLITE_SimplifyJoin SQLITE_SkipScan       SQLITE_PropagateConst
SQLITE_OnePass         SQLITE_OrderBySubq  SQLITE_BloomFilter    SQLITE_BloomPulldown
SQLITE_BalancedMerge   SQLITE_ReleaseReg   SQLITE_FlttnUnionAll  SQLITE_IndexedExpr
```
```c
sqlite3_test_control(SQLITE_TESTCTRL_OPTIMIZATIONS, db, mask);  /* 1 bit = DISABLE */
```
This is the correct way to measure what an optimization is worth, and how SQLite's own
tests verify that each optimization is individually correct.

## EQP vocabulary → what it means internally
```
SCAN t                      WhereLoop with no constraints; full b-tree walk
SEARCH t USING INDEX x (a=?)  WHERE_INDEXED | WHERE_COLUMN_EQ, nEq=1
SEARCH t USING COVERING INDEX WHERE_IDX_ONLY
SEARCH t USING INTEGER PRIMARY KEY (rowid=?)   WHERE_IPK | WHERE_COLUMN_EQ
USE TEMP B-TREE FOR ORDER BY   path.isOrdered < 0, sorter added by generateSortTail()
MULTI-INDEX OR                 WHERE_MULTI_OR — RowSet union of several index scans
AUTOMATIC COVERING INDEX       WHERE_AUTO_INDEX; rSetup > 0
CO-ROUTINE                     subquery implemented with OP_InitCoroutine/OP_Yield
MATERIALIZE                    subquery written to an ephemeral table
BLOOM FILTER ON t (a=?)        WHERE_BLOOMFILTER; OP_FilterAdd/OP_Filter
SCAN t USING INDEX x           index scan chosen for ordering or covering, not filtering
LEFT-JOIN                      WhereLevel.iLeftJoin register in use
```

## Planner control surface
```sql
ANALYZE;  ANALYZE tbl;  PRAGMA optimize;  PRAGMA analysis_limit=400;
SELECT ... FROM t INDEXED BY idx ...       -- hard requirement; errors if unusable
SELECT ... FROM t NOT INDEXED ...
WHERE +col = ?                              -- unary + defeats index use on that term
likelihood(expr, 0.001)  likely(expr)  unlikely(expr)
a CROSS JOIN b                              -- forces a outer, b inner
PRAGMA automatic_index=OFF;
PRAGMA case_sensitive_like=ON;              -- affects LIKE→range optimization
```

## Gotchas

1. **`sqlite_stat1` stores averages.** Skewed columns get bad estimates without STAT4.
   This is the #1 cause of "ANALYZE made it slower".
2. STAT4 is **not** compiled in by default (nor in Apple's system SQLite). Check
   `PRAGMA compile_options`.
3. `INDEXED BY` is a *constraint*, not a hint: the statement errors if the index cannot be
   used. Never use it to "nudge" the planner in application code that must keep working
   after a schema change.
4. `CROSS JOIN` changes only the loop order, never the result.
5. LIKE → range conversion requires: the pattern's prefix is a literal, the column has
   BINARY collation (or `case_sensitive_like=ON` for a NOCASE-capable index), and the
   pattern does not begin with a wildcard.
6. An index on `(a,b)` can serve `WHERE a=? ORDER BY b` but **not** `WHERE b=?` — except
   via skip-scan, and only when `a` has few distinct values.
7. `WHERE a=1 OR b=2` can use two indexes (MULTI-INDEX OR); `WHERE a=1 OR b>2` may not.
8. A `LEFT JOIN`'s ON-clause terms (`EP_OuterON`) cannot be used to constrain the *left*
   table, and cannot be moved into the WHERE clause. Getting this wrong produces
   wrong answers, not slow ones.
9. `AUTOMATIC COVERING INDEX` in a plan means SQLite built an index at runtime because you
   didn't — usually a 100× speedup is available by creating it permanently.
10. The solver is heuristic (`mxChoice` ≤ 10). With many tables it can miss the optimum;
    that is a documented trade-off, not a bug.
11. Cost estimates assume uniform distribution *within* an index prefix even with STAT4
    outside the sampled values.
12. Prepared statements cache the plan. New statistics only affect statements prepared
    afterwards (and `sqlite3_prepare_v2` re-prepares on schema change, which `ANALYZE`
    triggers).

## Canonical reading
`sqlite.org/optoverview.html` (every optimization, with examples),
`sqlite.org/queryplanner.html`, `sqlite.org/queryplanner-ng.html` (the NGQP design paper),
`sqlite.org/eqp.html`, `sqlite.org/lang_analyze.html`, `sqlite.org/np1queryprob.html`.
