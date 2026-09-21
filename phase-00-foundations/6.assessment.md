# Phase 0 — Assessment

Deliverables go in `labs/phase00/`. Self-grade with the rubric; you must hit **70/100** to
start Phase 1, and you should aim for 85+.

---

## Task 1 — Reproducible debug build (15 pts)
Produce `labs/phase00/build-notes.md` containing:
- exact `configure` line and `CFLAGS` used
- output of `./sqlite3 --version` from *your* build (not `/usr/bin/sqlite3`)
- binary size and compile time for the amalgamation build
- one paragraph: why does the amalgamation build faster to run but slower to compile?

*Full marks:* both builds work, and `PRAGMA compile_options;` from your build shows
`DEBUG` and `ENABLE_EXPLAIN_COMMENTS`.

---

## Task 2 — Source map (20 pts)
Produce `labs/phase00/source-map.md`: a table of **20 source files** with one line each
describing responsibility, written *in your own words after opening each file*. Must
include: `tokenize.c`, `parse.y`, `resolve.c`, `build.c`, `select.c`, `expr.c`, `where.c`,
`wherecode.c`, `vdbe.c`, `vdbeaux.c`, `vdbesort.c`, `btree.c`, `pager.c`, `wal.c`,
`pcache1.c`, `os_unix.c`, `malloc.c`, `util.c`, `prepare.c`, `main.c`.

*Full marks:* each line names at least one concrete function from that file.

---

## Task 3 — The spine backtrace (20 pts)
Using `driver.c` from Lab 0.5, capture and explain a backtrace taken at a breakpoint in
`unixRead` during `sqlite3_step()`.

In `labs/phase00/first-backtrace.txt`, annotate each frame with the layer it belongs to:
API / codegen / VDBE / b-tree / pager / VFS.

*Full marks:* ≥6 frames correctly labeled, and you identify which frame first knows the
*page number* being read and which first knows the *rowid*.

---

## Task 4 — Bytecode reading (20 pts)
For each of these three queries, capture `EXPLAIN` output and explain every opcode in prose:

```sql
SELECT count(*) FROM users;
SELECT name FROM users WHERE id = 2;
SELECT name FROM users ORDER BY age DESC;
```

Answer in `labs/phase00/bytecode-notes.md`:
1. Why does query 2 use `SeekRowid`/`NotExists` while query 3 uses `Rewind`/`Next` + a sorter?
2. Which query opens the fewest cursors, and why?
3. Where does the transaction begin in the program, and why is it not at address 0?

*Full marks:* you correctly explain the `Init → Goto` prologue/epilogue trick.

---

## Task 5 — Break and detect (15 pts)
Introduce a one-character bug in `src/select.c` or `src/where.c`, rebuild, and run the TCL
suite until something fails. Record in `labs/phase00/test-notes.md`:
- the diff of your bug
- the first failing test file and the assertion text
- how long the run took, and how many tests ran before the failure

Then `git checkout -- src/` and confirm the suite passes again.

*Full marks:* failure is clearly attributable to your edit, and you can state what
invariant the failing test was protecting.

---

## Task 6 — Written explanation (10 pts)
In `labs/phase00/why-no-server.md`, 400–600 words: **"How does SQLite's serverless design
determine its concurrency model?"** Must mention: file locks, the pending byte, hot
journals, the fact that any process may need to recover another's crash, and why WAL needs
a `-shm` file.

You are not expected to know the answers in detail yet — write your best model now, and
revisit this file at the end of Phase 7. Growth between the two versions *is* the grade.

---

## Rubric

| Band | Meaning |
|---|---|
| 90–100 | Could onboard another engineer to the codebase |
| 75–89 | Solid: builds, debugs, and reads bytecode independently |
| 60–74 | Can build and navigate; bytecode still opaque — redo Task 4 |
| <60 | Redo Labs 0.5–0.6 before continuing |

## Exit criteria (hard gate)
- [ ] `./sqlite3` from your own debug build runs
- [ ] `lldb`/`gdb` breakpoint in `sqlite3BtreeNext` hits and you can walk the stack
- [ ] You can state, from memory, the 6 layers of the architecture in order
- [ ] `testfixture` runs at least one `.test` file to completion
- [ ] MCQ ≥ 16/20
