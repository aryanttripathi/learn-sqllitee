# Phase 4 — Assessment

Deliverables in `labs/phase04/`. Pass mark 75/100.

---

## Task 1 — Tokenizer mastery (15 pts)
`tokens.md` with token dumps and explanations for all five exercises in Lab 4.1, plus:
- a table of the four quoting styles and what each produces
- your own explanation of `keywordCode()` and the generated hash (cite line numbers in
  `sqlite3.c`)
- one input you found that produces `TK_ILLEGAL`

---

## Task 2 — ⭐ Tree-trace analysis (25 pts)
`treetrace-notes.md` covering all eight queries from Lab 4.2. For each:
- the tree *before* and *after* name resolution
- what `{cursor:column}` values were assigned and why
- which optimizer stages appear in the trace

Then, in depth, for `SELECT name FROM (SELECT * FROM users WHERE age>40)`:
- show the flattening, name the function and its file:line
- list at least four of the conditions in `flattenSubquery()` that must hold for
  flattening to be legal, and construct a query that violates one (e.g. an aggregate
  subquery, a LIMIT, a LEFT JOIN on the right side) and show the un-flattened trace

---

## Task 3 — Grammar reading (15 pts)
`grammar-notes.md` answering all five questions in Lab 4.3, with `parse.y` line citations,
plus the operator-precedence table copied from the source and one example query per
precedence level demonstrating the binding.

---

## Task 4 — Affinity & collation (15 pts)
`affinity.md`:
- 18-column affinity prediction table with reasoning, verified against `typeof()`
- explanation of the four comparison results in Lab 4.5
- a demonstration that an index on a BINARY column is *not* used for a `COLLATE NOCASE`
  query, plus the fix (`CREATE INDEX ... COLLATE NOCASE`), shown with `EXPLAIN QUERY PLAN`

---

## Task 5 — ⭐ Add a keyword (20 pts)
Deliver `add-keyword.diff`: a working grammar extension in your local SQLite tree.

Minimum: a new statement that parses and does something observable.
Suggested ladder (pick one):
- `SHAPE <select>` that prints the number of result columns
- `PRAGMA my_stats` implemented in `pragma.c` (note: `pragma.h` is generated from
  `tool/mkpragmatab.tcl` — find out how)
- an expression operator such as `a <-> b` mapped to an existing function

Your notes must state:
- every file you edited, and every *generated* file that changed as a result
- how you discovered where to edit
- what broke first, and how you diagnosed it
- the result of running `./testfixture ../test/select1.test` after your change

---

## Task 6 — Schema forensics (10 pts)
`schema-notes.md`: the corruption/repair exercise from Lab 4.6 plus answers to its four
questions, including a reproduction of `SQLITE_SCHEMA` with two connections and a
demonstration of the difference between `prepare()` and `prepare_v2()`.

---

## Task 7 — Teach-back (bonus 10 pts)
`front-end-walkthrough.md`: trace `SELECT u.name FROM users u JOIN orders o ON o.uid=u.id
WHERE o.total > 100` from characters to a fully resolved tree, in your own words and
diagrams, naming every function that touches it.

---

## Rubric
| Band | Meaning |
|---|---|
| 90–100 | You could add a SQL feature end-to-end |
| 75–89 | You can read and modify the front end confidently |
| 60–74 | Trees understood, grammar shaky — redo Tasks 3 and 5 |
| <60 | Redo Labs 4.1, 4.2 |

## Exit criteria (hard gate)
- [ ] `tokdump` builds and you can explain every stage of its treetrace output
- [ ] You can state the 6-step name-resolution order from memory
- [ ] You can state the 5 affinity rules from memory, in order
- [ ] Your keyword patch compiles and the statement parses
- [ ] MCQ ≥ 20/25
