# Phase 6 — The Query Planner (NGQP)

The planner's job: given a `Select` tree and the available indexes, pick **which index to
use for each table, in which order to nest the loops, and whether a sort is needed** — then
hand `wherecode.c` a plan to turn into bytecode.

SQLite's planner is small (~10k lines across `where.c`, `whereexpr.c`, `wherecode.c`) and
entirely cost-based since 3.8.0 ("NGQP" — Next Generation Query Planner).

---

## 6.1 The pipeline

```
sqlite3WhereBegin(pParse, pTabList, pWhere, pOrderBy, ...)
   │
   ├─ 1. ANALYSIS      whereexpr.c
   │       split the WHERE clause on AND into WhereTerm[]
   │       derive VIRTUAL terms (transitive closure, LIKE→range, IN→OR, BETWEEN→2 terms)
   │       compute prereq bitmasks: which tables each term depends on
   │
   ├─ 2. LOOP GENERATION   whereLoopAddAll()
   │       for each table, enumerate candidate WhereLoops:
   │         - full scan
   │         - each usable index, with 0..N equality columns + optional range
   │         - skip-scan variants
   │         - automatic (transient) index
   │         - multi-index OR
   │         - virtual-table plans via xBestIndex
   │       each candidate gets (rSetup, rRun, nOut) costs; dominated ones are discarded
   │
   ├─ 3. PATH SOLVING      wherePathSolver()
   │       dynamic programming over subsets of tables ("N nearest neighbours"):
   │       build the best join orders, keeping up to ~10 candidate paths per step
   │       track whether each path already produces the required ORDER BY
   │
   ├─ 4. CODE GENERATION   wherecode.c
   │       for the winning WherePath, emit OP_Open*, seeks, loop starts;
   │       sqlite3WhereEnd() emits the OP_Next chain, innermost first
   │
   └─ returns WhereInfo* to select.c, which fills the loop body
```

---

## 6.2 LogEst: the unit of everything

All costs and row counts are **LogEst**: a 16-bit fixed-point base-2 logarithm.

```
   LogEst(x) = round(10 * log2(x))

   x        LogEst        because
   1          0           10*log2(1)   = 0
   2         10           10*log2(2)   = 10
   10        33           10*log2(10)  = 33.2
   100       66
   1000      99
   1,000,000 199
```

Two rules that make the whole cost model trivial arithmetic:

```
   multiply values  →  ADD LogEsts        (cost of 100 rows × 10 lookups = 66+33 = 99)
   divide values    →  SUBTRACT LogEsts   (1M rows / 10 per key       = 199-33 = 166)
```

That is why the planner is fast: no floating point, no multiplication, just `i16` adds.

`sqlite3LogEst(u64)`, `sqlite3LogEstToInt(LogEst)`, `sqlite3LogEstAdd(a,b)` (which computes
LogEst(x+y), used for summing costs) live in `util.c`.

**Default estimates when there is no `ANALYZE` data:**
```
table row count           LogEst 200  ≈ 1,048,576 rows
one equality on an index  ≈ 10 rows per distinct key (LogEst 33)
range constraint (x>?)    ≈ 1/4 of rows       (LogEst -20)
two-sided range           ≈ 1/64 of rows      (LogEst -60)
unique index equality     exactly 1 row       (LogEst 0)
full table scan cost      rows + ~16 (a ~3× penalty relative to an index probe)
```
These constants explain most "why did it choose that plan?" surprises on un-analyzed
databases.

---

## 6.3 The data structures

