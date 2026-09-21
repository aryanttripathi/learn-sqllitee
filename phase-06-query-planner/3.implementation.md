# Phase 6 — Implementation Labs: Make the Planner Show Its Work

---

## Lab 6.0 — Build the test database

```sh
mkdir -p ~/Desktop/sqllite/labs/phase06 && cd ~/Desktop/sqllite/labs/phase06
sqlite3 p6.db <<'SQL'
CREATE TABLE a(id INTEGER PRIMARY KEY, k INT, status TEXT, v TEXT);
CREATE TABLE b(id INTEGER PRIMARY KEY, aid INT, amt REAL);
BEGIN;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<200000)
INSERT INTO a SELECT i, i%500,
       CASE WHEN i%20000=0 THEN 'pending' ELSE 'done' END, hex(randomblob(8)) FROM c;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<200000)
INSERT INTO b SELECT i, (i%200000)+1, i*1.5 FROM c;
CREATE INDEX ia_k ON a(k);
CREATE INDEX ia_status ON a(status);
CREATE INDEX ib_aid ON b(aid);
COMMIT;
SQL
```

`a` has 200k rows: `k` has 500 distinct values (400 rows each), `status` is 199,990 `'done'`
and 10 `'pending'` — a deliberately **skewed** column, which is where planners get
interesting.

---

## Lab 6.1 — ⭐ The ANALYZE flip (verified on this machine)

```sh
sqlite3 p6.db 'EXPLAIN QUERY PLAN SELECT count(*) FROM a WHERE status="pending" AND k<10;'
```
**Before ANALYZE:**
```
QUERY PLAN
`--SEARCH a USING INDEX ia_status (status=?)
```
The planner assumes ~10 rows per distinct `status` value (the no-statistics default) and
picks the equality constraint. That happens to be right — `'pending'` really is 10 rows.

```sh
sqlite3 p6.db 'ANALYZE;'
sqlite3 p6.db 'SELECT * FROM sqlite_stat1;'
```
```
b|ib_aid   |200000 1
a|ia_status|200000 100000      ← "each distinct status matches ~100,000 rows"
a|ia_k     |200000 400         ← "each distinct k matches ~400 rows"
```

**After ANALYZE:**
```
QUERY PLAN
`--SEARCH a USING INDEX ia_k (k<?)
```

The plan **got worse for this particular value** — because `sqlite_stat1` only stores an
*average*, and the average `status` matches 100k rows. The planner now believes
`status='pending'` is terrible and `k<10` is better.

**This is the single most instructive experiment in the phase.** Write it up in
`labs/phase06/analyze-flip.md` and then fix it:

```sh
# STAT4 stores per-VALUE samples, which captures skew.
# Apple's system sqlite3 is NOT built with STAT4 — check:
sqlite3 :memory: "SELECT * FROM pragma_compile_options WHERE compile_options LIKE '%STAT%';"

# Build your own with STAT4:
cd ~/Desktop/sqllite/sqlite-amalgamation-3500400
cc -O2 -DSQLITE_ENABLE_STAT4 -DSQLITE_ENABLE_EXPLAIN_COMMENTS \
   -o ~/Desktop/sqllite/labs/phase06/sqlite3-stat4 shell.c sqlite3.c -lpthread -ldl -lm

cd ~/Desktop/sqllite/labs/phase06
cp p6.db p6-stat4.db
./sqlite3-stat4 p6-stat4.db 'ANALYZE;'
./sqlite3-stat4 p6-stat4.db 'SELECT count(*) FROM sqlite_stat4;'
./sqlite3-stat4 p6-stat4.db 'EXPLAIN QUERY PLAN SELECT count(*) FROM a WHERE status="pending" AND k<10;'
./sqlite3-stat4 p6-stat4.db 'EXPLAIN QUERY PLAN SELECT count(*) FROM a WHERE status="done" AND k<10;'
```

With STAT4 the two queries should get **different plans** — the whole point.

Deliverable must answer: what exactly does `sqlite_stat1.stat` mean token by token? What
does STAT4 store per sample (`nEq`, `nLt`, `nDLt`)? Why is STAT4 off by default?

