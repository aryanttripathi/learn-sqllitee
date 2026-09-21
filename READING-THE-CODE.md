# How to Read SQLite's Source Without Drowning

262,899 lines in one file. This document is the mental model that makes that tractable.
Read it once before Phase 0, and re-read it whenever you feel lost.

---

## 1. The single most important idea: SQLite is a stack of translators

Nothing in SQLite is "the code that runs a query". Each layer **translates a request into a
simpler request** and hands it down:

```
  "SELECT name FROM users WHERE age>40"     ← text
        ↓ tokenizer                          translates to tokens
  TK_SELECT TK_ID TK_FROM TK_ID ...
        ↓ parser                             translates to a tree
  Select{ pEList, pSrc, pWhere }
        ↓ planner + code generator           translates to a program
  OpenRead 0,2 / Rewind / Column / Le / ResultRow / Next
        ↓ VDBE                               translates to cursor calls
  sqlite3BtreeNext(cursor)
        ↓ b-tree                             translates to page requests
  sqlite3PagerGet(pager, 7, &page)
        ↓ pager                              translates to file I/O
  unixRead(fd, buf, 4096, 24576)
```

**Therefore: when reading any function, the only two questions are**
1. What request did it receive (its arguments)?
2. What simpler request does it make of the layer below?

Everything else in the function is bookkeeping, error handling, or an optimization.
On a first pass you may ignore all of it.

---

## 2. The three-pass reading method

Never read a SQLite function top to bottom on your first encounter. Do this instead:

### Pass 1 — the header comment (60 seconds)
Every non-trivial function has a block comment that states the contract. SQLite's comments
are unusually honest and complete; they are the actual documentation. Read *only* that,
plus the parameter list, then move on.

```c
/*
** Move the cursor down to a new child page.  The newPgno argument is the
** page number of the child page to move to.
**
** This function returns SQLITE_CORRUPT if the page-header flags field of
** the new child page does not match the flags field of the parent (i.e.
** if an intkey page appears to be the parent of a non-intkey page, or
** vice-versa).
*/
static int moveToChild(BtCursor *pCur, u32 newPgno){
```
You now know what `moveToChild` does. That is often enough.

### Pass 2 — the skeleton (5 minutes)
Read only:
- the local variable declarations (they name the concepts)
- the `if`/`while`/`switch` structure
- the calls to *other* functions

Skip: every `assert()`, every `testcase()`, every `#ifdef`, every error path that ends in
`goto`. You are building a map, not verifying correctness.

### Pass 3 — the details (as needed)
Only now read the asserts — and read them as **specification**, not as defensive code:

```c
assert( pPage->intKeyLeaf );        /* "this function is only ever called on
                                       leaf pages of a table b-tree" */
assert( CORRUPT_DB || pPage->nFree>=0 );  /* "nFree is valid unless the FILE is corrupt" */
```
`assert( CORRUPT_DB || X )` is SQLite's signature idiom and means: *X is guaranteed by the
file format; if it fails, the database is corrupt, not the code.* Those asserts are the
file-format spec restated in C.

---

## 3. Read the naming convention once, then read code twice as fast

Prefixes are used with total discipline. A signature tells you the data flow before you
read the body.

| Prefix | Meaning | Example |
|---|---|---|
| `p` | pointer | `pPage`, `pCur`, `pParse` |
| `a` | array | `aOp`, `aMem`, `aCell` |
| `n` | count / number of | `nCell`, `nByte`, `nRow` |
| `i` | index or integer | `iDb`, `iCol`, `iOffset` |
| `z` | zero-terminated string | `zName`, `zSql`, `zErrMsg` |
| `e` | enum / state value | `eState`, `eLock`, `eDest` |
| `b` | boolean | `bSeenOne`, `bRev` |
| `u` / `x` | union / struct member | `u.zToken`, `x.pList` |
| `mx` | maximum | `mxFrame`, `mxChoice` |
| `sz` | size in bytes | `szPage`, `szCell` |

Function prefixes:
```
sqlite3_xxx()    PUBLIC API — frozen forever, documented on sqlite.org
sqlite3Xxx()     internal, visible across files
xxxYyy()         file-static helper (most of them)
```
So `sqlite3BtreeInsert` is internal and lives in `btree.c`; `sqlite3_bind_int` is public.