```c
struct WhereTerm {            /* one AND-connected term of the WHERE clause */
  Expr *pExpr;
  int iParent;                /* index of the parent term, for derived terms */
  int leftCursor;             /* cursor of the left-hand column */
  union { int leftColumn; ... } u;
  u16 eOperator;              /* WO_EQ, WO_LT, WO_LE, WO_GT, WO_GE, WO_IN, WO_ISNULL,
                                 WO_IS, WO_AUX (vtab), WO_SINGLE, WO_ALL */
  u16 wtFlags;                /* TERM_DYNAMIC, TERM_VIRTUAL, TERM_CODED, TERM_COPIED,
                                 TERM_LIKE, TERM_VNULL, TERM_SLICE, TERM_IS ... */
  u8 nChild;
  LogEst truthProb;           /* probability this term is true (likelihood()) */
  Bitmask prereqRight;        /* tables the RHS depends on */
  Bitmask prereqAll;          /* all tables this term depends on */
};

struct WhereLoop {            /* one way to scan one table */
  Bitmask prereq;             /* tables that must come BEFORE this loop */
  Bitmask maskSelf;           /* bitmask of this loop's table */
  LogEst rSetup;              /* one-time setup cost (e.g. building an automatic index) */
  LogEst rRun;                /* cost per iteration of the outer loops */
  LogEst nOut;                /* estimated rows output per outer row */
  u32 wsFlags;                /* WHERE_* flags describing the strategy */
  union {
    struct {                  /* b-tree loop */
      u16 nEq;                /* # of equality constraints used */
      u16 nBtm, nTop;         /* range constraint sizes */
      u16 nDistinctCol;
      Index *pIndex;          /* the index used, or NULL for a table scan */
    } btree;
    struct { int idxNum; ... } vtab;   /* virtual table plan from xBestIndex */
  } u;
  u16 nLTerm;
  WhereTerm **aLTerm;         /* the terms this loop uses */
  ...
};

struct WherePath {            /* a candidate join order */
  Bitmask maskLoop;           /* tables included so far */
  Bitmask revLoop;            /* loops that must run in reverse */
  LogEst nRow;                /* rows produced */
  LogEst rCost;               /* total cost INCLUDING sorting */
  LogEst rUnsorted;           /* total cost excluding sorting */
  i8 isOrdered;               /* # of ORDER BY terms already satisfied (-1 = none) */
  WhereLoop **aLoop;          /* the loops, in order */
};

struct WhereMaskSet {         /* cursor number → bit position */
  int n; int ix[BMS];         /* BMS = 64 ⇒ the 64-table join limit */
};
```

**Key `wsFlags`:**
```
WHERE_COLUMN_EQ     one or more == constraints on index columns
WHERE_COLUMN_RANGE  a range constraint
WHERE_COLUMN_IN     an IN constraint (loop over values)
WHERE_COLUMN_NULL   IS NULL constraint
WHERE_IPK           the "index" is the rowid (INTEGER PRIMARY KEY)
WHERE_INDEXED       uses a real index
WHERE_IDX_ONLY      covering index — no table access at all
WHERE_ONEROW        provably at most one row
WHERE_MULTI_OR      OR-clause handled with multiple index scans + a RowSet union
WHERE_AUTO_INDEX    a transient index is built for this loop
WHERE_SKIPSCAN      skip-scan: use a low-cardinality leading index column by enumerating it
WHERE_PARTIALIDX    a partial index whose WHERE clause is implied by the query
WHERE_BLOOMFILTER   a Bloom filter pre-test is used
WHERE_VIRTUALTABLE  plan produced by xBestIndex
```

---

## 6.4 Stage 1: WHERE-clause analysis (`whereexpr.c`)

`sqlite3WhereSplit()` splits on `AND` into terms. Then `exprAnalyze()` on each term does
a surprising amount of work:

| Input | Derived VIRTUAL terms |
|---|---|
| `a BETWEEN 1 AND 9` | `a>=1` and `a<=9` (two usable range terms) |
| `x LIKE 'abc%'` | `x>='abc' AND x<'abd'` — *only if* the collation and case-sensitivity allow |
| `a IN (1,2,3)` | a `WO_IN` term usable by an index |
| `a=b AND b=c` | `a=c` — **transitive closure**, so an index on `a` can be used with the `c` constraint |
| `a=5` where `a` is a column | constant propagation into other terms (`SF_Converted`) |
| `(a=1 AND b=2) OR (a=3)` | analyzed for the multi-index OR optimization |
| `x IS NULL` | usable by an index (unlike most SQL engines) |

