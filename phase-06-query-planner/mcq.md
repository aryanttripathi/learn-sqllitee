# Phase 6 — MCQ (25 questions)

1. LogEst(x) is defined as:
   a) log10(x)  b) 10*log2(x) rounded  c) x/10  d) 2^x

2. LogEst 33 corresponds to approximately:
   a) 3 rows  b) 10 rows  c) 33 rows  d) 100 rows

3. Multiplying two row counts corresponds to:
   a) multiplying LogEsts  b) adding LogEsts  c) taking the max  d) XOR

4. Without ANALYZE, SQLite assumes a table has:
   a) 100 rows  b) ~1,048,576 rows (LogEst 200)  c) 0 rows  d) the file size in rows

5. Without statistics, an equality constraint on a non-unique index column is assumed to match:
   a) 1 row  b) ~10 rows  c) half the table  d) all rows

6. `WhereTerm.prereqRight` records:
   a) the index used  b) which tables the right-hand side depends on
   c) the cost  d) the collation

7. `a BETWEEN 1 AND 9` is converted into:
   a) an IN term  b) two virtual range terms `a>=1` and `a<=9`  c) a function call  d) nothing

8. Transitive closure means:
   a) closing cursors  b) deriving `a=c` from `a=b AND b=c` so more indexes become usable
   c) a join type  d) a lock upgrade

9. `x LIKE 'abc%'` can be converted into a range scan only if:
   a) always  b) the prefix is literal and the collation/case-sensitivity permit
   c) the column is indexed  d) `x` is TEXT

10. A `WhereLoop` records:
    a) one way to scan one table, with rSetup/rRun/nOut  b) the join order
    c) the bytecode  d) a cursor

11. `WHERE_IDX_ONLY` means:
    a) index is unique  b) covering index — the table is never read
    c) index-only table  d) only one index exists

12. `wherePathSolver` is:
    a) exhaustive search  b) N-nearest-neighbour dynamic programming keeping ≤10 paths
    c) greedy only  d) a genetic algorithm

13. Ordered and unordered paths are kept separately because:
    a) memory  b) an ordered path may cost more now but avoid a sort later
    c) they use different indexes  d) ORDER BY is always last

14. `sqlite_stat1.stat = "200000 400"` means:
    a) 200000 pages, 400 indexes  b) 200000 rows, each distinct key matches ~400 rows
    c) 400 rows sampled  d) cost 200000

15. `sqlite_stat4` improves on stat1 because it:
    a) is smaller  b) stores per-value samples, capturing skew
    c) is faster to compute  d) stores page counts

16. STAT4 is:
    a) on by default  b) a compile-time option (`SQLITE_ENABLE_STAT4`), often absent
    c) a pragma  d) deprecated

17. `AUTOMATIC COVERING INDEX` in a query plan usually indicates:
    a) an SQLite bug  b) a missing permanent index
    c) a corrupt database  d) the query is optimal

18. Skip-scan is useful when:
    a) the index leading column has few distinct values and is unconstrained
    b) the table is small  c) there is no index  d) ORDER BY is present

19. `INDEXED BY x`:
    a) is a hint  b) is a hard requirement — the statement errors if x cannot be used
    c) disables the planner  d) creates the index

20. `CROSS JOIN` in SQLite:
    a) computes a cartesian product only  b) forces the left table to be the outer loop
    c) is a syntax error with ON  d) disables indexes

21. `WHERE +k = 5`:
    a) is invalid  b) defeats index use on `k`  c) casts to integer  d) is faster

22. An index on `(a,b)` can satisfy:
    a) `WHERE b=?` directly  b) `WHERE a=? ORDER BY b`  c) `ORDER BY b` alone  d) nothing

23. Terms from a LEFT JOIN's ON clause:
    a) may be moved to WHERE freely  b) are marked `EP_OuterON` and must not constrain the left table
    c) are ignored  d) always use an index

24. `PRAGMA optimize`:
    a) rebuilds the database  b) runs ANALYZE selectively where statistics look stale
    c) vacuums  d) rewrites queries

25. `sqlite3_test_control(SQLITE_TESTCTRL_OPTIMIZATIONS, db, mask)`:
    a) enables optimizations  b) disables individual optimizations for testing/measurement
    c) sets the cache size  d) changes the journal mode

---

## Answers

1. **b** — a 16-bit fixed-point base-2 log; makes the cost model integer addition.
2. **b** — 10*log2(10) ≈ 33.2.
3. **b** — that is the whole point of LogEst.
4. **b**.
5. **b** — hence the classic "un-analyzed database picks the equality index" behaviour.
6. **b** — it determines when a term can be used given the loops already chosen.
7. **b**.
8. **b**.
9. **b**.
10. **a**.
11. **b**.
12. **b** — heuristic, bounded, and fast; documented to occasionally miss the optimum.
13. **b**.
14. **b**.
15. **b**.
16. **b** — check `PRAGMA compile_options`.
17. **b**.
18. **a**.
19. **b** — never use it as a soft hint in production code.
20. **b** — ordering hint; semantics unchanged.
21. **b** — the unary `+` makes the expression non-indexable.
22. **b** — prefix rule; `WHERE b=?` needs skip-scan or another index.
23. **b** — mishandling this yields wrong answers, not slow ones.
24. **b**.
25. **b** — 1 bit = disable that optimization.
