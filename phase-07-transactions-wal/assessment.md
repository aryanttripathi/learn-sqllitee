# Phase 7 — Assessment

Deliverables in `labs/phase07/`. Pass mark 75/100.

---

## Task 1 — ⭐ WAL decoder + verifier (25 pts)
Extend `walparse.c` into a real tool:
- [ ] header + all frame headers, with commit frames marked (5)
- [ ] full checksum chain verification, matching SQLite exactly (10)
- [ ] `--recover` mode: report which frames a recovery would accept, and what the resulting
      database size would be (5)
- [ ] `--pages` mode: for a given page number, list every frame holding it and which one a
      reader with a given read mark would see (5)

*Stretch (+10):* apply the accepted frames to a copy of the database file (your own
checkpoint implementation) and verify the result with `PRAGMA integrity_check`.

---

## Task 2 — Corruption matrix (15 pts)
`wal-corruption.md`: five corruption experiments from Lab 7.2. For each: the bytes changed,
your tool's verdict, SQLite's actual behaviour after reopening, and `integrity_check`
output. Explain any case where your prediction was wrong.

---

## Task 3 — Concurrency demonstrations (15 pts)
`concurrency.md`:
- the reader/writer matrix for WAL vs rollback (reproduced, not copied)
- a demonstration of snapshot isolation (the reader not seeing a concurrent commit)
- a demonstration that writers serialize
- a reproduction of `SQLITE_BUSY_SNAPSHOT` and the correct application response
- a measurement of how `busy_timeout` changes the failure rate under contention

---

## Task 4 — Checkpoint policy (15 pts)
`checkpoint.md`: Lab 7.5's numbers plus:
- a reproduction of unbounded WAL growth, with the `wal_checkpoint` return values proving
  the cause
- the difference between PASSIVE/FULL/RESTART/TRUNCATE, demonstrated
- a working `sqlite3_wal_hook()` implementation in C that checkpoints on a policy you
  choose, with an explanation of why the default autocheckpoint is sometimes wrong

---

## Task 5 — Durability benchmark (10 pts)
`wal-bench.md`: the full (journal_mode × synchronous × batching) grid with real timings,
plus a recommendation table for four application archetypes.

---

## Task 6 — Savepoints & conflict resolution (10 pts)
`savepoints.md`: Lab 7.7's experiments including the `OR FAIL` vs `OR ABORT` proof, plus a
`tracevfs` trace showing the statement journal being created.

---

## Task 7 — Source questions (10 pts)
`source-questions.md`: all eight `wal.c` questions from Lab 7.9, with `file:line` citations.

---

## Task 8 — Crash matrix (bonus 10 pts)
`crash-matrix.md`: Lab 7.10 across five configurations, with your pre-recovery predictions
recorded *before* reopening each database, and an accuracy score for your predictions.

---

## Rubric
| Band | Meaning |
|---|---|
| 90–100 | You could triage a real corruption report and explain WAL behaviour to a team |
| 75–89 | Solid grasp of both durability engines and their trade-offs |
| 60–74 | Concepts ok, byte-level shaky — redo Tasks 1 and 2 |
| <60 | Redo Labs 7.1, 7.3, 7.5 |

## Exit criteria (hard gate)
- [ ] Your WAL parser validates checksums on a real file
- [ ] You can draw the WAL + wal-index + read-mark picture from memory
- [ ] You can explain why a long reader stalls checkpointing
- [ ] You can state what `synchronous=NORMAL` risks in each journal mode
- [ ] You can name the three safe backup methods and why `cp` is not one
- [ ] MCQ ≥ 20/25