`prereqRight`/`prereqAll` bitmasks say which tables a term needs. A term can only be used
by a loop if all its prerequisite tables are already in the outer loops — that's how the
planner knows `o.uid = u.id` is usable when `users` is outer but not when `orders` is.

`truthProb` comes from `likelihood(X, p)`, `likely(X)`, `unlikely(X)` — the only way to
hand the planner selectivity information directly.

---

## 6.5 Stage 2: enumerating loops (`whereLoopAddBtree`)

For each table, and for each index on it, the planner tries every useful prefix:

```
   Index i_abc ON t(a,b,c), query: WHERE a=? AND b>? ORDER BY c

   candidate loops for table t:
     1. full table scan                          rRun = nRow+16, nOut = nRow
     2. i_abc with nEq=0                         (scan the index; useful if covering)
     3. i_abc with nEq=1 (a=?)                   nOut = nRow - 33
     4. i_abc with nEq=1 + range on b            nOut = nRow - 33 - 20
     5. i_abc skip-scan (if a has few distinct values)
     6. automatic index (if the table is used repeatedly in an inner loop)
```

Each candidate's cost:
```
  rRun ≈ (cost to seek)  +  (nOut rows × cost per row)
         + (if not covering) nOut × (cost of one table lookup)
  nOut = estimated rows produced
  rSetup = 0, except for automatic indexes (cost of building them)
```

**The covering-index test** uses `SrcItem.colUsed` from Phase 4: if every referenced column
is in the index, `WHERE_IDX_ONLY` is set and the per-row table lookup disappears from the
cost — often a 2× difference.

Dominated loops are pruned (`whereLoopInsert` keeps a loop only if no existing loop is
better on all of prereq/cost/nOut).

---

## 6.6 Stage 3: join ordering (`wherePathSolver`)

Choosing a join order is `O(n!)` if done naively. SQLite uses **"N nearest neighbours"**
dynamic programming:

```
  Let mxChoice = (nLoop<=1) ? 1 : (nLoop==2 ? 5 : computeMxChoice(pWInfo))
  /* TUNING comment in where.c:  nLoop 1 -> 1,  nLoop 2 -> 5,  nLoop 3+ -> 12 or 18 */

  paths = [ empty path ]
  repeat nLoop times:
      newPaths = []
      for each path p in paths:
          for each loop L not yet in p, whose prereqs are satisfied by p:
              extend p with L, computing:
                  rUnsorted = p.rUnsorted + L.rRun + L.rSetup(if first use)
                  nRow      = p.nRow + L.nOut                 (LogEst add = multiply)
                  isOrdered = how many ORDER BY terms are still satisfied
                  rCost     = rUnsorted + (sort cost if not ordered)
              keep the best mxChoice paths, but keep ORDERED and UNORDERED
              variants separately (an ordered path may be more expensive now
              but avoid a sort later)
      paths = newPaths
  pick the cheapest complete path
```

Two subtleties that make this work in practice:

1. **Ordered and unordered paths are tracked separately.** Otherwise a slightly cheaper
   unsorted path would evict the ordered path that avoids a full sort.
2. **The solver is heuristic.** It can miss the optimal plan for pathological joins — that
   is the documented trade-off of NGQP versus exhaustive search. `wherePathSolver` will
   retry with `mxChoice` raised if the first pass fails to find any plan.

Sort cost: if the path is not ordered, add `nRow + estimated sort cost` — see
`whereSortingCost()`, which also accounts for whether a `LIMIT` reduces the sort.

---

## 6.7 ANALYZE and the statistics tables

Without statistics, the planner uses the defaults from §6.2. `ANALYZE` populates:

### `sqlite_stat1`
```sql
CREATE TABLE sqlite_stat1(tbl TEXT, idx TEXT, stat TEXT);
-- stat = "<rows-in-table> <avg rows per distinct value of col1> <... of col1,col2> ..."
```
Example: `users i_age  "1000000 10"` means 1M rows, and each distinct `age` matches ~10
rows. Loaded into `Index.aiRowLogEst[]` by `analyze.c`.

