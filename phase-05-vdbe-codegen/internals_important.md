# Phase 5 — Internals Reference: VDBE & Code Generation

## Files

| File | Contents |
|---|---|
| `vdbe.c` | `sqlite3VdbeExec()` — the interpreter; the `/* Opcode: */` doc comments are compiled inputs |
| `vdbeaux.c` | program assembly: `sqlite3VdbeAddOp*`, labels, `sqlite3VdbeMakeReady`, `sqlite3VdbeHalt`, EXPLAIN output, commit logic |
| `vdbeapi.c` | `sqlite3_step/bind/column/reset/finalize`, `sqlite3_result_*`, `sqlite3_value_*` |
| `vdbemem.c` | `Mem` manipulation: `sqlite3VdbeMemSetStr`, `MemSetInt64`, `MemStringify`, `MemNumerify`, `MemCast` |
| `vdbesort.c` | external merge sorter used by `SorterOpen`/`SorterInsert`/`SorterSort` |
| `vdbeblob.c` | `sqlite3_blob_open/read/write` — incremental BLOB I/O |
| `vdbetrace.c` | `sqlite3_expanded_sql()` / trace callbacks |
| `vdbeInt.h` | `Vdbe`, `Mem`, `VdbeCursor`, `VdbeFrame`, `AuxData` |
| `expr.c` | expression codegen: `sqlite3ExprCode*`, `sqlite3ExprIfTrue/False` |
| `select.c` | `sqlite3Select`, `selectInnerLoop`, `generateSortTail`, aggregate codegen |
| `insert.c`/`update.c`/`delete.c` | DML codegen, `sqlite3GenerateConstraintChecks` |
| `trigger.c` | `SubProgram` generation, `OP_Program` |
| `opcodes.h`/`opcodes.c` | **generated** from `vdbe.c` comments |

## Structures

```c
struct Vdbe {
  sqlite3 *db;
  Vdbe *pPrev, *pNext;
  Parse *pParse;
  int nVar, nMem, nCursor, nOp;
  Mem *aMem;            /* registers, 1-based */
  Mem **apArg;
  Mem *aColName, *pResultRow;
  VdbeCursor **apCsr;
  Mem *aVar; char **azVar;
  VdbeOp *aOp;
  int pc, rc, nChange;
  u32 cacheCtr, expmask;
  u8 errorAction, minWriteFileFormat, prepFlags;
  bft explain:2, changeCntOn:1, expired:2, runOnlyOnce:1, usesStmtJournal:1,
      readOnly:1, bIsReader:1;
  VdbeFrame *pFrame, *pDelFrame;
  SubProgram *pProgram;
  ...
};

struct VdbeCursor {
  u8 eCurType;          /* CURTYPE_BTREE / SORTER / VTAB / PSEUDO */
  i8 iDb;
  u8 nullRow, deferredMoveto, isTable, isEphemeral, useRandomRowid;
  Bool cacheStatus;     /* CACHE_STALE means aType/aOffset must be rebuilt */
  i16 seekHit;
  union { BtCursor *pCursor; sqlite3_vtab_cursor *pVCur; VdbeSorter *pSorter; } uc;
  KeyInfo *pKeyInfo;
  u32 *aOffset;         /* cached offsets of each column in the record */
  u32 aType[1];         /* cached serial types (flexible array) */
  const u8 *aRow;       /* pointer into the page, when the whole row is local */
  u32 payloadSize, szRow;
  i64 movetoTarget;     /* for deferredMoveto */
  ...
};

struct VdbeFrame {       /* saved state for OP_Program (triggers, sub-selects) */
  Vdbe *v; VdbeFrame *pParent;
  Mem *aMem; VdbeCursor **apCsr; VdbeOp *aOp;
  int pc, nOp, nMem, nCursor, nChildMem, nChildCsr;
  i64 nChange; ...
};
```

## `Mem.flags` cheat sheet
```
MEM_Null MEM_Str MEM_Int MEM_Real MEM_Blob MEM_IntReal MEM_AffMask
MEM_FromBind MEM_Undefined MEM_Cleared MEM_TypeMask
MEM_Term    (string is NUL-terminated)
MEM_Dyn     (z owns memory freed by xDel)
MEM_Static  (z points to static storage)
MEM_Ephem   (z points to memory owned by someone else — must be copied to persist)
MEM_Agg     (holds an aggregate accumulator)
MEM_Zero    (blob of u.nZero zero bytes, not materialized)
```
`MEM_Ephem` is the source of most VDBE memory bugs: an ephemeral value must be
`sqlite3VdbeMemMakeWriteable()`d before it can outlive the cursor position.

