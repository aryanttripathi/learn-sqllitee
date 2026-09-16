# Phase 3 — Assessment

Deliverables in `labs/phase03/`. Pass mark 75/100. This is the hardest phase so far; budget
two weeks.

---

## Task 1 — ⭐ Extend `minibtree.c` (30 pts)

Start from the working `labs/phase03/minibtree.c`.

- [ ] **balance_quick (10 pts)** — implement the append fast path. Report fill factor for
      `seq 50000` before and after. Expect a jump from ~45% to >90%.
- [ ] **sibling redistribution (10 pts)** — before splitting, try to shift cells into a
      left or right sibling. Report the effect on `rand 50000`.
- [ ] **delete + freeblocks (10 pts)** — implement `btDelete()` with a freeblock list,
      fragment accounting, `defragmentPage()`, and the "balance only when `nFree >
      2/3 page`" rule. Verify with the built-in order/count checks after deleting every
      third key.

*Stretch (+10):* variable-length keys (make it an index b-tree with `memcmp` ordering) or
overflow pages for payloads > page size.

All variants must keep `checks: lookups missing=0  cells==distinct? YES  keys in order? YES`.

---

## Task 2 — Split forensics in real SQLite (15 pts)
`split-log.md`: the exact insert at which a 512-byte-page table's root splits, the tree
shape before and after (from your `dbparse`), and a debugger-confirmed list of which
balance function ran. Include the cursor path (`iPage`, `aiIdx[]`) before and after.

---

## Task 3 — `balance_nonroot` explained (20 pts)
`balance-nonroot.md`: a one-page, in-your-own-words walkthrough of the function, covering
all eight reading stages listed in `internals_important.md`, plus:
1. Why are dividers pulled *down* into the cell pool for index b-trees but regenerated for
   table b-trees?
2. Why must new pages be allocated in increasing page-number order?
3. What exactly is written to the ptrmap at the end, and why in that order?
4. What would break if `nOld` were fixed at 2 instead of up to 3?

*Full marks:* you cite `file:line` for each claim.

---

## Task 4 — Fill-factor report (15 pts)
`fill-report.md` with real measurements (dbstat or sqlite3_analyzer) for:
- sequential rowid insert, 200k rows
- random rowid insert, 200k rows
- random TEXT PK in a `WITHOUT ROWID` table, 200k rows
- the same three after `VACUUM`

Include file sizes, page counts, average fill, and a paragraph of schema-design advice
derived from the numbers.

---

## Task 5 — Integrity messages (10 pts)
`integrity-messages.md`: five distinct `integrity_check` error messages, each reproduced by
a deliberate corruption, mapped to the invariant violated and the `btree.c` line that
detects it.

---

## Task 6 — Source questions (10 pts)
`source-questions.md`: all eight questions from Lab 3.10, each answered with `file:line`
citations and a short quote of the relevant code.

---

## Task 7 — Teach-back (bonus 10 pts)
A 10-minute talk (write the script) titled **"What happens when you INSERT a row"**, going
from `sqlite3_step` down to `balance_nonroot` and back. Include one diagram you drew
yourself.

---

## Rubric
| Band | Meaning |
|---|---|
| 90–100 | You could review a patch to `btree.c` |
| 75–89 | You can navigate and modify the b-tree layer with a debugger |
| 60–74 | Concepts ok, code unfamiliar — redo Tasks 1 and 3 |
| <60 | Redo Labs 3.1, 3.3, 3.4 |

## Exit criteria (hard gate)
- [ ] Your `minibtree` handles 50k sequential and 50k random inserts with all checks green,
      including `balance_quick`
- [ ] You can draw a page split and a root split from memory
- [ ] You can explain `CURSOR_REQUIRESEEK` without notes
- [ ] You can state where each of `balance`, `balance_quick`, `balance_deeper`,
      `balance_nonroot` is called from
- [ ] MCQ ≥ 20/25
