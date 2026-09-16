# Phase 8 — Assessment

Deliverables in `labs/phase08/`. Pass mark 75/100. This phase produces public artifacts —
treat everything here as portfolio work.

---

## Task 1 — ⭐ Virtual table (25 pts)
Build and ship a virtual table beyond `myseries`:
- [ ] loads as an extension and works in the shell (5)
- [ ] `xBestIndex` consumes at least two constraint types with correct `omit` handling (5)
- [ ] correct, tuned `estimatedCost`/`estimatedRows`, demonstrated with a join plan (5)
- [ ] `ORDER BY` consumption when applicable, proven by the absence of a sorter (5)
- [ ] a test file (TCL or shell-based) covering ≥8 cases including wrong-usage errors (5)

*Stretch (+10):* make it writable (`xUpdate`) and transactional
(`xBegin`/`xSync`/`xCommit`/`xRollback`), and show it working inside a rolled-back
transaction.

---

## Task 2 — Read the real extensions (10 pts)
`series-diff.md` + `fts5-notes.md`: what official `series.c` handles that yours doesn't, and
a complete map of FTS5's shadow tables with what each stores (verified with `sqlite_schema`
and your Phase 1 `dbparse`).

---

## Task 3 — Sandboxing untrusted SQL (15 pts)
`authz.c` + a policy writeup:
- an authorizer implementing a real read-only, table-scoped policy
- a demonstration of `SQLITE_IGNORE` redacting a column
- `DEFENSIVE`, `TRUSTED_SCHEMA=OFF`, DQS off, limits, and a progress handler that
  interrupts a runaway query
- a short threat model: what an attacker-supplied *database file* could try, and which of
  your settings stops it

---

## Task 4 — ⭐ A submittable regression test (15 pts)
A TCL test file in SQLite's own format that:
- [ ] runs green under `./testfixture` (5)
- [ ] pins a non-obvious behaviour you discovered in Phases 1–7 (5)
- [ ] includes both positive and negative cases, and a plan assertion (`{/SCAN/}` /
      `{~/SCAN/}`) (5)

---

## Task 5 — Coverage archaeology (10 pts)
`coverage.md`: a gcov run, 10 uncovered lines identified in a module you know, the input
that would reach each, and at least 3 that you actually covered with new tests.

---

## Task 6 — Fuzzing (10 pts)
`fuzzing.md`: fuzz your own `dbparse`/`walparse`/vtab. Report executions, crashes found,
minimized reproducers, and the fixes. Also run `fuzzcheck` against a shipped corpus and
report the result.

---

## Task 7 — Bug-report craft (5 pts)
`bug-report-template.md`: a complete, filled-in example including `dbtotxt` output. If you
found a genuine issue, include the real report (and whether you filed it).

---

## Task 8 — ⭐ Capstone (10 pts + up to 15 bonus)
One finished, public artifact from Lab 8.10 with a README, build instructions, and tests.
Bonus points for: an accepted contribution to a downstream project, a published extension
others can install, or a tool that someone else uses.

---

## Rubric
| Band | Meaning |
|---|---|
| 90–100 | You are a credible database-internals engineer with public artifacts |
| 75–89 | You can extend and test SQLite competently |
| 60–74 | Extension works, testing thin — redo Tasks 4 and 5 |
| <60 | Redo Labs 8.1 and 8.4 |

## Exit criteria (hard gate)
- [ ] Your virtual table loads and answers queries correctly, with a sane plan
- [ ] Your TCL test passes under the real `testfixture`
- [ ] You can explain `xBestIndex`'s contract without notes
- [ ] You have one public artifact you would put on a CV
- [ ] MCQ ≥ 20/25

---

## Course completion checklist

- [ ] Phase 0 — build, debug, navigate
- [ ] Phase 1 — file format; `dbparse` works
- [ ] Phase 2 — pager, VFS; `tracevfs` + a custom VFS
- [ ] Phase 3 — b-tree; `minibtree` with `balance_quick`
- [ ] Phase 4 — front end; a keyword/grammar patch
- [ ] Phase 5 — VDBE; an opcode patch
- [ ] Phase 6 — planner; `.wheretrace` fluency
- [ ] Phase 7 — WAL; `walparse` validates checksums
- [ ] Phase 8 — extensions, testing, a public artifact

When every box is ticked, rewrite `labs/phase00/why-no-server.md` from scratch and diff it
against your Phase 0 version. That diff is the measure of this course.
