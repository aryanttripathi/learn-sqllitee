# Phase 5 — MCQ (25 questions)

1. The VDBE is:
   a) a stack machine  b) a register machine  c) a JIT compiler  d) a tree interpreter

2. Registers are:
   a) 64-bit ints  b) `Mem` objects — full dynamically typed SQL values  c) pointers  d) pages

3. Every program starts with `Init 0 N` jumping to address N because:
   a) N is the entry point of a subroutine  b) the prologue is generated last and appended
   c) it randomizes execution  d) N is the transaction number

4. `OpenRead P1 P2` means:
   a) open cursor P2 on page P1  b) open cursor P1 on root page P2
   c) read P2 bytes  d) open file P1

5. `SeekRowid` with no following `Next` implies:
   a) an error  b) at most one row can match — a PK lookup  c) a full scan  d) a sort

6. In `SELECT id FROM users WHERE age=45` with an index on `age`, the table cursor is:
   a) opened and seeked  b) never opened — the index covers the query
   c) opened but not used  d) opened twice

7. `OP_DeferredSeek` exists to:
   a) delay the transaction  b) avoid seeking the table b-tree unless a column is actually needed
   c) implement LIMIT  d) sort rows

8. A join in SQLite is implemented as:
   a) hash join  b) sort-merge join  c) nested loops, with indexes doing the lookups  d) all three

9. `Gosub`/`Return` in generated code are used for:
   a) user functions  b) code shared between call sites, e.g. GROUP BY flush and reset
   c) triggers only  d) recursion

10. `AggStep`/`AggFinal` correspond to:
    a) the two halves of a sort  b) the step and finalize callbacks of an aggregate function
    c) page splits  d) transaction phases

11. `SoftNull` in an INSERT program marks:
    a) a NOT NULL violation  b) the INTEGER PRIMARY KEY column, stored as NULL in the record
    c) a deferred constraint  d) an unused register

12. In an INSERT, index entries are written by:
    a) `Insert`  b) `IdxInsert`, one per index, before the table `Insert`  c) `MakeRecord`  d) the b-tree automatically

13. `MakeRecord`'s P4 is:
    a) a table name  b) the affinity string applied to the values  c) a collation  d) a jump target

14. Constant expressions appear in the prologue because of:
    a) accident  b) constant factoring (`sqlite3ExprCodeRunJustOnce`)  c) the parser  d) the b-tree

15. `sqlite3ExprIfTrue()` generates:
    a) a boolean in a register  b) jump-based short-circuit evaluation  c) a subroutine  d) a sorter

16. `sqlite3_step()` returning `SQLITE_ROW` means:
    a) the query finished  b) the VM executed `OP_ResultRow` and paused mid-program
    c) a row was inserted  d) the cursor moved

17. A prepared statement left open between steps:
    a) is free  b) holds a read transaction open  c) releases the file  d) re-parses each step

18. `VdbeCursor.aType[]` / `aOffset[]` are:
    a) the b-tree path  b) cached record-header offsets, invalidated by `CACHE_STALE`
    c) column affinities  d) sort keys

19. Reading the 20th column of a wide row costs more than the 1st because:
    a) it is stored elsewhere  b) `OP_Column` must walk the record header to reach it
    c) of locking  d) it is always an overflow page

20. `OP_Program` is used for:
    a) subqueries  b) trigger bodies executed in a new `VdbeFrame`  c) EXPLAIN  d) VACUUM

21. Co-routines (`InitCoroutine`/`Yield`) let SQLite:
    a) run in parallel  b) consume a subquery row-at-a-time without materializing it
    c) handle errors  d) use threads

22. The `/* out2 */` comment after a `case OP_x:` label:
    a) is documentation only  b) is parsed by `mkopcodeh.tcl` into opcode property flags
    c) marks dead code  d) controls optimization

23. `EXPLAIN` vs `EXPLAIN QUERY PLAN`:
    a) both execute the query  b) neither executes; EXPLAIN shows opcodes, EQP shows a loop summary
    c) EQP shows opcodes  d) EXPLAIN shows costs

24. `bytecode('SELECT ...')` is:
    a) a pragma  b) a table-valued function exposing a statement's program as rows
    c) a shell command  d) an extension you must load

25. `SQLITE_DETERMINISTIC` on a UDF enables:
    a) multithreading  b) `OP_PureFunc` and hoisting the call out of loops
    c) index use  d) caching of results on disk

---

## Answers

1. **b** — a register machine since 2008; registers made programs shorter.
2. **b** — a register can hold any SQL value, with a flags word as the type tag.
3. **b** — the prologue (transactions, constants) is only known after the body is built.
4. **b** — P1 is always the cursor number for cursor opcodes.
5. **b**.
6. **b** — the index holds `(age, rowid)`, which is everything needed.
7. **b** — it can turn a non-covering plan into a nearly-covering one.
8. **c** — plus Bloom filters (3.38+) as a pre-filter, but the loop structure is unchanged.
9. **b**.
10. **b** — the same interface used by `sqlite3_create_function`'s xStep/xFinal.
11. **b** — matches the Phase 1 discovery about rowid aliases.
12. **b**.
13. **b** — the affinity string, e.g. `"DBD"`.
14. **b**.
15. **b** — which is exactly why `AND`/`OR` short-circuit.
16. **b** — VM state is preserved; the next step resumes.
17. **b** — the #1 cause of `SQLITE_BUSY` in applications.
18. **b**.
19. **b**.
20. **b**.
21. **b**.
22. **b** — get them wrong and debug builds assert.
23. **b**.
24. **b** — available when built with `SQLITE_ENABLE_BYTECODE_VTAB` (Apple's build has it).
25. **b**.
