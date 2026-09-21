# Phase 8 — MCQ (25 questions)

1. A virtual table's cursor struct must:
   a) be allocated with malloc  b) begin with `sqlite3_vtab_cursor base`
   c) contain a BtCursor  d) be thread-local

2. `xBestIndex`'s `omit = 1` means:
   a) skip the column  b) SQLite will not re-check this constraint — you must enforce it
   c) the constraint is unusable  d) omit the row

3. Setting `orderByConsumed = 1` without producing that order causes:
   a) an error  b) silently wrong results  c) a sort  d) a crash

4. An eponymous virtual table is one where:
   a) `xCreate` is 0 (or equals `xConnect`), so it is usable by name without CREATE
   b) the name equals the database  c) it has no columns  d) it is read-only

5. HIDDEN columns in a vtab declaration:
   a) cannot be read  b) act as arguments in table-valued-function syntax
   c) are encrypted  d) are not stored

6. `estimatedCost` returned by `xBestIndex`:
   a) is ignored  b) competes with b-tree plan costs in the planner
   c) must be 1.0  d) is in milliseconds

7. `xBestIndex` may be called:
   a) exactly once per query  b) many times — once per candidate join order
   c) once per row  d) only on the first prepare

8. FTS5 stores its data in:
   a) a special file  b) ordinary shadow tables (`_data`, `_idx`, `_content`, …)
   c) the WAL  d) memory only

9. `xShadowName` exists to:
   a) rename tables  b) let SQLite protect shadow tables in defensive mode
   c) hide columns  d) support aliases

10. `SQLITE_DETERMINISTIC` on a function allows:
    a) parallelism  b) `OP_PureFunc`, hoisting out of loops, and use in index expressions
    c) larger results  d) recursion

11. `SQLITE_DIRECTONLY` means the function:
    a) is faster  b) may not be called from views/triggers — a security boundary
    c) returns directly  d) bypasses the VDBE

12. `sqlite3_set_authorizer` callbacks run:
    a) at execution  b) at statement preparation  c) at commit  d) per row

13. Returning `SQLITE_IGNORE` from the authorizer for a `SQLITE_READ` action:
    a) fails the query  b) makes that column read as NULL  c) skips the row  d) denies access

14. `sqlite3_progress_handler` is used to:
    a) show progress bars only  b) make long-running queries interruptible
    c) count rows  d) profile

15. Apple's system `/usr/bin/sqlite3`:
    a) supports `.load`  b) has extension loading disabled — build your own shell
    c) has no vtabs  d) is read-only

16. The extension init function must be named:
    a) `sqlite3_init`  b) `sqlite3_<basename>_init` matching the library file name
    c) `main`  d) anything

17. Memory returned in `pVtab->zErrMsg` must be allocated with:
    a) malloc  b) `sqlite3_mprintf`  c) strdup  d) a static buffer

18. SQLite's public test suite is written in:
    a) Python  b) TCL  c) C++  d) shell

19. `do_catchsql_test NAME {SQL} {1 {msg}}` asserts:
    a) the SQL succeeds  b) the SQL fails with that exact message
    c) it returns 1 row  d) nothing

20. TH3 is:
    a) the public test suite  b) a proprietary suite used for 100% MC/DC branch coverage
    c) a fuzzer  d) a benchmark

21. `dbsqlfuzz` mutates:
    a) SQL only  b) both SQL text and database files  c) the source code  d) test scripts

22. `SQLITE_TESTCTRL_IMPOSTER` lets a test:
    a) fake a schema over an existing root page, to write corrupt content deliberately
    b) impersonate a user  c) mock the VFS  d) disable assertions

23. The canonical SQLite repository is hosted with:
    a) Git on GitHub  b) Fossil at sqlite.org/src (GitHub is a read-only mirror)
    c) SVN  d) Mercurial

24. Outside code contributions to SQLite core are:
    a) accepted via PR  b) rarely accepted; the project is public domain and closed to
       outside code, but bug reports are welcomed
    c) accepted with a CLA  d) accepted only from companies

25. The highest-value contribution you can make as an outsider is:
    a) a refactor  b) a minimal reproducible bug report (especially fuzzer-found)
    c) a style fix  d) a benchmark

---

## Answers

1. **b** — SQLite casts your pointer; a missing/reordered base member is instant UB.
2. **b** — the classic silent-wrong-answers bug.
3. **b** — same class of bug.
4. **a** — e.g. `generate_series`, `bytecode`, `pragma_table_info`.
5. **b**.
6. **b** — return a huge cost for plans you don't want.
7. **b** — keep it cheap and side-effect free.
8. **b** — check `sqlite_schema` after creating one.
9. **b**.
10. **b**.
11. **b**.
12. **b** — which is why it can block a statement before it ever runs.
13. **b** — a neat way to redact columns instead of failing.
14. **b** — plus `sqlite3_interrupt` from another thread.
15. **b**.
16. **b** — or pass the entry point explicitly to `.load`.
17. **b**.
18. **b**.
19. **b**.
20. **b**.
21. **b** — which is why every byte of the Phase 1 format has adversarial tests.
22. **a**.
23. **b**.
24. **b**.
25. **b**.