## Program-building API (`vdbeaux.c`)
```c
int  sqlite3VdbeAddOp0/1/2/3/4/4Int(Vdbe*, int op, ...);
int  sqlite3VdbeMakeLabel(Parse*);
void sqlite3VdbeResolveLabel(Vdbe*, int x);
void sqlite3VdbeJumpHere(Vdbe*, int addr);
void sqlite3VdbeJumpHereOrPopInst(Vdbe*, int addr);
void sqlite3VdbeChangeP1/P2/P3/P5(Vdbe*, int addr, int val);
void sqlite3VdbeChangeP4(Vdbe*, int addr, const char *z, int n);
void sqlite3VdbeAppendP4(Vdbe*, void *p, int p4type);
int  sqlite3VdbeCurrentAddr(Vdbe*);
void sqlite3VdbeComment(Vdbe*, const char *zFmt, ...);   /* VdbeComment((v,...)) macro */
void sqlite3VdbeNoopComment(...);
void sqlite3VdbeVerifyAbortable(Vdbe*, int);
int  sqlite3VdbeAddFunctionCall(Parse*, int, int, int, int, const FuncDef*, int);
```
P4 types: `P4_NOTUSED P4_TRANSIENT P4_STATIC P4_COLLSEQ P4_INT32 P4_INT64 P4_REAL
P4_SUBPROGRAM P4_ADVANCE P4_TABLE P4_KEYINFO P4_EXPR P4_MEM P4_VTAB P4_FUNCDEF
P4_FUNCCTX P4_DYNAMIC P4_INTARRAY`.

## Expression codegen API (`expr.c`)
```c
int  sqlite3ExprCodeTarget(Parse*, Expr*, int target);   /* returns the register used */
void sqlite3ExprCode(Parse*, Expr*, int target);
int  sqlite3ExprCodeTemp(Parse*, Expr*, int *pReg);
void sqlite3ExprIfTrue(Parse*, Expr*, int dest, int jumpIfNull);
void sqlite3ExprIfFalse(Parse*, Expr*, int dest, int jumpIfNull);
int  sqlite3ExprCodeExprList(Parse*, ExprList*, int target, int srcReg, u8 flags);
int  sqlite3ExprCodeRunJustOnce(Parse*, Expr*, int regDest);   /* constant factoring */
void sqlite3ExprCodeMove(Parse*, int iFrom, int iTo, int nReg);
int  sqlite3GetTempReg(Parse*); void sqlite3ReleaseTempReg(Parse*, int);
int  sqlite3GetTempRange(Parse*, int); void sqlite3ReleaseTempRange(Parse*, int, int);
char sqlite3ExprAffinity(Expr*);
CollSeq *sqlite3ExprCollSeq(Parse*, Expr*);
```

## SELECT destinations (`SelectDest.eDest`)
```
SRT_Output     send rows to the caller (OP_ResultRow)
SRT_Mem        store a single value in a register (scalar subquery)
SRT_Set        build an ephemeral index (for IN (...))
SRT_Table      store in an ephemeral table
SRT_EphemTab   create + fill an ephemeral table
SRT_Coroutine  yield rows to a coroutine consumer
SRT_Except / SRT_Union / SRT_Fifo / SRT_DistFifo / SRT_Queue / SRT_DistQueue
SRT_Exists     set a register to 1 if any row
SRT_Discard    throw rows away (used by `SELECT` inside triggers)
SRT_Upfrom     UPDATE ... FROM
```
Reading `selectInnerLoop()`'s `switch(eDest)` is the fastest way to understand how the same
query body serves 12 different consumers.

## Key opcodes with exact semantics