Other markers you will see constantly:
```c
SQLITE_PRIVATE      /* = static in the amalgamation; it is NOT static in src/*.c */
UNUSED_PARAMETER(x) /* portable "I know" for unused args */
testcase( x );      /* a coverage marker: TH3 proves both branches get exercised */
VVA_ONLY(x)         /* code that exists only for asserts ("verification, validation, assert") */
NEVER(x) / ALWAYS(x)/* branch believed unreachable/always-taken; for coverage, not safety */
/* EVIDENCE-OF: R-24078-09375 ... */  /* this line implements that documented requirement */
```

`EVIDENCE-OF` is worth pausing on: those tags tie individual lines of C to individual
sentences in the published documentation, so the test suite can prove every documented
requirement is implemented and exercised. When you see one, the sentence it quotes is the
authoritative explanation of that code.

---

## 4. Where to read: three views of the same code

| View | Use it for | How |
|---|---|---|
| **`src/*.c` in the tree** | real work, patches, discussing with others | `git clone https://github.com/sqlite/sqlite` |
| **the amalgamation** | grep-everything, one buffer, following a call chain | `sqlite-amalgamation-*/sqlite3.c` |
| **the web browser** | linking, reading on a phone, checking history | see below |

Canonical online sources:
- Fossil (source of truth): `https://sqlite.org/src/file?name=src/btree.c&ci=trunk`
- GitHub mirror (nice line anchors): `https://github.com/sqlite/sqlite/blob/master/src/btree.c`
- Timeline / blame: `https://sqlite.org/src/timeline`, `https://sqlite.org/src/annotate?filename=src/btree.c`
- Doc pages that *are* internals docs: `arch.html`, `fileformat2.html`, `opcode.html`,
  `optoverview.html`, `queryplanner-ng.html`, `wal.html`, `atomiccommit.html`, `vdbe.html`

**Do not cite amalgamation line numbers to anyone.** They change with every release and
mean nothing to the developers. Cite `src/btree.c` + the function name.

Find anything, in either view:
```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400

# where is it defined?  (definitions start at column 0)
grep -n "^[a-zA-Z_].*balance_nonroot(" $A/sqlite3.c

# who calls it?
grep -n "balance_nonroot(" $A/sqlite3.c

# print a function
sed -n '/^static int balance_nonroot(/,/^}/p' $A/sqlite3.c | head -80

# the header comment above it
grep -n -B30 "^static int balance_nonroot(" $A/sqlite3.c | head -40
```

---

## 5. The four landmark comments

SQLite hides four long design documents *inside* source files. Each is better than any
blog post about the subject. Read the comment, not the code, first:

| Location | What it explains |
|---|---|
| top of `src/btree.c` | the complete file format, page layout, cell format |
| top of `src/pager.c` | the pager state machine and the atomic-commit argument |
| top of `src/wal.c` | ~400 lines: the entire WAL design, reader/writer/checkpoint protocol |
| top of `src/where.c` | the query planner's cost model and solver |

```sh
sed -n '1,120p' $A/sqlite3.c | head -5      # amalgamation header
# in the source tree, simply:  head -200 src/wal.c
```

---

## 6. How to trace a real query (the technique you will use most)

Static reading has limits. Three dynamic techniques answer "what actually runs?":

**(a) The debugger — ground truth**
```sh
lldb ./sqlite3 mydb.db
(lldb) b sqlite3BtreeNext
(lldb) run
sqlite> SELECT ...;
(lldb) bt                    # the whole layer cake in one screen
```

**(b) The built-in traces — the layer's own narration** (needs `SQLITE_DEBUG`)
```
.treetrace 0xffff        parse tree at each transformation      (Phase 4)
.wheretrace 0xfff        every WhereLoop candidate and its cost (Phase 6)
PRAGMA vdbe_listing=ON;  the bytecode program                   (Phase 5)
PRAGMA vdbe_trace=ON;    every instruction as it executes       (Phase 5)
```

**(c) The bytecode itself — free, no special build**
```sql
EXPLAIN SELECT ...;                        -- the program
EXPLAIN QUERY PLAN SELECT ...;             -- the loop summary
SELECT * FROM bytecode('SELECT ...');      -- the program, as queryable rows
```

