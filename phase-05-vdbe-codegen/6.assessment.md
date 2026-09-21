# Phase 5 — Assessment

Deliverables in `labs/phase05/`. Pass mark 75/100.

---

## Task 1 — The 20-query drill (25 pts)
`drills.md`: all 20 queries from Lab 5.1, each with its `EXPLAIN` output and a complete
line-by-line explanation.

Scoring: 1 pt per query correctly explained (opcodes, cursors, loops, where the I/O
happens), +5 for correctly explaining the three hardest (GROUP BY, correlated subquery,
recursive CTE).

*Full marks require:* for every query, a one-line summary of the algorithm ("index range
scan on i_age, deferred table seek, no sort").

---

## Task 2 — Bytecode analysis toolkit (15 pts)
`bytecode-queries.sql` + `bcdiff` tool from Labs 5.2 and 5.10, with `bcdiff-findings.md`
answering all four diff questions.

*Full marks:* your toolkit can automatically answer "does this query sort?", "is this
query covered by an index?", and "how many cursors does it open?" for arbitrary SQL.

---

## Task 3 — Execution trace (10 pts)
`trace.md`: a full `vdbe_trace` of a filtered scan over ≥5 rows, annotated with loop
iterations and register values, plus the answer to the `OP_Column` counting question.

---

## Task 4 — ⭐ Add an opcode (25 pts)
`opcode-patch.diff` implementing `OP_Hello` (or something more ambitious — see below) with:
- [ ] the opcode `case` and doc comment in `vdbe.c` (5)
- [ ] correct `/* out2 */`-style annotations, verified by a clean `SQLITE_DEBUG` build (5)
- [ ] codegen that actually emits it, reachable from SQL (10)
- [ ] `./testfixture ../test/select1.test` and `../test/func.test` still pass (5)

*Stretch (+10):* implement something genuinely useful, e.g.
- `OP_RowCount` that returns the b-tree entry count via `sqlite3BtreeCount`
- an opcode that exposes the current page number of a cursor
- an `EXPLAIN`-visible `OP_Trace` that logs register contents to a callback

Notes must explain what is generated, what you hand-edited, and how `mkopcodeh.tcl` uses
your comment.

---

## Task 5 — Read three opcodes to the bottom (15 pts)
`opcodes-read.md`: all five questions from Lab 5.7, each with `file:line` citations and a
short code quote.

---

## Task 6 — Performance experiments (10 pts)
`perf.md`:
- wide-table first-column vs last-column sum, with times and an explanation
- prepare-once-step-many vs prepare-every-time, with times
- one experiment of your own design that demonstrates a bytecode-level effect
  (suggestions: `LIMIT 1` and early exit, `OP_Once` on a repeated subquery, deterministic
  vs non-deterministic UDF)

---

## Task 7 — Teach-back (bonus 10 pts)
`vm-walkthrough.md`: "What happens between `sqlite3_step()` and a row appearing" — a full
narrative for one non-trivial query, naming every layer and the opcodes crossing each
boundary.

---

## Rubric
| Band | Meaning |
|---|---|
| 90–100 | You can read any SQLite program and modify the VM |
| 75–89 | You read bytecode fluently and can add codegen |
| 60–74 | Reading ok, patching shaky — redo Task 4 |
| <60 | Redo Labs 5.1 and 5.3 |

## Exit criteria (hard gate)
- [ ] You can read an unfamiliar `EXPLAIN` output and describe the algorithm
- [ ] You can name the opcode that does the disk read, the one that sorts, and the one that
      returns rows
- [ ] Your opcode patch builds and runs
- [ ] You can explain why the prologue is at the end
- [ ] MCQ ≥ 20/25
