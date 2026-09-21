# Phase 0 — MCQ (20 questions)

Answers + explanations at the bottom. Target ≥16/20 before moving to Phase 1.

1. SQLite's architecture is best described as:
   a) client/server with a lightweight protocol  b) an embedded library linked into the app
   c) a daemon with shared-memory IPC  d) a filesystem driver

2. Which file is the *canonical* definition of SQL syntax in SQLite?
   a) `tokenize.c`  b) `parse.c`  c) `parse.y`  d) `sqlite3.h`

3. `opcodes.h` is produced by:
   a) hand editing  b) `lemon` from `parse.y`  c) `mkopcodeh.tcl` scanning comments in `vdbe.c`  d) `configure`

4. The amalgamation `sqlite3.c` exists mainly to:
   a) hide source code  b) simplify deployment and enable whole-program optimization
   c) compress the source  d) support C++ builds

5. Building from the canonical tree requires which extra tool?
   a) Python  b) TCL  c) CMake  d) Perl

6. Which layer converts bytecode into disk reads?
   a) VDBE calls b-tree, which calls pager, which calls VFS  b) parser calls pager directly
   c) planner calls VFS  d) VFS calls VDBE

7. The single-threaded `switch` statement that runs a query lives in:
   a) `select.c`  b) `vdbe.c`  c) `btree.c`  d) `main.c`

8. `BtShared` is shared between:
   a) processes  b) `Btree` handles on the same file within one process
   c) all connections globally  d) nothing, it's per-cursor

9. A `Mem` object in `vdbeInt.h` represents:
   a) a memory allocator arena  b) a VDBE register / `sqlite3_value`
   c) a page cache entry  d) a b-tree node

10. `sqlite3DbMalloc()` differs from `sqlite3Malloc()` because it:
    a) is faster on Windows  b) can draw from the connection's lookaside pool
    c) never fails  d) allocates pages

11. `NEVER(x)` in SQLite source means:
    a) runtime guard against attackers  b) branch believed unreachable, marked for coverage tooling
    c) deprecated code  d) assertion enabled in release builds

12. Which statement about SQLite's file format is true?
    a) it changes yearly  b) it is little-endian  c) multi-byte integers on disk are big-endian
    d) it is platform-specific

13. `PRAGMA vdbe_trace=ON` works only if compiled with:
    a) `SQLITE_TEST`  b) `SQLITE_DEBUG`  c) `SQLITE_ENABLE_TRACE`  d) nothing special

14. In SQLite naming, `zName` indicates:
    a) an integer  b) a zero-terminated string  c) a compressed field  d) an enum

15. Which is NOT part of SQLite's testing infrastructure?
    a) TH3  b) SQL Logic Test  c) `dbsqlfuzz`  d) JUnit harness

16. The "usable size" of a page is:
    a) page size minus 100  b) page size minus reserved bytes (header byte 20)
    c) page size minus 8  d) always 4096

17. Contributions to SQLite core:
    a) go through GitHub pull requests  b) require a CLA  c) are rarely accepted; the project is public domain and closed to outside code  d) require Fossil commit rights for any bug report

18. `EXPLAIN` in SQLite prints:
    a) a cost estimate tree  b) the VDBE bytecode program  c) the parse tree  d) index statistics

19. `EXPLAIN QUERY PLAN` differs from `EXPLAIN` because it:
    a) runs the query  b) shows a high-level, human-readable plan summary
    c) shows raw opcodes  d) shows the journal layout

20. Which call chain is correct for `sqlite3_step()` on a table scan?
    a) `sqlite3_step → sqlite3VdbeExec → sqlite3BtreeNext → sqlite3PagerGet → unixRead`
    b) `sqlite3_step → sqlite3RunParser → sqlite3BtreeNext → unixWrite`
    c) `sqlite3_step → sqlite3WhereBegin → unixRead`
    d) `sqlite3_step → pcache1Fetch → sqlite3VdbeExec`

---

## Answers

1. **b** — no server process; the library runs inside your process. Everything else about
   locking/recovery follows from this.
2. **c** — `parse.y` is the Lemon grammar; `parse.c` is generated from it.
3. **c** — `mkopcodeh.tcl` scans `/* Opcode: ... */` comments in `vdbe.c`. Comments are
   compiled inputs.
4. **b** — one translation unit = easier drop-in + cross-function inlining (~5–10% faster).
5. **b** — TCL runs `mkopcodeh.tcl`, `mksqlite3h.tcl`, and builds `testfixture`.
6. **a** — the strict layer cake. No layer skips the one below it.
7. **b** — `sqlite3VdbeExec()` in `vdbe.c`.
8. **b** — `Btree` is per-connection, `BtShared` is per-open-file-per-process.
9. **b** — `Mem` is the value container: registers, bound params, column results.
10. **b** — lookaside is a per-connection slab that avoids global malloc for small,
    short-lived allocations (parse structures).
11. **b** — `NEVER`/`ALWAYS` exist so 100% branch coverage is provable; they are not
    security checks.
12. **c** — big-endian on disk, so files are byte-identical across platforms.
13. **b** — `SQLITE_DEBUG`.
14. **b** — `z` = zero-terminated string. `n`=count, `i`=index, `p`=pointer, `a`=array.
15. **d** — no JUnit; the three harnesses are TCL tests, TH3, SLT (plus fuzzers).
16. **b** — reserved bytes (usually 0, non-zero for encryption/checksum VFS shims).
17. **c** — bug reports are welcome; code contributions essentially are not. Plan your
    contribution path accordingly (extensions, forks, other DBs).
18. **b** — bytecode listing.
19. **b** — `EXPLAIN QUERY PLAN` summarizes loops/indexes; neither form executes the query.
20. **a**.