---

## Lab 6.2 — Read a real plan zoo (verified outputs)

```sh
sqlite3 p6.db 'EXPLAIN QUERY PLAN SELECT a.v,b.amt FROM a JOIN b ON b.aid=a.id WHERE a.k=7;'
```
```
QUERY PLAN
|--SEARCH a USING INDEX ia_k (k=?)      ← outer loop
`--SEARCH b USING INDEX ib_aid (aid=?)  ← inner loop, driven by a.id
```

```sh
sqlite3 p6.db 'EXPLAIN QUERY PLAN SELECT * FROM a ORDER BY k LIMIT 10;'
```
```
QUERY PLAN
`--SCAN a USING INDEX ia_k              ← index scan gives ORDER BY for free; no sorter
```

```sh
sqlite3 p6.db 'EXPLAIN QUERY PLAN SELECT * FROM a WHERE v="ZZ";'
```
```
QUERY PLAN
`--SCAN a                                ← no index on v
```

```sh
sqlite3 p6.db 'EXPLAIN QUERY PLAN
  SELECT * FROM a JOIN (SELECT aid, sum(amt) s FROM b GROUP BY aid) x
  ON x.aid=a.id WHERE a.k=3;'
```
```
QUERY PLAN
|--CO-ROUTINE x                                    ← subquery streamed, not materialized
|  `--SCAN b USING INDEX ib_aid
|--SEARCH a USING INDEX ia_k (k=?)
|--BLOOM FILTER ON x (aid=?)                       ← 3.38+ star-query optimization
`--SEARCH x USING AUTOMATIC COVERING INDEX (aid=?) ← transient index built at runtime
```

That last plan contains three advanced features at once. Explain each in
`labs/phase06/plan-zoo.md`, and answer: what would you change in the schema or query to
make the `AUTOMATIC COVERING INDEX` unnecessary?

Now build the rest of the zoo yourself — one EQP capture + explanation each:
```sql
SELECT count(*) FROM a;                                      -- count optimization
SELECT max(k) FROM a;                                        -- min/max optimization
SELECT DISTINCT k FROM a;
SELECT * FROM a WHERE k=1 OR k=2;                            -- OR → IN
SELECT * FROM a WHERE k=1 OR status='pending';               -- MULTI-INDEX OR
SELECT * FROM a WHERE status LIKE 'pen%';                    -- LIKE → range
SELECT * FROM a WHERE status LIKE '%ing';                    -- no range possible
SELECT * FROM a WHERE k IN (1,2,3);
SELECT * FROM a, b WHERE a.id=b.aid AND a.k=5;
SELECT * FROM a CROSS JOIN b ON a.id=b.aid;                  -- forced order
SELECT * FROM a LEFT JOIN b ON b.aid=a.id;
SELECT a.id FROM a LEFT JOIN b ON b.aid=a.id;                -- LEFT JOIN elimination?
SELECT * FROM a ORDER BY v;                                  -- temp b-tree sort
SELECT * FROM a WHERE k>10 ORDER BY k DESC;                  -- reverse index scan
SELECT * FROM a WHERE +k=5;                                  -- index defeated by unary +
SELECT * FROM a INDEXED BY ia_status WHERE k=5;              -- forced, may error
SELECT * FROM a NOT INDEXED WHERE k=5;
```

---

## Lab 6.3 — ⭐ Watch the solver think (`.wheretrace`)

Needs a `SQLITE_ENABLE_WHERETRACE` build (your Phase 0 debug build, or rebuild the
amalgamation with `-DSQLITE_DEBUG -DSQLITE_ENABLE_WHERETRACE`):

```sh
cd ~/Desktop/sqllite/labs/phase06
cc -O0 -g -DSQLITE_DEBUG -DSQLITE_ENABLE_WHERETRACE -DSQLITE_ENABLE_TREETRACE \
   -DSQLITE_ENABLE_EXPLAIN_COMMENTS -DSQLITE_ENABLE_STAT4 \
   -I../../sqlite-amalgamation-3500400 \
   -o sqlite3-trace ../../sqlite-amalgamation-3500400/shell.c \
      ../../sqlite-amalgamation-3500400/sqlite3.c -lpthread -ldl -lm

