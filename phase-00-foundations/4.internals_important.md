# Phase 0 — Internals Reference: Source Map & Build System

## Source tree at a glance (`src/`)

### Front end
| File | Contents |
|---|---|
| `tokenize.c` | `sqlite3GetToken()`, `sqlite3RunParser()`; hand-written lexer |
| `keywordhash.h` | *generated* perfect hash of SQL keywords |
| `parse.y` | the Lemon grammar (the real SQL definition) |
| `parse.c` / `parse.h` | *generated* LALR(1) parser |
| `resolve.c` | name resolution: identifiers → tables/columns/aggregates |
| `build.c` | CREATE/DROP TABLE/INDEX, schema object construction, `sqlite3StartTable()` |
| `alter.c`, `attach.c`, `pragma.c`, `analyze.c`, `vacuum.c`, `trigger.c`, `fkey.c` | statement-specific front ends |
| `expr.c` | `Expr` tree building **and** expression code generation |
| `walker.c` | generic tree walker used by resolve/optimizer |
| `treeview.c` | `sqlite3TreeViewExpr()` etc. — debug tree printing (huge learning aid) |

### Planner + codegen
| File | Contents |
|---|---|
| `select.c` | `sqlite3Select()`, subquery flattening, aggregates, compound selects, DISTINCT |
| `insert.c`, `update.c`, `delete.c`, `upsert.c` | DML codegen, constraint checking |
| `where.c` | NGQP entry: `sqlite3WhereBegin()`, loop/path solver |
| `whereexpr.c` | WHERE-clause analysis into `WhereTerm`s |
| `wherecode.c` | emits the bytecode for one `WhereLoop` |
| `window.c` | window functions |

### Execution
| File | Contents |
|---|---|
| `vdbe.c` | `sqlite3VdbeExec()` — the interpreter `switch` |
| `vdbeaux.c` | program assembly, `sqlite3VdbeMakeReady()`, `Explain` output, commit logic |
| `vdbeapi.c` | `sqlite3_step/column/bind` |
| `vdbemem.c` | `Mem` (a.k.a. `sqlite3_value`) manipulation |
| `vdbesort.c` | external merge sort for ORDER BY / CREATE INDEX |
| `vdbeblob.c` | incremental BLOB I/O |
| `vdbeInt.h` | `Vdbe`, `Mem`, `VdbeCursor`, `VdbeFrame` definitions |

### Storage
| File | Contents |
|---|---|
| `btree.c` / `btreeInt.h` | b-tree, cursors, balancing, file-format writer/reader |
| `pager.c` | ACID state machine, journal, page cache glue |
| `wal.c` | write-ahead log |
| `pcache.c` / `pcache1.c` | page cache interface / default implementation |
| `os.c`, `os_unix.c`, `os_win.c`, `os_kv.c` | VFS implementations |
| `backup.c`, `journal.c`, `memjournal.c` | online backup, journal file abstraction |

### Infrastructure
`main.c` (connection lifecycle), `prepare.c` (schema load + `sqlite3_prepare`),
`malloc.c` + `mem1..5.c` (allocators), `mutex*.c`, `hash.c`, `bitvec.c` (sparse page-number
set), `util.c` (varints, `sqlite3Atoi64`, error handling), `printf.c`, `random.c`,
`status.c`, `global.c`, `ctime.c` (compile options), `loadext.c`, `vtab.c`, `func.c`,
`date.c`, `callback.c`, `table.c`, `rowset.c`, `threads.c`, `legacy.c`.

### Extensions (`ext/`)
`fts5/`, `fts3/`, `rtree/`, `misc/` (json1 pre-3.38, `carray`, `csv`, `series`, `regexp`,
`uuid`…), `session/` (changesets), `rbu/`, `expert/` (index advisor), `lsm1/`, `userauth/`.

## Key headers to read once, early
| Header | Why |
|---|---|
| `sqliteInt.h` | ~5k lines; defines `sqlite3`, `Parse`, `Expr`, `Select`, `Table`, `Index`, `SrcList`, `FuncDef`, every flag |
| `btreeInt.h` | `MemPage`, `BtShared`, `BtCursor`, `CellInfo`, file-format constants |
| `vdbeInt.h` | `Vdbe`, `Op`, `Mem`, `VdbeCursor` |
| `pagerInt.h`(in `pager.c`) | pager state machine comments — one of the best comments in the codebase |
| `wal.c` header comment | the WAL design doc lives in the source |
| `os.h`, `sqlite3.h` (`sqlite3_io_methods`) | the VFS contract |