`EXPLAIN` is the fastest way to answer "which code path will this take?" — the opcodes name
the functions. `OP_Column` → `sqlite3VdbeExec`'s `case OP_Column` → `sqlite3BtreePayload`.

---

## 7. Reading strategy per layer

| Layer | Read this way |
|---|---|
| **Front end** (`tokenize.c`, `parse.y`, `resolve.c`) | Read `parse.y` grammar rules, not `parse.c`. `parse.c` is generated — skim it once to see the table-driven machine, then never again. |
| **Code generator** (`select.c`, `expr.c`, `insert.c`) | Read it **alongside `EXPLAIN` output**. Every function emits specific opcodes; match code to opcodes. |
| **Planner** (`where.c`) | Read it **alongside `.wheretrace`**. The cost numbers make the code concrete. |
| **VDBE** (`vdbe.c`) | It is 190 independent mini-programs. Never read it linearly — jump to one `case OP_X:` at a time. |
| **B-tree** (`btree.c`) | Read the header comment, then cursor movement, then insert, then `balance_nonroot` last. Draw pictures. |
| **Pager/WAL** (`pager.c`, `wal.c`) | Read the header comments (they are the design docs), then follow one transaction with the VFS tracer from Phase 2. |
| **VFS** (`os_unix.c`) | Read only the function you need. It is 8k lines of platform workarounds. |

---

## 8. The five questions that unstick you

When a function makes no sense, ask in this order:

1. **What layer am I in?** (If you don't know, you are reading the wrong function.)
2. **What does the header comment promise?**
3. **What is the *one* call this makes to the layer below?** Everything else is local.
4. **What invariant do the asserts claim?** That is usually the missing context.
5. **Can I make it run?** Set a breakpoint, print the arguments. Ten seconds of `lldb`
   beats an hour of staring.

---

## 9. What to skip, guilt-free

On a first pass, skip **all** of:
- `#ifdef SQLITE_OMIT_*` blocks (feature-removal builds)
- `#ifdef SQLITE_ENABLE_*` for features you are not studying
- mutex enter/leave pairs (`sqlite3BtreeEnter`, `sqlite3_mutex_held`) — shared-cache and
  threading bookkeeping
- `testcase()`, `VVA_ONLY()`, `NEVER()`, `ALWAYS()` — coverage instrumentation
- OOM handling (`db->mallocFailed`, `goto no_mem`) — a uniform mechanism, not logic
- the `UPDATE`/`DELETE` variants until you have read the `INSERT` path
- everything in `ext/` until Phase 8

That removes roughly half the text and none of the meaning.

---

## 10. A worked example of the method

Goal: understand how `SELECT name FROM users WHERE id=2` reads a row.

```sh
# 1. what will run?
sqlite3 db '.explain on' 'EXPLAIN SELECT name FROM users WHERE id=2;'
#    → SeekRowid, Column, ResultRow
```
```sh
# 2. find the opcode implementations
grep -n "case OP_SeekRowid:" $A/sqlite3.c
grep -n "case OP_Column:"    $A/sqlite3.c
```
```
# 3. read the /* Opcode: */ comment above each case — that is the contract
# 4. note the one call each makes downward:
#      OP_SeekRowid → sqlite3BtreeTableMoveto()
#      OP_Column    → sqlite3BtreePayloadFetch() / sqlite3VdbeSerialGet()
# 5. repeat the process one layer down
```
```sh
# 6. confirm with the debugger
lldb ./sqlite3 db
(lldb) b sqlite3BtreeTableMoveto
(lldb) run
sqlite> SELECT name FROM users WHERE id=2;
(lldb) bt
```

Six steps, twenty minutes, and you have a verified end-to-end understanding of one path.
Repeat for `INSERT`, then for a join, and the codebase stops being opaque.

---

## 11. Per-phase walkthroughs

Each phase directory now contains **`2.code_walkthrough.md`**: real, annotated SQLite C for
that layer, chosen to be short enough to read line by line. Large functions
(`balance_nonroot`, `sqlite3VdbeExec`, `wherePathSolver`) are explained by *mechanism* with
a link to the source rather than pasted in full — reading 700 lines of redistribution logic
on a page is not how anyone learns it.

Use them in this order: `1.knowledge.md` → `2.code_walkthrough.md` → `3.implementation.md`.
