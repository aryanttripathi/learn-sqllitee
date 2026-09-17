# Phase 0 — Code Walkthrough: The Spine of the Library

**Read `../READING-THE-CODE.md` first.** This document applies that method to the three
public API functions every SQLite program calls, so you can see where the layers begin.

Source links (always cite these, never amalgamation line numbers):
- [`src/main.c`](https://github.com/sqlite/sqlite/blob/master/src/main.c) — connection lifecycle
- [`src/prepare.c`](https://github.com/sqlite/sqlite/blob/master/src/prepare.c) — compile SQL
- [`src/vdbeapi.c`](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c) — step/bind/column

---

## Mental model for this phase

> **SQLite's public API is a thin, paranoid shell around three objects:**
> `sqlite3` (connection), `Vdbe` (prepared statement), `Mem` (value).
> Every `sqlite3_*` function does the same four things: check the arguments, take the
> mutex, call one internal `sqlite3Xxx()` function, translate the result code.

Once you internalize that shape, the entire API surface becomes skimmable, and you can
spend your attention on the internals where the real work happens.

---

## 1. `sqlite3_step()` — the whole control flow in 20 useful lines

Real code, `src/vdbeapi.c`
([source](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c)):

```c
SQLITE_API int sqlite3_step(sqlite3_stmt *pStmt){
  int rc = SQLITE_OK;      /* Result from sqlite3Step() */
  Vdbe *v = (Vdbe*)pStmt;  /* the prepared statement */
  int cnt = 0;             /* Counter to prevent infinite loop of reprepares */
  sqlite3 *db;             /* The database connection */

  if( vdbeSafetyNotNull(v) ){
    return SQLITE_MISUSE_BKPT;
  }
  db = v->db;
  sqlite3_mutex_enter(db->mutex);
  while( (rc = sqlite3Step(v))==SQLITE_SCHEMA
         && cnt++ < SQLITE_MAX_SCHEMA_RETRY ){
    int savedPc = v->pc;
    rc = sqlite3Reprepare(v);
    if( rc!=SQLITE_OK ){
      /* ... copy the compiler's error message into the statement ... */
      break;
    }
    sqlite3_reset(pStmt);
    if( savedPc>=0 ){
      v->minWriteFileFormat = 254;
    }
    assert( v->expired==0 );
  }
  sqlite3_mutex_leave(db->mutex);
  return rc;
}
```

Line by line:

| Line | What it means |
|---|---|
| `Vdbe *v = (Vdbe*)pStmt;` | **`sqlite3_stmt` *is* a `Vdbe`.** The public type is an opaque alias for the bytecode program. This one cast is the whole "API vs internals" boundary. |
| `vdbeSafetyNotNull(v)` | argument paranoia: NULL or already-finalized statement ⇒ `SQLITE_MISUSE`. Every public function starts like this. |
| `sqlite3_mutex_enter(db->mutex)` | serialize access to the connection. In `SQLITE_THREADSAFE=0` builds this compiles to nothing. |
| `while( (rc = sqlite3Step(v))==SQLITE_SCHEMA ...)` | **the real work is `sqlite3Step(v)`** — one call. Everything around it is the schema-change retry loop. |
| `sqlite3Reprepare(v)` | another connection ran DDL, so the cached plan is stale: recompile the original SQL and try again. |
| `cnt++ < SQLITE_MAX_SCHEMA_RETRY` | bounded retry, so a connection spinning on DDL cannot hang you forever. |
| `v->minWriteFileFormat = 254;` | a flag smuggled through a struct field so `OP_Init` knows not to re-fire the trace callback on the retry. The comment tags it `tag-20220401a` — SQLite's way of cross-referencing a subtle interaction from another file. |

**What to take from this:** `sqlite3_step()` contains no query logic at all. It is
mutex + retry. That is the shape of the entire public API.

Follow it down:
```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400
sed -n '/^static int sqlite3Step(Vdbe \*p){/,/^}/p' $A/sqlite3.c
```
`sqlite3Step()` checks statement state, opens a transaction if needed, and calls
`sqlite3VdbeExec(p)` — the interpreter, which is Phase 5.

---

## 2. `sqlite3LockAndPrepare()` — why compiling can happen twice

Real code, `src/prepare.c`
([source](https://github.com/sqlite/sqlite/blob/master/src/prepare.c)):

```c
static int sqlite3LockAndPrepare(
  sqlite3 *db,              /* Database handle. */
  const char *zSql,         /* UTF-8 encoded SQL statement. */
  int nBytes,               /* Length of zSql in bytes. */
  u32 prepFlags,            /* Zero or more SQLITE_PREPARE_* flags */
  Vdbe *pOld,               /* VM being reprepared */
  sqlite3_stmt **ppStmt,    /* OUT: A pointer to the prepared statement */
  const char **pzTail       /* OUT: End of parsed string */
){
  int rc;
  int cnt = 0;

  *ppStmt = 0;
  if( !sqlite3SafetyCheckOk(db)||zSql==0 ){
    return SQLITE_MISUSE_BKPT;
  }
  sqlite3_mutex_enter(db->mutex);
  sqlite3BtreeEnterAll(db);
  do{
    /* Make multiple attempts to compile the SQL, until it either succeeds
    ** or encounters a permanent error.  A schema problem after one schema
    ** reset is considered a permanent error. */
    rc = sqlite3Prepare(db, zSql, nBytes, prepFlags, pOld, ppStmt, pzTail);
    assert( rc==SQLITE_OK || *ppStmt==0 );
    if( rc==SQLITE_OK || db->mallocFailed ) break;
  }while( (rc==SQLITE_ERROR_RETRY && (cnt++)<SQLITE_MAX_PREPARE_RETRY)
       || (rc==SQLITE_SCHEMA && (sqlite3ResetOneSchema(db,-1), cnt++)==0) );
  sqlite3BtreeLeaveAll(db);
  rc = sqlite3ApiExit(db, rc);
  db->busyHandler.nBusy = 0;
  sqlite3_mutex_leave(db->mutex);
  return rc;
}
```

The interesting lines:

- `sqlite3BtreeEnterAll(db)` — takes the b-tree mutex for **every** attached database.
  Compiling needs the schema, and the schema lives inside `BtShared`. Note the layering:
  the *compiler* must lock the *storage* layer just to read table definitions.
- The `do{...}while` condition is the densest line in the file:
  ```c
  (rc==SQLITE_ERROR_RETRY && (cnt++)<SQLITE_MAX_PREPARE_RETRY)
  || (rc==SQLITE_SCHEMA && (sqlite3ResetOneSchema(db,-1), cnt++)==0)
  ```
  Read the second clause carefully: the comma operator calls `sqlite3ResetOneSchema()`
  **for its side effect** (throw away the cached schema), then evaluates `cnt++==0`, which
  is true exactly once. Translation: *on a schema error, reload the schema and retry — but
  only once.*
- `sqlite3ApiExit(db, rc)` — the universal exit filter for public functions: converts an
  OOM flag into `SQLITE_NOMEM`, applies `db->errMask`. Every `sqlite3_*` return passes
  through it.

**Take-away:** preparing a statement is not a pure function of the SQL text. It depends on
the schema, which can change under you — which is why `SQLITE_SCHEMA` exists and why
`prepare_v2` hides it.

---

## 3. The spine, as a call chain you can breakpoint

```
sqlite3_open_v2()          main.c        → openDatabase() → sqlite3BtreeOpen()
                                                          → sqlite3PagerOpen()
sqlite3_prepare_v2()       prepare.c     → sqlite3LockAndPrepare()
                                          → sqlite3Prepare()
                                            → sqlite3RunParser()        (Phase 4)
                                              → sqlite3Select()         (Phase 5/6)
                                                → sqlite3WhereBegin()   (Phase 6)
sqlite3_bind_int()         vdbeapi.c     → writes v->aVar[i]
sqlite3_step()             vdbeapi.c     → sqlite3Step() → sqlite3VdbeExec()   (Phase 5)
                                            → sqlite3BtreeNext()        (Phase 3)
                                              → sqlite3PagerGet()       (Phase 2)
                                                → unixRead()            (Phase 2)
sqlite3_column_text()      vdbeapi.c     → reads v->pResultRow[i]
sqlite3_finalize()         vdbeapi.c     → sqlite3VdbeFinalize()
```

Verify it yourself — this is Lab 0.5:
```
(lldb) b unixRead
(lldb) run
sqlite> SELECT name FROM users WHERE age>40;
(lldb) bt
```

Each of the eight frames you see is the entry point of one phase of this course.

---

## 4. Reading `main.c`'s `openDatabase()` — the parts that matter

`openDatabase()` is ~300 lines, mostly flag validation. Do **not** read it linearly.
[Source](https://github.com/sqlite/sqlite/blob/master/src/main.c) — search for
`static int openDatabase(`. The four things it actually does:

1. `sqlite3MallocZero(sizeof(sqlite3))` — allocate the connection.
2. Install built-in functions and collations (`sqlite3RegisterPerConnectionBuiltinFunctions`,
   `createCollation(db, sqlite3StrBINARY, ...)`).
3. `sqlite3BtreeOpen(db->pVfs, zOpen, db, &db->aDb[0].pBt, 0, flags)` — **the one call
   that touches storage.** Everything above this line is setup.
4. Set up `db->aDb[]`: slot 0 is `"main"`, slot 1 is `"temp"`; `ATTACH` appends more.

That `aDb[]` array is worth remembering: whenever you see `iDb` in the codebase (and you
will, constantly — `OP_Transaction P1`, `Parse.iDb`, `sqlite3SchemaGet(db, iDb)`), it is an
index into it.

```c
struct Db {
  char *zDbSName;      /* "main", "temp", or the ATTACH name */
  Btree *pBt;          /* the b-tree handle for this database file */
  u8 safety_level;     /* PRAGMA synchronous */
  u8 bSyncSet;
  Schema *pSchema;     /* the in-memory schema (Phase 4) */
};
```

---

## 5. Exercise: map the API to the layers

For each function, find its implementation and name the *one* internal function it
delegates to. Put the answers in `labs/phase00/api-map.md`.

```
sqlite3_open_v2       sqlite3_bind_blob      sqlite3_column_count
sqlite3_exec          sqlite3_reset          sqlite3_last_insert_rowid
sqlite3_changes       sqlite3_interrupt      sqlite3_db_filename
```

```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400
sed -n '/^SQLITE_API int sqlite3_reset(/,/^}/p' $A/sqlite3.c
```

You should discover the pattern holds every time: **validate → mutex → one internal call →
`sqlite3ApiExit`**. That is why the public API has never broken backwards compatibility —
there is almost nothing in it to break.