Extra tokens in `stat`: `unordered` (tells the planner this index is not useful for
ORDER BY), `sz=NNN` (average row size), `noskipscan`.

### `sqlite_stat4` (needs `SQLITE_ENABLE_STAT4`)
Stores up to ~24 sample keys per index, with `nEq`, `nLt`, `nDLt` counts. This gives the
planner **value-specific** estimates: it can tell that `WHERE status='pending'` matches 12
rows while `WHERE status='done'` matches 8 million. Without stat4, both are estimated the
same.

```
  stat4 row:  tbl, idx, neq, nlt, ndlt, sample
              e.g.  orders, i_status, "8000000", "0", "0", <'done'>
                    orders, i_status, "12",      "8000000", "1", <'pending'>
```

Practical guidance:
- Run `ANALYZE` after bulk loads.
- `PRAGMA optimize;` before closing a long-lived connection runs `ANALYZE` only on tables
  whose statistics look stale — the recommended production pattern.
- Statistics are read at schema load; changing them requires `ANALYZE` + reconnect or
  `PRAGMA analysis_limit`-driven refresh.

---

## 6.8 The named optimizations (from `optoverview.html`)

| Optimization | What it does | How to see it |
|---|---|---|
| **Index usage** | equality then range on index prefix columns | `USING INDEX` in EQP |
| **Covering index** | never touch the table | no table cursor in `EXPLAIN` |
| **ORDER BY via index** | skip the sorter | no `Sorter*` opcodes |
| **DISTINCT via index** | skip the dedup ephemeral table | no `OpenEphemeral` |
| **min()/max()** | `OP_Last`/`OP_Rewind` + one Column | tiny program |
| **count(\*)** | `OP_Count` over the smallest b-tree | `USING COVERING INDEX` |
| **Multi-index OR** | run one index scan per OR branch, union the rowids in a RowSet | `MULTI-INDEX OR` in EQP |
| **Automatic index** | build a transient index for a repeatedly scanned inner table | `AUTOMATIC COVERING INDEX` in EQP |
| **Skip-scan** | use an index whose leading column is not constrained by enumerating its few distinct values | `USING INDEX ... (ANY(a) AND b=?)` |
| **Partial index** | use `CREATE INDEX ... WHERE cond` when the query implies `cond` | `USING PARTIAL INDEX` |
| **Query flattening** | merge a subquery into the outer query (`flattenSubquery`) | subquery disappears from EQP |
| **WHERE push-down** | push outer WHERE terms into a subquery/view that cannot be flattened | fewer rows materialized |
| **Transitive closure** | `a=b AND b=5` ⇒ `a=5` | an index on `a` becomes usable |
| **Constant propagation** | replace a column with a known constant | visible in `.treetrace` |
| **LEFT JOIN elimination** | drop a LEFT JOIN whose columns are unused and whose join is provably 1:1 (unique index) | table missing from EQP |
| **Subquery co-routines** | stream a FROM-clause subquery instead of materializing | `CO-ROUTINE` in EQP |
| **Bloom filter (3.38+)** | pre-filter the outer loop of a star join | `BLOOM FILTER` in EQP |
| **`OR` → `IN`** | `a=1 OR a=2` ⇒ `a IN (1,2)` | one index scan |
| **LIKE → range** | `x LIKE 'abc%'` ⇒ `x>='abc' AND x<'abd'` | index range scan |
| **Push LIMIT into the sorter** | keep only the top-N rows while sorting | smaller sorter memory |
| **Omit unused columns / index-only sorter refs** | `SQLITE_ENABLE_SORTER_REFERENCES` | fewer bytes sorted |

### `flattenSubquery()` — the famous 20+ conditions

Flattening `SELECT ... FROM (SELECT ...)` into one query is only legal in specific cases.
The conditions in `select.c` include (abridged): the subquery has no aggregate unless the
outer has none, no LIMIT if the outer has a WHERE, not `DISTINCT` on both sides, no
compound subquery in certain positions, not the right operand of a LEFT JOIN if it can
produce NULL rows, no window functions, etc. Each condition is numbered in the source with
a comment explaining a counterexample. **Reading that comment block is one of the best SQL
semantics lessons available anywhere.**

