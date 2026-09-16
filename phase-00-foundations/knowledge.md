# Phase 0 — Foundations: What SQLite *Is*, and How Its Source Tree Is Shaped

## 0.1 The one design decision that explains everything

SQLite's defining constraint: **there is no server process**. The database is a file, and
every process that opens it runs the whole engine in its own address space.

Consequences you will meet again in every phase:

| Consequence | Where it bites you later |
|---|---|
| Concurrency control must use *file locks*, not shared memory owned by a daemon | Phase 2 (locking), Phase 7 (WAL uses an extra `-shm` file exactly because file locks alone are too coarse) |
| Crash recovery must be doable by *any* process that opens the file next | Phase 2 (hot journal detection), Phase 7 (WAL recovery) |
| The file format must be frozen forever, cross-platform, cross-endian | Phase 1 (big-endian integers, varints, `schema format` numbers) |
| The library must run with ~a few hundred KB of RAM and no malloc if needed | Phase 2 (page cache), everywhere (`SQLITE_OMIT_*`, lookaside allocator) |
| No IPC ⇒ query execution must be a single-threaded loop | Phase 5 (VDBE is a `switch` in one function) |

SQLite is public-domain C89-ish code, ~155k lines of core source, shipped as one
generated file (`sqlite3.c`, the *amalgamation*, ~250k lines including comments) plus one
header. The project has ~600× more test code than library code.

## 0.2 The layer cake, named by file

```
 API surface          main.c legacy.c vdbeapi.c prepare.c
 ─────────────────────────────────────────────────────────────
 Tokenizer            tokenize.c          "SELECT a FROM t" -> TK_SELECT TK_ID ...
 Parser               parse.y -> parse.c  tokens -> Parse tree (Select/Expr/SrcList)
 Name resolution      resolve.c           "a" -> column 0 of table t
 Query planner        where.c whereexpr.c wherecode.c  choose indexes & join order
 Code generator       select.c insert.c update.c delete.c expr.c trigger.c
 ─────────────────────────────────────────────────────────────
 Virtual machine      vdbe.c vdbeaux.c vdbemem.c vdbesort.c   bytecode interpreter
 ─────────────────────────────────────────────────────────────
 B-tree               btree.c             ordered map on pages
 Pager                pager.c wal.c pcache.c pcache1.c        ACID + caching
 OS interface (VFS)   os_unix.c os_win.c os.c                 read/write/lock/sync
 ─────────────────────────────────────────────────────────────
 Utilities            util.c printf.c hash.c malloc.c mem1.c random.c bitvec.c mutex*.c
```

Rough size ranking (useful for planning your reading): `btree.c` ≈ 11k lines,
`vdbe.c` ≈ 9k, `select.c` ≈ 9k, `where.c` + `wherecode.c` + `whereexpr.c` ≈ 10k,
`pager.c` ≈ 8k, `expr.c` ≈ 7k, `os_unix.c` ≈ 8k, `wal.c` ≈ 4k, `build.c` ≈ 5k.

## 0.3 Generated files — why you cannot just `gcc *.c`

The canonical tree contains *code generators* written in C and TCL. `make` runs them first:

```
tool/lemon.c  + parse.y          ──► parse.c parse.h      (LALR(1) parser generator)
tool/mkkeywordhash.c             ──► keywordhash.h        (perfect hash of SQL keywords)
mkopcodeh.tcl  < vdbe.c          ──► opcodes.h            (opcode numbers, properties)
mkopcodec.tcl                    ──► opcodes.c            (opcode name strings)
tool/mksqlite3h.tcl < sqlite.h.in──► sqlite3.h
tool/mkamalgamation / mksqlite3c.tcl ──► sqlite3.c        (the amalgamation)
```

That is why **TCL is a build dependency** of the source tree (not of the amalgamation).
It is also why the opcode numbers change between versions: they are assigned by a script
that sorts opcodes so that hot ones get favourable jump-table positions.

```
   vdbe.c  ── comments like "/* Opcode: OpenRead P1 P2 P3 P4 P5 */" ──┐
                                                                     ▼
                                                    mkopcodeh.tcl  ─► opcodes.h
                                                                     │
                                            #define OP_OpenRead 109  ┘
```

## 0.4 Amalgamation vs. source tree