```
Init      P1 P2 * P4 *      jump to P2 (the prologue trick); P4 is the SQL comment
Goto      * P2 * * *        pc = P2
Gosub     P1 P2 * * *       r[P1] = return address; pc = P2
Return    P1 * * * *        pc = r[P1]
Halt      P1 P2 * P4 P5     end statement; P1 = result code, P2 = ON CONFLICT action
Transaction P1 P2 P3 P4 P5  start a read (P2=0) or write (P2=1) txn on db P1;
                            P3 = expected schema cookie (SQLITE_SCHEMA if mismatched)
OpenRead  P1 P2 P3 P4 P5    cursor P1 on root page P2 of db P3; P4 = KeyInfo or column count
OpenWrite same, for writing
OpenEphemeral P1 P2 * P4 P5 transient table/index in a temp file or memory
Rewind    P1 P2 * * *       move cursor P1 to the first entry; jump P2 if empty
Next      P1 P2 P3 P4 P5    advance cursor P1; jump back to P2 if a row exists
Column    P1 P2 P3 P4 P5    r[P3] = column P2 of the row at cursor P1
Rowid     P1 P2 * * *       r[P2] = rowid of cursor P1
SeekRowid P1 P2 P3 * *      position cursor P1 at rowid r[P3]; jump P2 if not found
NotExists P1 P2 P3 * *      like SeekRowid, but asserts the table is a rowid table
SeekGE/GT/LE/LT P1 P2 P3 P4 seek index cursor P1 using the P4 values starting at r[P3]
IdxGT/GE/LT/LE  P1 P2 P3 P4 compare the current index key against r[P3..] and jump
IdxRowid  P1 P2 * * *       r[P2] = rowid stored in the index entry at cursor P1
IdxInsert P1 P2 P3 P4 P5    insert record r[P2] into index cursor P1
MakeRecord P1 P2 P3 P4 P5   r[P3] = record built from r[P1..P1+P2-1]; P4 = affinity string
Insert    P1 P2 P3 P4 P5    write record r[P2] with rowid r[P3] into table cursor P1
Delete    P1 P2 P3 P4 P5    delete the row at cursor P1
ResultRow P1 P2 * * *       return r[P1..P1+P2-1] to the caller (sqlite3_step → SQLITE_ROW)
AggStep   P1 P2 P3 P4 P5    call the step function of P4 with r[P2..]; accumulator r[P3]
AggFinal  P1 P2 * P4 *      finalize accumulator r[P1]
Function  P1 P2 P3 P4 P5    r[P3] = P4(r[P2..P2+P5-1]); PureFunc for deterministic funcs
Compare   P1 P2 P3 P4 P5    compare r[P1..] with r[P2..], P3 values, using KeyInfo P4
Jump      P1 P2 P3 * *      three-way branch on the last OP_Compare result (<, ==, >)
Once      P1 P2 * * *       run the following block only on the first pass
DeferredSeek P1 P2 P3 * *   remember that table cursor P3 must be moved to the rowid of
                            index cursor P1 — but only if a Column actually needs it
Program   P1 P2 P3 P4 P5    run the SubProgram in P4 (trigger) in a new VdbeFrame
Param     P1 P2 * * *       read a value from the parent frame (OLD./NEW. columns)
```

## Opcode property flags (generated into `opcodes.h`)
```
OPFLG_JUMP   P2 is a jump target
OPFLG_IN1/IN2/IN3    P1/P2/P3 are input registers
OPFLG_OUT2/OUT3      P2/P3 are output registers
OPFLG_NCYCLE         instruction is counted by scanstatus cycle profiling
```
These come from the `/* out2 */`, `/* jump, in1, in3 */` comments after each `case` label —
`mkopcodeh.tcl` parses them. Getting them wrong triggers assertion failures in debug builds.

## Statement lifecycle details

- `sqlite3VdbeMakeReady()` allocates `aMem`, `apCsr`, `aVar` in **one** allocation carved
  from the same block as the ops — fewer mallocs per statement.
- `sqlite3_step()` on a statement that returned `SQLITE_DONE` auto-resets (since 3.7.0).
- `sqlite3_reset()` returns the error code of the most recent run.
- A statement holds a read transaction open between `step()` calls. Long-lived open
  statements are the #1 cause of "database is locked" in applications.
- `expmask` tracks which bound parameters appear in places where re-binding requires a
  re-prepare (`sqlite3_bind` on a parameter used in a LIMIT, etc. sets `expired`).

## Gotchas

1. Opcode **numbers** change between versions; always use names.
2. `EXPLAIN` does not execute; `EXPLAIN QUERY PLAN` does not execute either.
3. `OP_ResultRow` registers are only valid until the next `sqlite3_step()`.
   `sqlite3_column_text()` pointers are invalidated by the next step/reset/finalize.
4. `OP_Column` result may be `MEM_Ephem` pointing into the page buffer. Anything that
   stores it must copy.
5. `VdbeCursor.cacheStatus` must be invalidated (`CACHE_STALE`) whenever the cursor moves,
   or `OP_Column` will read stale offsets — a classic patch bug.
6. Adding an opcode requires: the `case` + doc comment in `vdbe.c`, regenerating
   `opcodes.h`/`opcodes.c`, and correct `/* inN outN jump */` annotations.
7. `sqlite3VdbeAddOp4(..., P4_KEYINFO, ...)` takes ownership of the KeyInfo reference.
8. Subprograms (`OP_Program`) recurse with `SQLITE_MAX_TRIGGER_DEPTH`; unbounded trigger
   recursion is a resource-exhaustion bug class.
9. `sqlite3_expanded_sql()` (vdbetrace.c) substitutes bound values into the SQL text — great
   for logging, but it is a *reconstruction*, not what was executed.
10. In debug builds, `sqlite3VdbeAssertAbortable()` checks that any opcode which can abort
    is preceded by a statement journal — a real invariant for constraint handling.