---

## 6.9 Reading `EXPLAIN QUERY PLAN`

```
sqlite> EXPLAIN QUERY PLAN
   ...> SELECT u.name, o.total FROM users u JOIN orders o ON o.uid=u.id WHERE u.age>40;
QUERY PLAN
|--SEARCH u USING INDEX i_age (age>?)
`--SEARCH o USING INDEX i_orders_uid (uid=?)
```

Vocabulary:
```
SCAN t                       full scan of t — no index constrains the loop
SEARCH t USING INDEX x (a=?) index lookup with an equality constraint
SEARCH t USING INTEGER PRIMARY KEY (rowid=?)   direct rowid lookup
SEARCH t USING COVERING INDEX x (...)          index-only; table never read
USE TEMP B-TREE FOR ORDER BY  a sort is happening
USE TEMP B-TREE FOR GROUP BY  grouping requires a sort
USE TEMP B-TREE FOR DISTINCT
MULTI-INDEX OR               OR handled by several index scans
AUTOMATIC COVERING INDEX     a transient index was built at runtime ← usually a missing-index smell
CO-ROUTINE subquery          streamed, not materialized
MATERIALIZE subquery         materialized into an ephemeral table
BLOOM FILTER ON t (...)      Bloom pre-filter
SCAN t USING INDEX x         index scan with no constraint (usually for ORDER BY or covering)
```

**Loop order in EQP output is outer-to-inner.** The first line is the outermost loop.

Red flags, in rough priority order:
1. `SCAN` on a large table inside a join (missing index)
2. `AUTOMATIC COVERING INDEX` (SQLite built the index you forgot)
3. `USE TEMP B-TREE FOR ORDER BY` on a large result
4. A `SEARCH` on the *wrong* index — check with `ANALYZE`

---

## 6.10 Controlling the planner

| Tool | Effect |
|---|---|
| `ANALYZE` / `PRAGMA optimize` | give it real statistics (the right answer 90% of the time) |
| `INDEXED BY x` | force an index; the statement **fails** if it can't be used |
| `NOT INDEXED` | forbid all indexes on that table |
| `+column` in an expression | defeat index use on that term (`WHERE +a=1`) |
| `likelihood(expr, 0.01)`, `unlikely(expr)` | supply selectivity |
| `CROSS JOIN` | **force** join order: the left table is always outer |
| `PRAGMA automatic_index=OFF` | disable transient indexes |
| `sqlite3_test_control(SQLITE_TESTCTRL_OPTIMIZATIONS, db, mask)` | disable individual optimizations — the debugging tool |
| `PRAGMA query_only` | prevent writes while experimenting |

`CROSS JOIN` as an ordering hint is SQLite-specific and genuinely useful when you know
better than the planner. It changes no semantics, only the loop nesting.

---

## 6.11 The `.wheretrace` window into the solver

In a `SQLITE_ENABLE_WHERETRACE` build:

```
sqlite> .wheretrace 0xfff
sqlite> SELECT u.name FROM users u JOIN orders o ON o.uid=u.id WHERE u.age>40;
```

You get, literally, the planner thinking:

```
---- begin solver.  (nRowEst=0, nQueryLoop=0)
 cost=  0 nrow= 0 order=
 ...
WhereLoop 0.1  users (age>?)  ... rSetup=0 rRun=44 nOut=33
WhereLoop 1.2  orders (uid=?) ... rSetup=0 rRun=39 nOut=10
---- after round 1 ----
 0: cost=44 nrow=33 order= {users}
---- after round 2 ----
 0: cost=83 nrow=43 order= {users, orders}
 1: cost=91 nrow=43 order= {orders, users}
---- Solution nRow=43
```

Every cost in that output is a LogEst — divide by 10 and raise 2 to that power to get real
numbers. Learning to read `.wheretrace` is the single most valuable planner skill; it turns
"why did it do that?" into a readable arithmetic argument.