| | Amalgamation `sqlite3.c` | Source tree `src/*.c` |
|---|---|---|
| Reading | good for grep-everything, one buffer | good for structure, real file boundaries |
| Debugging | line numbers meaningless to devs | line numbers match upstream discussion |
| Build | `cc sqlite3.c shell.c -o sqlite3` | `./configure && make` (needs TCL) |
| Optimization | ~5–10% faster (whole-program inlining) | normal |
| Contributing | never patch it — it is generated | this is the real code |

Use the amalgamation to *explore*, the tree to *work*.

## 0.5 The object model you need before Phase 1

Four handles that appear in every stack trace:

```
sqlite3          — a database connection. Holds: Db[] array (one per attached schema),
                   mutex, lookaside allocator, function table, pointer to open Btrees.

Btree / BtShared — Btree is per-connection; BtShared is the shared-per-file state
                   (page cache, pager, lock state). Multiple Btree handles on the same
                   file in the same process share one BtShared (shared-cache mode aside,
                   this is how ATTACH of the same file behaves).

Pager            — page cache + journal/WAL state + file handle.

Vdbe             — one prepared statement: an array of Op, an array of Mem registers,
                   an array of VdbeCursor.
```

Call-chain for the simplest possible query, memorize this spine:

```
sqlite3_prepare_v2()
  └─ sqlite3RunParser()            tokenize.c
       └─ yy_reduce() in parse.c   builds Select/Expr trees
            └─ sqlite3Select()     select.c
                 ├─ sqlite3WhereBegin()   where.c   (plan + open cursors)
                 ├─ ... codegen ...       expr.c
                 └─ sqlite3WhereEnd()
sqlite3_step()
  └─ sqlite3VdbeExec()             vdbe.c    (the interpreter loop)
       └─ sqlite3BtreeNext() / sqlite3BtreePayload()   btree.c
            └─ sqlite3PagerGet()   pager.c
                 └─ unixRead()     os_unix.c  -> pread(2)
```

## 0.6 Testing culture (why the project feels different)

- Three independent test harnesses: the public TCL suite (`test/*.test`), **TH3**
  (proprietary, used for 100% MC/DC branch coverage on embedded builds), and
  **SQL Logic Test** (cross-checks results against PostgreSQL/MySQL).
- Continuous fuzzing: `fuzzcheck`, `dbsqlfuzz`, OSS-Fuzz. Fuzzers mutate both SQL *and*
  the database file, so *every* byte of Phase 1's format has an adversarial test.
- Anomaly testing: crash simulation VFS, out-of-memory simulation, I/O-error injection at
  every single I/O call site.

Practical meaning for you: a patch is judged by whether it breaks any of ~tens of millions
of test cases, and whether it keeps 100% branch coverage. "It works on my machine" is not
a contribution.

## 0.7 Reality check on "contributing to SQLite"

Be clear-eyed, because it shapes your strategy:

- SQLite is **public domain and deliberately closed to outside contributions** in the
  normal open-source sense. There is no pull-request workflow; GitHub is a read-only
  mirror. The core team accepts bug reports enthusiastically and code rarely.
- High-value paths that *do* work:
  1. **Bug reports with a minimal reproducer** (especially fuzzer-found corruption or
     wrong-answer bugs) — these are genuinely welcomed on the `sqlite-users` /
     `sqlite-forum`.
  2. **Extensions**: virtual tables, custom functions, VFS shims, loadable modules.
  3. **Forks that do take PRs**: `libsql`, `sqlite-wasm` ecosystems, `rusqlite`,
     `better-sqlite3`, DuckDB (different engine, same ethos), `limbo`/`turso`.
  4. **Other DBs**: the skills here transfer directly to Postgres, MySQL/InnoDB, DuckDB,
     RocksDB, where contribution is fully open.
- So the plan: learn SQLite's internals as the *reference implementation*, then contribute
  where patches land. Phase 8 covers the mechanics of both routes.

## 0.8 Vocabulary to fix now

| Term | Meaning in SQLite specifically |
|---|---|
| page | fixed-size block, 512…65536 bytes, unit of I/O and caching |
| usable size | page size minus `reserved bytes` (header byte 20); all format math uses this |
| rowid | 64-bit signed integer key of a table b-tree |
| record / payload | the serialized row (header of serial types + values) |
| cell | one entry inside a b-tree page: key + payload (+ child pointer on interior pages) |
| cursor | position in a b-tree; VDBE opcodes operate through cursors |
| varint | 1–9 byte big-endian base-128 integer |
| affinity | a column's type preference applied on store/compare |
| LogEst | 10*log2(x), SQLite's fixed-point cost/row-count unit |
| hot journal | a rollback journal left behind by a crash; next opener must replay it |
