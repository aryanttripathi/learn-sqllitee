# Phase 6 — Assessment

Deliverables in `labs/phase06/`. Pass mark 75/100.

---

## Task 1 — ⭐ The ANALYZE flip, fully explained (20 pts)
`analyze-flip.md` must contain:
- the before/after plans (reproduce them)
- the `sqlite_stat1` contents decoded token by token
- an explanation of *why* the post-ANALYZE plan is worse for `status='pending'` and better
  for `status='done'`
- a STAT4 build showing different plans for the two values
- timing for all four combinations (value × statistics)

*Full marks:* you can state the general rule about averages vs. skew, and name one other
database engine's equivalent mechanism (histograms).

---

## Task 2 — Plan zoo (20 pts)
`plan-zoo.md`: EQP capture + explanation for all 17 queries in Lab 6.2, plus the three
advanced features in the co-routine/Bloom-filter/automatic-index plan.

Scoring: 1 pt per query correctly explained; +3 for the advanced plan.

---

## Task 3 — ⭐ Wheretrace analysis (20 pts)
`wheretrace.md`: all five deliverables from Lab 6.3, for a 3-table join with an ORDER BY.

*Full marks:* your LogEst→row-count conversions are correct and you can point to the exact
round in which the winning path overtook its competitor.

---

## Task 4 — LogEst arithmetic (10 pts)
`logest.md`: the tool output plus four hand-computed plan costs checked against
`.wheretrace`, each within ±3 LogEst units of the planner's number, with any discrepancy
explained.

---

## Task 5 — Index clinic (15 pts)
`index-clinic.md`: all seven queries from Lab 6.6 with minimum index sets, EQP before/after,
timings, and index size costs. Must include one partial index and one covering index, and
one case where adding an index made something *worse* (write amplification or a bad plan
choice) — find one.

---

## Task 6 — Source questions (10 pts)
`source-questions.md`: all eight questions from Lab 6.7 with `file:line` citations. Question
8 (flattenSubquery conditions) requires three working counterexample queries.

---

## Task 7 — Plan-regression harness (5 pts)
`plancheck.sh` + a 20-query corpus + `plan-regressions.md` documenting which changes moved
which plans.

---

## Task 8 — Optimization value measurement (bonus 10 pts)
`optimization-value.md`: using `SQLITE_TESTCTRL_OPTIMIZATIONS`, measure the speedup
contributed by at least six individual optimizations on your workload, with a short note on
which are safe to disable and which change results (none should — verify!).

---

## Rubric
| Band | Meaning |
|---|---|
| 90–100 | You could debug a planner regression report from a user |
| 75–89 | You read plans fluently and can reason about costs |
| 60–74 | EQP ok, cost model fuzzy — redo Tasks 3 and 4 |
| <60 | Redo Labs 6.1 and 6.3 |

## Exit criteria (hard gate)
- [ ] You can convert between LogEst and row counts in your head (±)
- [ ] You can read `.wheretrace` output and explain the chosen plan
- [ ] You can state the four no-statistics default constants
- [ ] You can name six named optimizations and how to observe each
- [ ] You diagnosed at least one real plan problem and fixed it with an index
- [ ] MCQ ≥ 20/25