./sqlite3-trace p6.db
sqlite> .wheretrace 0xfff
sqlite> SELECT a.v,b.amt FROM a JOIN b ON b.aid=a.id WHERE a.k=7 ORDER BY b.amt;
```

You will see, for each candidate:
```
WhereLoop 0.x ... rSetup=..  rRun=..  nOut=..
---- after round N ----
 0: cost=... nrow=... order=...
---- Solution ...
```

**Deliverable `labs/phase06/wheretrace.md`:**
1. List every `WhereLoop` candidate the planner generated for each table, with its costs.
2. Convert three `rRun`/`nOut` LogEst values to real numbers (`2^(x/10)`).
3. Show the paths kept after each round and why the loser lost.
4. Re-run with `ORDER BY` removed and explain the difference in `isOrdered` handling.
5. Re-run after `ANALYZE` and show which costs changed.

---

## Lab 6.4 — LogEst arithmetic by hand

Write `logest.c`:

```c
#include <stdio.h>
#include <math.h>
/* mirror of sqlite3LogEst() */
static short logEst(double x){ return (short)(10.0*log2(x) + 0.5); }
static double fromLogEst(short e){ return pow(2.0, e/10.0); }

int main(void){
  double v[] = {1,2,5,10,100,1000,10000,1000000,1e9};
  int i;
  for(i=0;i<9;i++) printf("%12.0f -> LogEst %4d -> %12.0f\n",
                          v[i], logEst(v[i]), fromLogEst(logEst(v[i])));
  /* the two rules */
  printf("\n100 rows x 10 lookups: %d + %d = %d  => %.0f rows\n",
         logEst(100), logEst(10), logEst(100)+logEst(10),
         fromLogEst(logEst(100)+logEst(10)));
  printf("1e6 rows / 10 per key: %d - %d = %d  => %.0f rows\n",
         logEst(1e6), logEst(10), logEst(1e6)-logEst(10),
         fromLogEst(logEst(1e6)-logEst(10)));
  return 0;
}
```

```sh
cc -o logest logest.c -lm && ./logest
```

Then, in `labs/phase06/logest.md`, hand-compute the cost of these plans for the `p6.db`
statistics and check against `.wheretrace`:
1. full scan of `a`
2. `ia_k` with `k=?`
3. `ia_k` with `k<?`
4. join `a`→`b` vs `b`→`a`

---

## Lab 6.5 — Force bad plans and measure the damage

```sh
cd ~/Desktop/sqllite/labs/phase06
q='SELECT count(*) FROM a JOIN b ON b.aid=a.id WHERE a.k=7'

echo "default:"; time sqlite3 p6.db "$q;"
echo "no index on a:"; time sqlite3 p6.db "SELECT count(*) FROM a NOT INDEXED JOIN b ON b.aid=a.id WHERE a.k=7;"
echo "forced order (b outer):"; time sqlite3 p6.db "SELECT count(*) FROM b CROSS JOIN a ON b.aid=a.id WHERE a.k=7;"
echo "automatic_index off:"; time sqlite3 p6.db "PRAGMA automatic_index=OFF; $q;"
```

Then use the optimization kill-switch (debug builds):
```c
/* disable individual optimizations to see what each is worth */
sqlite3_test_control(SQLITE_TESTCTRL_OPTIMIZATIONS, db, mask);
/* masks include SQLITE_QueryFlattener, SQLITE_GroupByOrder, SQLITE_FactorOutConst,
   SQLITE_DistinctOpt, SQLITE_CoverIdxScan, SQLITE_OrderByIdxJoin, SQLITE_Transitive,
   SQLITE_OmitNoopJoin, SQLITE_CountOfView, SQLITE_SkipScan, SQLITE_PushDown,
   SQLITE_BloomFilter, ... — grep for "SQLITE_QueryFlattener" in sqlite3.c */
