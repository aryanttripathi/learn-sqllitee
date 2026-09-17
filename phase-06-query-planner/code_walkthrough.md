# Phase 6 — Code Walkthrough: The Query Planner

Source:
[`src/where.c`](https://github.com/sqlite/sqlite/blob/master/src/where.c) ·
[`src/whereexpr.c`](https://github.com/sqlite/sqlite/blob/master/src/whereexpr.c) ·
[`src/wherecode.c`](https://github.com/sqlite/sqlite/blob/master/src/wherecode.c) ·
[`src/whereInt.h`](https://github.com/sqlite/sqlite/blob/master/src/whereInt.h) ·
[`src/util.c`](https://github.com/sqlite/sqlite/blob/master/src/util.c) (LogEst) ·
design paper: [queryplanner-ng.html](https://sqlite.org/queryplanner-ng.html)

---

## Mental model for this phase

> **The planner is a search over `WhereLoop` objects, scored in LogEst, pruned by
> dominance, assembled by dynamic programming into `WherePath`s.** Four nouns, one verb.

And the reading rule that makes `where.c` tractable:

> **Never read planner code without `.wheretrace` output beside it.** The code computes
> numbers; the trace shows you the numbers. Read them together and every line has meaning;
> read the code alone and it is arithmetic on abbreviations.

Look for `/* TUNING: */` comments. They mark every hand-chosen constant in the cost model
and usually explain the reasoning and the trade-off. They are the planner's design notes.

---

## 1. `sqlite3LogEst()` — the unit everything is measured in

Real code, `src/util.c`:

```c
SQLITE_PRIVATE LogEst sqlite3LogEst(u64 x){
  static LogEst a[] = { 0, 2, 3, 5, 6, 7, 8, 9 };
  LogEst y = 40;
  if( x<8 ){
    if( x<2 ) return 0;
    while( x<8 ){  y -= 10; x <<= 1; }
  }else{
#if GCC_VERSION>=5004000
    int i = 60 - __builtin_clzll(x);
    y += i*10;
    x >>= i;
#else
    while( x>255 ){ y += 40; x >>= 4; }  /*OPTIMIZATION-IF-TRUE*/
    while( x>15 ){  y += 10; x >>= 1; }
#endif
  }
  return a[x&7] + y - 10;
}
```

Line by line:

- The goal is `10*log2(x)` **with no floating point**, because the planner runs on every
  `prepare()` and must be fast and deterministic across platforms.
- `while( x<8 ){ y -= 10; x <<= 1; }` and the `>>=` loops: each doubling is worth exactly 10
  LogEst units. Shifting normalizes `x` into `[8,15]` while accumulating the exponent.
- `__builtin_clzll(x)` — the same normalization in one instruction: count leading zeros
  gives `floor(log2(x))` directly.
- `a[] = { 0, 2, 3, 5, 6, 7, 8, 9 }` is the **mantissa lookup**: the fractional part of
  `log2` for the three low bits. `a[x&7]` interpolates between powers of two.
- `/*OPTIMIZATION-IF-TRUE*/` marks a branch that only affects speed, so coverage tooling
  knows it need not prove both directions matter for correctness.

Check it against the table in `knowledge.md` §6.2: `sqlite3LogEst(10)` → normalizes to
x=10, y=40+... → 33. Exactly the "one equality ≈ 10 rows" constant you have been using.

### `sqlite3LogEstAdd()` — the one operation that is *not* an add

```c
SQLITE_PRIVATE LogEst sqlite3LogEstAdd(LogEst a, LogEst b){
  static const unsigned char x[] = {
     10, 10,                         /* 0,1 */
      9, 9,                          /* 2,3 */
      8, 8,                          /* 4,5 */
      7, 7, 7,                       /* 6,7,8 */
      6, 6, 6,                       /* 9,10,11 */
      5, 5, 5,                       /* 12-14 */
      4, 4, 4, 4,                    /* 15-18 */
      3, 3, 3, 3, 3, 3,              /* 19-24 */
      2, 2, 2, 2, 2, 2, 2,           /* 25-31 */
  };
  if( a>=b ){
    if( a>b+49 ) return a;
    if( a>b+31 ) return a+1;
    return a+x[a-b];
  }else{ ... symmetric ... }
}
```

**Remember the two rules:** adding LogEsts *multiplies* the values. So how do you add two
*costs*? This function: `LogEstAdd(a,b) == LogEst(2^(a/10) + 2^(b/10))`, computed with a
31-entry table.

- `if( a>b+49 ) return a;` — if one term is 32× larger, the other is noise. Free precision
  argument.
- `x[a-b]` interpolates the rest. `LogEstAdd(x,x) == x+10`, i.e. doubling. Check:
  `a-b==0` ⇒ `x[0]==10`. ✔

Every time you see `pNew->rRun = sqlite3LogEstAdd(pNew->rRun, nLookup);` in `where.c`, it
means *"the total cost is the sum of these two real costs"*, not a product.

---

## 2. The cost model, in its own words

These are the actual cost assignments in `whereLoopAddBtree()`. They are short, and they
carry `/* TUNING: */` comments explaining themselves.

### Full table scan

```c
      /* ... At 2.75, a full table scan is preferred over using an index on
      ** a column with just two distinct values where each value has about
      ** an equal number of appearances.  Without STAT4 data, we still want
      ** to use an index in that case, since the constraint might be for
      ** the scarcer of the two values, and in that case an index lookup is
      ** better. */
#ifdef SQLITE_ENABLE_STAT4
      pNew->rRun = rSize + 16 - 2*((pTab->tabFlags & TF_HasStat4)!=0);
#else
      pNew->rRun = rSize + 16;
#endif
      ApplyCostMultiplier(pNew->rRun, pTab->costMult);
      whereLoopOutputAdjust(pWC, pNew, rSize);
```

- `rSize + 16` — `rSize` is `LogEst(rows)`; `+16` multiplies by ~3. So **a full scan costs
  about 3× the row count.** That is the `+16` constant you memorized in
  `knowledge.md` §6.2, and here is where it lives.
- With STAT4 available the penalty drops to `+14` (~2.75×), and the comment explains
  exactly why: with real per-value statistics the planner trusts its index estimates more,
  so it needs less of a thumb on the scale toward scanning.
- `ApplyCostMultiplier(..., pTab->costMult)` — virtual tables and some table types can be
  declared more expensive per row.
- `whereLoopOutputAdjust()` — applies `likelihood()`/`unlikely()` and term selectivity to
  `nOut` *after* the base cost is set.

### Index scan, covering vs. not

```c
        /* The cost of visiting the index rows is N*K, where K is
        ** between 1.1 and 3.0, depending on the relative sizes of the
        ** index and table rows. */
        pNew->rRun = rSize + 1 + (15*pProbe->szIdxRow)/pTab->szTabRow;
        if( m!=0 ){
          /* If this is a non-covering index scan, add in the cost of
          ** doing table lookups.  The cost will be 3x the number of
          ** lookups.  Take into account WHERE clause terms that can be
          ** satisfied using just the index, and that do not require a
          ** table lookup. */
          LogEst nLookup = rSize + 16;  /* Base cost:  N*3 */
          for(ii=0; ii<pWC2->nTerm; ii++){
            WhereTerm *pTerm = &pWC2->a[ii];
            if( !sqlite3ExprCoveredByIndex(pTerm->pExpr, iCur, pProbe) ) break;
            if( pTerm->truthProb<=0 ){
              nLookup += pTerm->truthProb;
            }else{
              nLookup--;
              if( pTerm->eOperator & (WO_EQ|WO_IS) ) nLookup -= 19;
            }
          }
          pNew->rRun = sqlite3LogEstAdd(pNew->rRun, nLookup);
        }
```

This is the **covering-index decision, quantified**:

- Scanning the index costs `rSize + 1 + (15*szIdxRow)/szTabRow` — a narrow index over a wide
  table is cheap to scan (small `szIdxRow`), which is why `Column.szEst` from Phase 4's
  `sqlite3AffinityType()` ends up mattering here.
- `if( m!=0 )` — `m` is the mask of needed columns *not* in the index. **Zero means covering,
  and the entire table-lookup term disappears.** That single branch is the 2× you measured
  in Phase 5 between a covering and non-covering plan.
- `nLookup = rSize + 16` — each table lookup costs ~3, same constant as the scan penalty.
- `nLookup -= 19;` for an equality term satisfiable from the index — 19 LogEst ≈ ÷3.7. The
  planner is crediting terms it can check *before* paying for the row fetch. This is index
  push-down, expressed as a cost adjustment rather than a separate optimization.

**Reading technique:** when a plan surprises you, find the `rRun` assignment for each
candidate, plug in your table's `rSize`, and compare. The whole model is four or five such
lines.

---

## 3. `whereLoopInsert()` — pruning by dominance

[Source](https://github.com/sqlite/sqlite/blob/master/src/where.c) — search for
`static int whereLoopInsert(`. Mechanism:

```
Given a candidate pTemplate, compare against every loop already collected for this table:

  a loop X "dominates" template T when, roughly:
      X.prereq  ⊆ T.prereq        (X needs no more outer tables than T)
      X.rSetup ≤ T.rSetup
      X.rRun   ≤ T.rRun
      X.nOut   ≤ T.nOut
      and X is at least as good on ordering

  if some existing X dominates T   → discard T entirely
  if T dominates some existing X   → replace X with T
  otherwise                        → append T to the list
```

The helper doing the real comparison is `whereLoopCheaperProperSubset()`. The point of all
this is that the number of candidates per table stays small (usually < 10), so the
dynamic-programming step that follows is cheap.

In `.wheretrace` output this is visible as lines that appear and then vanish:
```
WhereLoop 0.1 users (age>?)  ... rRun=44 nOut=33
WhereLoop 0.2 users          ... rRun=51 nOut=44      ← dominated, dropped
```

---

## 4. `wherePathSolver()` — the join-order search

~400 lines. [Source](https://github.com/sqlite/sqlite/blob/master/src/where.c) — search for
`static int wherePathSolver(`. Read the TUNING comment first; it is the whole design:

```c
  /* TUNING: mxChoice is the maximum number of possible paths to preserve
  ** at each step.  Based on the number of loops in the FROM clause:
  **
  **     nLoop      mxChoice
  **     -----      --------
  **       1            1            // the most common case
  **       2            5
  **       3+        12 or 18        // see computeMxChoice()
  */
  if( nLoop<=1 ){
    mxChoice = 1;
  }else if( nLoop==2 ){
    mxChoice = 5;
  }else if( pParse->nErr ){
    mxChoice = 1;
  }else{
    mxChoice = computeMxChoice(pWInfo);
  }
```

Mechanism of the search itself:

```
aFrom = { the empty path }                  /* paths built so far */
for( iLoop = 0; iLoop < nLoop; iLoop++ ){   /* add one table per round */
    for each path pFrom in aFrom:
        for each WhereLoop pWLoop not already in pFrom:
            if( (pWLoop->prereq & ~pFrom->maskLoop)!=0 ) continue;   /* prereqs unmet */
            rUnsorted = pFrom->rUnsorted + pWLoop->rRun + (setup if first use)
            nOut      = pFrom->nRow + pWLoop->nOut        /* LogEst add = multiply */
            isOrdered = wherePathSatisfiesOrderBy(...)    /* still sorted? */
            rCost     = rUnsorted + (isOrdered<0 ? sort cost : 0)
            insert into aTo[], keeping <= mxChoice paths,
              and keeping ORDERED and UNORDERED variants separately
    aFrom = aTo
}
pick the cheapest complete path → pWInfo->a[].pWLoop
```

The three subtleties worth carrying away:

1. **`prereq` masks enforce join legality.** A loop for `orders` constrained by
   `o.uid=u.id` has `users` in its `prereq`, so it can only be added to a path that already
   contains `users`. Join order validity is bitmask arithmetic, not a special case.
2. **Ordered and unordered paths are tracked separately** (`isOrdered>=0` vs `<0`). An
   ordered path may be more expensive now yet win later by avoiding the sorter. Merging
   them into one "best" list would lose that plan permanently.
3. **`rUnsorted` is carried alongside `rCost`** so the sort penalty can be recomputed as the
   path grows, rather than baked in too early.

`whereSortingCost()` computes the penalty, and it accounts for `LIMIT`: sorting to find the
top 10 is cheaper than a full sort, so an `ORDER BY ... LIMIT` query can legitimately prefer
a scan+sort over an index.

---

## 5. `whereexpr.c` — where terms are invented

[Source](https://github.com/sqlite/sqlite/blob/master/src/whereexpr.c). `exprAnalyze()` is
~400 lines; its job is to take one AND-term and derive *additional* usable terms. The
derivations, each with its own helper:

| Input | Derived | Function |
|---|---|---|
| `a BETWEEN x AND y` | `a>=x`, `a<=y` (both `TERM_VIRTUAL`) | `exprAnalyze` BETWEEN branch |
| `x LIKE 'abc%'` | `x>='abc' AND x<'abd'` | `isLikeOrGlob()` |
| `a=b AND b=c` | `a=c` | `exprAnalyze` + `TERM_COPIED`/equivalence classes |
| `(a=1 AND b=2) OR (a=3)` | OR-clause analysis for multi-index OR | `exprAnalyzeOrTerm()` |
| `a IN (...)` | `WO_IN` term usable by an index | `exprAnalyze` IN branch |
| vtab `MATCH`/`LIKE`/`GLOB` | `WO_AUX` terms offered to `xBestIndex` | `isAuxiliaryVtabOperator()` |

`isLikeOrGlob()` is the most instructive one to read in full — it is short, and it shows
exactly how many conditions must hold before a `LIKE` can become a range scan:
the pattern must be a literal (or a bound parameter, handled specially), the prefix must
contain no wildcards, the column's collation must be compatible, and `case_sensitive_like`
must agree. Every one of those conditions is a query you can write that *fails* to use the
index — which is Phase 6 Lab 6.2's `LIKE '%ing'` case.

The bitmask machinery underneath:

```c
pTerm->prereqRight = exprTableUsage(pMaskSet, pExpr->pRight);   /* tables the RHS needs */
pTerm->prereqAll   = exprTableUsage(pMaskSet, pExpr);           /* all tables referenced */
```
`WhereMaskSet` maps cursor numbers → bit positions, and `Bitmask` is a `u64`. That is the
origin of the ~64-table join limit (`BMS`).

---

## 6. `wherecode.c` — from plan to bytecode

`sqlite3WhereCodeOneLoopStart()` is ~1000 lines and generates the loop body for **one**
`WhereLoop`. Do not read it linearly; it is a dispatch on `wsFlags`:

```
if( pLoop->wsFlags & WHERE_VIRTUALTABLE )   → OP_VFilter / OP_VNext
else if( pLoop->wsFlags & WHERE_IPK && nEq==1 )
                                            → OP_SeekRowid / OP_NotExists   (no loop)
else if( pLoop->wsFlags & WHERE_INDEXED )   → build the seek key from nEq terms,
                                              OP_SeekGE/GT/LE/LT,
                                              OP_IdxGT/GE/LT/LE as the loop bound,
                                              OP_Next/OP_Prev
else if( pLoop->wsFlags & WHERE_MULTI_OR )  → one sub-loop per OR branch into a RowSet
else                                        → OP_Rewind / OP_Next (full scan)
```

Read it with `EXPLAIN` output side by side: each branch above produces one of the bytecode
shapes you decoded in Phase 5 §5.3. The `WhereLevel` fields `addrBrk`, `addrCont`,
`addrNxt` are the jump targets that let nested loops implement `break`/`continue`.

This file is also where `OP_DeferredSeek` is emitted (the "don't touch the table until a
column is needed" optimization) — search for `OP_DeferredSeek` to see the exact condition.

---

## 7. `analyze.c` — where the statistics come from

Two functions are worth knowing by name:

- **`sqlite3DefaultRowEst()`** — called when there is no `sqlite_stat1` row. It fills
  `Index.aiRowLogEst[]` with the defaults you memorized (table = 200; first equality ≈ 33,
  then 32, 30, 28, 26; a fully-specified UNIQUE index = 0). Read it once; it is 30 lines
  and it is the entire "no ANALYZE" cost model.
- **`loadStatTbl()` / `loadAnalysis()`** — parse `sqlite_stat1.stat` (`"nRow nEq1 nEq2 ..."`
  plus the `unordered`, `sz=N`, `noskipscan` tokens) and, with STAT4, load `aSample[]`.

```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400
sed -n '/^SQLITE_PRIVATE void sqlite3DefaultRowEst(/,/^}/p' $A/sqlite3.c
grep -n "\"unordered\"\|noskipscan\|sz=" $A/sqlite3.c | head
```

With STAT4, `whereRangeScanEst()` and `whereEqualScanEst()` consult the samples instead of
the averages — those two functions are the difference you measured in Phase 6 Lab 6.1.

---

## 8. Exercises against real source

Answer in `labs/phase06/source-questions.md` with `src/file.c` + function citations:

1. Compute `sqlite3LogEst(1000000)` by hand following the code, and check against
   `.wheretrace` output on a 1M-row table.
2. Show that `sqlite3LogEstAdd(x,x) == x+10` from the table, and explain why that is the
   correct answer.
3. Quote the full-table-scan `rRun` assignment and its TUNING comment. Explain, in terms of
   real multipliers, what changes when STAT4 is compiled in.
4. In the index-scan cost, what is `m`, and which Phase 4 field ultimately determines it?
   Construct two queries on the same index where one is covering and one is not, and
   compare their `rRun` in `.wheretrace`.
5. Explain the `nLookup -= 19;` credit. What does 19 LogEst units mean as a ratio?
6. Find `whereLoopCheaperProperSubset()`. List the exact conditions for dominance.
7. Find `computeMxChoice()`. What makes it return 18 instead of 12?
8. Read `isLikeOrGlob()` and list every condition that must hold for the LIKE→range
   optimization. Write one query that fails each condition.
9. Find `whereSortingCost()`. How does a `LIMIT` reduce the estimated sort cost, and what
   does that imply for `ORDER BY ... LIMIT 10` on a large table?
10. Find `sqlite3DefaultRowEst()` and confirm the five default constants from
    `knowledge.md` §6.2 against the source.