## Build targets you will use
```sh
make sqlite3            # CLI shell
make sqlite3.c          # amalgamation
make testfixture        # TCL test harness  (./testfixture ../test/x.test)
make fuzzcheck          # fuzz corpus runner
make sqlite3_analyzer   # space usage analyzer (a testfixture variant)
make showdb             # tool/showdb.c: page-level database dumper   <-- Phase 1 gold
make showwal            # tool/showwal.c: WAL dumper                  <-- Phase 7 gold
make showjournal        # rollback journal dumper                     <-- Phase 2 gold
make dbtotxt            # hexdump-to-text used in bug reports
```
If a target is missing in your tree: `cc -I. -Isrc -o showdb tool/showdb.c` usually works,
since these tools are standalone.

## Compile-time switches worth knowing now
| Macro | Effect |
|---|---|
| `SQLITE_DEBUG` | enables `assert()`, `NEVER/ALWAYS`, internal consistency checks, trace hooks |
| `SQLITE_ENABLE_EXPLAIN_COMMENTS` | comment column in `EXPLAIN` output |
| `SQLITE_ENABLE_SELECTTRACE` / `WHERETRACE` | `.selecttrace 0xffff`, `.wheretrace 0xfff` in the shell |
| `SQLITE_TEST` | builds test-only APIs used by the TCL suite |
| `SQLITE_OMIT_*` | ~60 feature-removal switches (shrinks build) |
| `SQLITE_DEFAULT_PAGE_SIZE` | default 4096 |
| `SQLITE_MAX_PAGE_COUNT` | default 1073741823 pages |
| `SQLITE_THREADSAFE` | 0/1/2 → mutex strategy |
| `SQLITE_ENABLE_STMT_SCANSTATUS` | per-loop row counts for profiling |
| `SQLITE_DIRECT_OVERFLOW_READ`, `SQLITE_ENABLE_SORTER_REFERENCES` | perf tweaks |

## Debug tricks (debug builds only)
```
sqlite> .selecttrace 0xffff      -- dump Select tree transformations
sqlite> .wheretrace 0xfff        -- dump WhereLoop/WherePath cost search
sqlite> .treetrace 0xffff        -- newer name for selecttrace
sqlite> PRAGMA vdbe_trace=ON;    -- print each opcode as it executes
sqlite> PRAGMA vdbe_listing=ON;  -- print program before running
sqlite> PRAGMA vdbe_addoptrace=ON;
```
In C: `sqlite3TreeViewExpr(0, pExpr, 0)` and `sqlite3TreeViewSelect(0, p, 0)` called from
the debugger print readable trees. `sqlite3VdbePrintOp()` prints one instruction.

## Invariants/gotchas that already matter
1. **Never edit `sqlite3.c`, `parse.c`, `opcodes.h`, `keywordhash.h`** — generated.
2. Opcode *numbers* are unstable across versions; opcode *names* are stable-ish.
3. `assert()` is compiled out in release; SQLite uses it as documentation of invariants.
   Read asserts as spec, not as defensive code.
4. `NEVER(x)` / `ALWAYS(x)` mark branches that must be unreachable — they exist for
   coverage measurement, not for runtime safety.
5. The library is single-file-format-compatible **in both directions since 2004**. Any
   change you propose to the format is effectively rejected by default.
6. Memory: SQLite never calls `malloc` directly; everything goes through `sqlite3Malloc()`
   / `sqlite3DbMalloc()` (the latter uses the connection's lookaside pool).
7. Error handling convention: functions return `int rc` (`SQLITE_OK`, `SQLITE_ERROR`,
   `SQLITE_NOMEM`, …) and use `goto` cleanup labels. No exceptions, no early `return`
   past allocated resources.

## Naming conventions
| Prefix | Meaning |
|---|---|
| `sqlite3Xxx()` | internal API, visible across files |
| `sqlite3_xxx()` | **public** API — stability contract forever |
| `xxxUnused` / `UNUSED_PARAMETER(x)` | silence warnings portably |
| `pX` | pointer, `nX` count, `iX` index/integer, `zX` zero-terminated string, `eX` enum, `aX` array, `bX` boolean, `uX` unsigned |

That Hungarian-ish scheme is used with total discipline; once you know it you can read a
function signature and know its data flow before reading the body.