```

Write a small C driver that toggles one optimization at a time and times the same query.
Deliverable: `labs/phase06/optimization-value.md` — a table of optimization → speedup on
your workload. This is exactly the kind of measurement a planner contributor makes.

---

## Lab 6.6 — Index design clinic

For each of the following, design the *minimum* set of indexes, verify with EQP that the
plan is optimal (`SEARCH` + covering + no temp b-tree), and measure:

```sql
1. SELECT * FROM a WHERE k=? AND status=?;
2. SELECT v FROM a WHERE k=? ORDER BY status;
3. SELECT status, count(*) FROM a GROUP BY status;
4. SELECT * FROM a WHERE status='pending' ORDER BY k LIMIT 10;
5. SELECT a.v FROM a JOIN b ON b.aid=a.id WHERE b.amt>1000 ORDER BY a.k;
6. SELECT * FROM a WHERE k BETWEEN 10 AND 20 AND status='done';
7. SELECT count(*) FROM a WHERE status='pending';         -- partial index candidate
```

Test a partial index for #7:
```sql
CREATE INDEX ia_pending ON a(k) WHERE status='pending';
EXPLAIN QUERY PLAN SELECT count(*) FROM a WHERE status='pending';
SELECT sum(pgsize) FROM dbstat WHERE name='ia_pending';   -- how small is it?
```

Deliverable: `labs/phase06/index-clinic.md` with, for each query: indexes created, EQP
before/after, timing before/after, and index size cost.

---

## Lab 6.7 — Read the source

Answer with `file:line` citations in `labs/phase06/source-questions.md`:

1. In `whereLoopAddBtree()`, where is the full-table-scan cost computed, and what constant
   is added? Explain the constant.
2. Where is the covering-index test (`WHERE_IDX_ONLY`), and what does it use `colUsed` for?
3. In `wherePathSolver()`, what is `mxChoice` and how is it chosen?
4. How does the solver keep ordered and unordered paths from evicting each other?
5. Find `whereLoopAddOr()` — how is the multi-index OR implemented at runtime (which
   opcodes)?
6. Find the automatic-index decision (`constructAutomaticIndex` / `whereLoopAddBtree`'s
   `WHERE_AUTO_INDEX` branch). What is the break-even rule?
7. Find the skip-scan condition. How many distinct values must the leading column have for
   skip-scan to be considered?
8. In `select.c`, read the numbered condition list in `flattenSubquery()`. Pick three
   conditions and construct a query that each one blocks; verify with `.treetrace` that
   flattening did not happen.

---

## Lab 6.8 — Build a plan-regression harness

Real planner work needs a way to detect plan changes. Write `plancheck.sh`:

```sh
#!/bin/sh
# usage: ./plancheck.sh db queries.sql baseline.txt
DB=$1; Q=$2; BASE=$3
: > /tmp/plans.txt
while IFS= read -r q; do
  [ -z "$q" ] && continue
  echo "### $q" >> /tmp/plans.txt
  sqlite3 "$DB" "EXPLAIN QUERY PLAN $q" >> /tmp/plans.txt 2>&1
done < "$Q"
if [ -f "$BASE" ]; then diff -u "$BASE" /tmp/plans.txt && echo "NO PLAN CHANGES";
else cp /tmp/plans.txt "$BASE"; echo "baseline created"; fi
```

Use it to detect which of these change plans: `ANALYZE`, adding an index, deleting 90% of
rows then `ANALYZE`, `PRAGMA optimize`, changing `SQLITE_ENABLE_STAT4`.

Deliverable: the harness, a 20-query corpus, and `labs/phase06/plan-regressions.md`.

---

## Lab 6.9 — `PRAGMA optimize` in production shape

```sh
sqlite3 p6.db 'PRAGMA analysis_limit=400; PRAGMA optimize;'     # the recommended pattern
sqlite3 p6.db 'SELECT * FROM sqlite_stat1;'
```

Investigate and write up in `labs/phase06/optimize-pragma.md`:
- what `PRAGMA optimize` actually decides to do (find `pragmaOptimize` / `OPTIMIZE_*` masks)
- what `analysis_limit` bounds, and why an unbounded ANALYZE on a huge table is dangerous
- the recommended application pattern (call it periodically / before close)
