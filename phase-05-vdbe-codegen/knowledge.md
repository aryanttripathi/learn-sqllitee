# Phase 5 — Code Generation and the VDBE

Every SQL statement becomes a small program for a purpose-built virtual machine. Once you
can read that program, SQLite stops being magic: `EXPLAIN` *is* the source of truth about
what your query does.

---

## 5.1 The machine model

```
 ┌───────────────────────────────────────────────────────────────────────┐
 │  Vdbe  (one prepared statement)                                       │
 │                                                                       │
 │  aOp[]    : the program — array of Op { opcode, p1, p2, p3, p4, p5 }  │
 │  aMem[]   : REGISTERS — array of Mem (a.k.a. sqlite3_value)           │
 │  apCsr[]  : CURSORS — VdbeCursor*, each wrapping a BtCursor           │
 │  pc       : program counter                                           │
 │  aVar[]   : bound parameters                                          │
 │  pFrame   : stack of VdbeFrame for triggers/subprograms               │
 └───────────────────────────────────────────────────────────────────────┘
```

Key design points:

- **It is a register machine, not a stack machine** (it was a stack machine until 2008;
  registers made the generated code shorter and faster).
- Registers are 1-based; `r[0]` is unused. Register count is `Parse.nMem`.
- Every register is a `Mem`, i.e. a full dynamically typed SQL value.
- Cursors are numbered from 0 and correspond exactly to `SrcItem.iCursor` from Phase 4.
- Control flow is by absolute jump to an instruction address.
- The whole interpreter is one function: `sqlite3VdbeExec()` — a `switch(pOp->opcode)`
  inside a `for(;;)` loop, with `computed goto` on compilers that support it.

```c
struct VdbeOp {
  u8 opcode;
  signed char p4type;   /* one of the P4_xxx constants */
  u16 p5;               /* flags */
  int p1, p2, p3;       /* operands: register numbers, cursor numbers, jump targets */
  union p4union {
    int i; void *p; char *z; i64 *pI64; double *pReal;
    FuncDef *pFunc; sqlite3_context *pCtx; CollSeq *pColl;
    Mem *pMem; sqlite3_vtab *pVtab; KeyInfo *pKeyInfo; u32 *ai;
    SubProgram *pProgram; Table *pTab; ...
  } p4;
#ifdef SQLITE_ENABLE_EXPLAIN_COMMENTS
  char *zComment;
#endif
};
```

`struct Mem` (`vdbeInt.h`) — the value type:
```c
struct sqlite3_value {          /* == Mem */
  union MemValue { double r; i64 i; int nZero; const char *zPType; FuncDef *pDef; } u;
  u16 flags;        /* MEM_Null, MEM_Str, MEM_Int, MEM_Real, MEM_Blob, MEM_Agg,
                       MEM_Dyn, MEM_Static, MEM_Ephem, MEM_Zero, MEM_Cleared, ... */
  u8 enc;           /* SQLITE_UTF8 / UTF16LE / UTF16BE */
  int n;            /* byte length of string/blob */
  char *z;          /* string/blob data */
  char *zMalloc;    /* the allocation backing z, if owned */
  sqlite3 *db;
  void (*xDel)(void*);
};
```
The `flags` field is the type tag; a value can be simultaneously `MEM_Int|MEM_Str` after a
cached conversion, which is how `typeof()` and comparisons stay fast.

---

## 5.2 The universal program shape

Every SQLite program looks like this:

```
 addr 0 : Init   0 <start> 0      ← jump to the PROLOGUE at the end
 addr 1 : ... the actual query ...
        : ResultRow
        : Halt
 addr N : Transaction / OpenRead / constant setup     ← PROLOGUE
 addr N+1: Goto 0 1                                    ← jump back to addr 1
```

Why the backwards arrangement? Because the prologue (which transactions to start, which
constants to load) is only fully known *after* the body has been generated. Rather than
patch or shift instructions, the code generator appends the prologue and jumps.

You saw this in Phase 0; now you know why.

---

## 5.3 Real programs, decoded

### A. Table scan with a filter — and a surprise

```sql
SELECT name FROM users WHERE age > 40;     -- index i_age ON users(age) exists
```
```
0   Init          0  10  0            Start at 10
1   OpenRead      0   2  0  3         root=2 iDb=0; users        ← cursor 0 = table
2   OpenRead      1   3  0  k(2,,)    root=3 iDb=0; i_age        ← cursor 1 = index
3   Integer      40   1  0            r[1]=40
4   SeekGT        1   9  1  1         key=r[1]                   ← seek index > 40
5     DeferredSeek  1  0  0           Move 0 to 1.rowid if needed ← LAZY table seek
6     Column        0  1  2           r[2]= cursor 0 column 1
7     ResultRow     2  1  0           output=r[2]
8   Next          1   5  0
9   Halt          0   0  0
10  Transaction   0   0  3  0     1
11  Goto          0   1  0
```

Three things to take away:

1. The planner chose the **index** even though the query looks like a scan.
2. `DeferredSeek` is an optimization: don't actually seek the table b-tree until some
   opcode needs a column from it. If the query had only needed `age` or `rowid`, the table
   seek would never happen (a covering-index-like effect).
3. `Next 1 5` — the loop increments the *index* cursor, not the table cursor.

### B. Primary-key lookup

```sql
SELECT name FROM users WHERE id = 2;
```
```
1   OpenRead     0  2  0  2      users
2   Integer      2  1  0         r[1]=2
3   SeekRowid    0  6  1         intkey=r[1]      ← direct b-tree descent, no loop
4   Column       0  1  2
5   ResultRow    2  1  0
6   Halt
```
No `Next`: at most one row can match, so there is no loop at all. This is the cheapest
possible plan.

### C. Index-only (covering) access

```sql
SELECT id FROM users WHERE age = 45;
```
```
1   OpenRead     1  3  0  k(2,,)   2      i_age   ← P5=2: OPFLAG_SEEKEQ hint
2   Integer     45  1  0
3   SeekGE       1  8  1  1
4     IdxGT        1  8  1  1              ← stop when key > 45
5     IdxRowid     1  2  0                 ← rowid comes from the INDEX entry
6     ResultRow    2  1  0
7   Next         1  4  1
8   Halt
```
**The `users` table is never opened.** The index contains `(age, rowid)`, which is
everything the query needs. That is a covering index, visible in the bytecode.

Note the `SeekGE` + `IdxGT` idiom: equality on an index is implemented as "seek to the
first key ≥ X, then stop as soon as a key > X appears".

### D. A join

```sql
SELECT u.name, o.total FROM users u JOIN orders o ON o.uid=u.id WHERE u.age>40;
```
```
1   OpenRead    1  4  0  3      orders     ← OUTER loop table
2   OpenRead    0  2  0  3      users      ← INNER, accessed by rowid
3   Rewind      1 13  0
4     Column      1  1  1       r[1]=o.uid
5     SeekRowid   0 12  1       intkey=r[1]         ← nested-loop join via PK
6     Column      0  2  2       r[2]=u.age
7     Le          3 12  2  BINARY-8  84   if r[2]<=r[3] goto 12
8     Column      0  1  4       r[4]=u.name
9     Column      1  2  5       r[5]=o.total
10    RealAffinity 5 0  0
11    ResultRow   4  2  0
12  Next        1  4  0
13  Halt
14  Transaction ...
15  Integer    40  3  0         r[3]=40       ← constant hoisted into the PROLOGUE
```

**This is what "nested loop join" means concretely**: an outer `Rewind`/`Next` loop, and an
inner `SeekRowid`. SQLite has no hash join and no merge join; every join is nested loops,
with indexes doing the work. (Bloom filters were added in 3.38 for large star-schema joins
— `OP_FilterAdd`/`OP_Filter` — but the loop structure is unchanged.)

Also note the constant `40` is loaded in the prologue, not inside the loop: the
**constant factoring** optimization (`sqlite3ExprCodeRunJustOnce`).

### E. GROUP BY with an aggregate

```sql
SELECT age, count(*) FROM users GROUP BY age;
```
The program (33 instructions) has this structure:

```
  prologue
  Gosub 4,29        → "reset accumulator" subroutine
  OpenRead 2 (i_age)          ← the index provides GROUP BY order for free!
  Rewind
  loop:
    Column   → r[6] = age of this row
    Compare  r[5] <-> r[6]    ← is this a new group?
    Jump     10,14,10
    Gosub 3,23   → "output one row" subroutine   (group changed)
    Move  r[6]→r[5]
    Gosub 4,29   → reset accumulator
    AggStep  count(0) → accum r[8]
    ...
  Next
  Gosub 3,23        → output final group
  Halt

  subroutine @23: IfPos r[1] ... AggFinal r[8] count → Copy → ResultRow → Return
  subroutine @29: Null r[7..8]; r[1]=0; Return
```

Three lessons:
1. **`OP_Gosub`/`OP_Return` give the code generator subroutines**, used for group-flush and
   accumulator-reset so the code is emitted once and called from two places.
2. Because an index on `age` exists, no sorter is needed — rows arrive in group order.
   Without the index you would see `SorterOpen`/`SorterInsert`/`SorterSort`/`SorterData`.
3. Aggregates are `AggStep` (per row) + `AggFinal` (per group), the same interface exposed
   to user-defined aggregate functions.

### F. INSERT with an index and a PK conflict check

```sql
INSERT INTO users VALUES(9,'x',1);
```
```
1   OpenWrite  0  2  0  3        users
2   OpenWrite  1  3  0  k(2,,)   i_age
3   SoftNull   2                 r[2]=NULL      ← id column: rowid alias stored as NULL
4   String8    0  3  0  'x'      r[3]='x'
5   Integer    1  4  0           r[4]=1
6   Integer    9  1  0           r[1]=9         ← the rowid
7   NotNull    1  9  0
8   NewRowid   0  1  0           (only if rowid was NULL)
9   MustBeInt  1
10  Noop                          uniqueness check for ROWID
11  NotExists  0 13  1           ← does rowid 9 already exist?
12  Halt 1555 2 0 'users.id'     ← SQLITE_CONSTRAINT_PRIMARYKEY
13  Affinity   2  3  0  'DBD'    ← apply column affinities to r[2..4]
15  SCopy      4  6              r[6]=age
16  IntCopy    1  7              r[7]=rowid
17  MakeRecord 6  2  5           r[5]=index key (age, rowid)
18  MakeRecord 2  3  8           r[8]=the row record
19  IdxInsert  1  5  6  2   16   insert into i_age
20  Insert     0  8  1  users 49 insert into the table
21  Halt
```

Read this once and you understand SQLite's write path completely:
constraint checks → build records → `IdxInsert` per index → `Insert` into the table.
Note `SoftNull` for the `INTEGER PRIMARY KEY` column — exactly the Phase 1 discovery that
the rowid alias is stored as NULL in the record.

---

## 5.4 The opcode families

| Family | Opcodes |
|---|---|
| control | `Init` `Goto` `Gosub` `Return` `Halt` `HaltIfNull` `Yield` `InitCoroutine` `EndCoroutine` `Jump` `Once` `If` `IfNot` `IfPos` `IfNotZero` `DecrJumpZero` `Program` `Param` |
| transactions | `Transaction` `AutoCommit` `Savepoint` `ReadCookie` `SetCookie` `VerifyCookie`(older) |
| cursors | `OpenRead` `OpenWrite` `OpenDup` `OpenEphemeral` `OpenAutoindex` `OpenPseudo` `Close` `ReopenIdx` |
| seeking | `SeekLT` `SeekLE` `SeekGE` `SeekGT` `SeekRowid` `NotExists` `SeekScan` `Found` `NotFound` `NoConflict` `SeekHit` |
| iteration | `Rewind` `Last` `Next` `Prev` `SorterNext` |
| data access | `Column` `Rowid` `IdxRowid` `NullRow` `DeferredSeek` `RowData` `TypeCheck` |
| index bounds | `IdxGT` `IdxGE` `IdxLT` `IdxLE` `IdxInsert` `IdxDelete` |
| writing | `Insert` `Delete` `NewRowid` `MakeRecord` `Clear` `Destroy` `ResetSorter` `RowCell` |
| registers | `Integer` `Int64` `String` `String8` `Null` `SoftNull` `Blob` `Variable` `Move` `Copy` `SCopy` `IntCopy` `ResultRow` |
| arithmetic | `Add` `Subtract` `Multiply` `Divide` `Remainder` `Concat` `BitAnd` `BitOr` `ShiftLeft` `ShiftRight` `BitNot` `AddImm` `MustBeInt` `RealAffinity` `Cast` |
| comparison | `Eq` `Ne` `Lt` `Le` `Gt` `Ge` `Compare` `Permutation` `IsNull` `NotNull` `IsTrue` `ZeroOrNull` |
| functions | `Function` `PureFunc` `AggStep` `AggStep1` `AggFinal` `AggValue` `AggInverse` |
| sorting | `SorterOpen` `SorterInsert` `SorterSort` `SorterData` `SorterNext` `SorterCompare` |
| misc | `Affinity` `Count` `MaxPgcnt` `Expire` `TableLock` `VBegin` `VOpen` `VFilter` `VColumn` `VNext` `VUpdate` `FkCounter` `FkIfZero` `Filter` `FilterAdd` `Explain` `Trace` `Noop` |

You do not memorize these. You learn ~25 and look the rest up at `sqlite.org/opcode.html`,
or in the doc comments in `vdbe.c`.

### Operand conventions (learn these, they are consistent)

```
P1  usually a cursor number or a register number
P2  usually a jump target (address) or a register
P3  usually a register (output)
P4  typed extra data: a KeyInfo, collation, function pointer, affinity string, table name
P5  flags (OPFLAG_*)
```

Important P5 flags:
```
OPFLAG_NCHANGE       increment the change counter
OPFLAG_LASTROWID     set last_insert_rowid
OPFLAG_APPEND        hint: rowid is larger than any existing (→ balance_quick)
OPFLAG_USESEEKRESULT the cursor is already positioned; skip the seek
OPFLAG_ISUPDATE      this insert is part of an UPDATE
OPFLAG_BULKCSR       bulk-load cursor hint
OPFLAG_SEEKEQ        cursor used only for equality seeks
OPFLAG_PERMUTE       OP_Compare uses the permutation set by OP_Permutation
```

---

## 5.5 How code is generated

```
sqlite3Select(pParse, p, pDest)                       select.c
  ├─ sqlite3SelectPrep       resolve names, expand *, type info
  ├─ optimizer passes        flattenSubquery, pushDownWhereTerms, constant propagation,
  │                          WHERE-clause transformations, count(*) optimization,
  │                          min/max optimization, ORDER BY ⇒ index, DISTINCT ⇒ index
  ├─ sqlite3WhereBegin()     ← THE PLANNER (Phase 6); emits cursor opens + loop starts
  │     ...
  ├─ inner loop body:        selectInnerLoop() → sqlite3ExprCode() for each result column
  │                          → OP_ResultRow / OP_SorterInsert / OP_IdxInsert (into a
  │                            destination: SRT_Output, SRT_Table, SRT_Set, SRT_Mem, ...)
  └─ sqlite3WhereEnd()       emits OP_Next and closes loops
```

### Expression codegen (`expr.c`)

`sqlite3ExprCode(pParse, pExpr, target)` recursively emits code leaving the value in
register `target`. The variants matter:

| Function | Meaning |
|---|---|
| `sqlite3ExprCode` | value into a specific register |
| `sqlite3ExprCodeTemp` | value into *some* register (may reuse an existing one) |
| `sqlite3ExprCodeTarget` | returns the register actually used (may avoid a copy) |
| `sqlite3ExprIfTrue` / `sqlite3ExprIfFalse` | **jump-based** evaluation for conditions — no boolean register is materialized |
| `sqlite3ExprCodeRunJustOnce` | hoist a constant expression into the prologue |

`sqlite3ExprIfTrue` is why `WHERE a>1 AND b<2` compiles to two conditional jumps rather
than computing a boolean: short-circuit evaluation falls out for free.

### Register allocation

```c
int target = ++pParse->nMem;              /* permanent register */
int reg = sqlite3GetTempReg(pParse);      /* from the temp pool */
sqlite3ReleaseTempReg(pParse, reg);
int base = sqlite3GetTempRange(pParse, n);
sqlite3ReleaseTempRange(pParse, base, n);
```
Registers are never reused across a loop boundary if a value must survive; the code
generator keeps a small pool (`aTempReg[8]`) for expression temporaries.

### Labels and jump patching

```c
int addrTop = sqlite3VdbeMakeLabel(pParse);      /* a negative pseudo-address */
sqlite3VdbeAddOp2(v, OP_Goto, 0, addrTop);       /* forward reference */
sqlite3VdbeResolveLabel(v, addrTop);             /* bind it to the current address */
/* or patch directly: */
int a = sqlite3VdbeAddOp2(v, OP_If, reg, 0);
sqlite3VdbeJumpHere(v, a);                       /* set P2 to "here" */
```

---

## 5.6 The interpreter loop

```c
int sqlite3VdbeExec(Vdbe *p){
  Op *aOp = p->aOp, *pOp = aOp;
  Mem *aMem = p->aMem;
  int rc = SQLITE_OK;
  ...
  for(pOp=&aOp[p->pc]; 1; pOp++){
    switch( pOp->opcode ){
      case OP_Goto: {
        pOp = &aOp[pOp->p2 - 1];
        break;
      }
      case OP_Integer: {          /* out2 */
        pOut = out2Prerelease(p, pOp);
        pOut->u.i = pOp->p1;
        break;
      }
      case OP_Column: { ... }
      case OP_Next: {
        rc = sqlite3BtreeNext(pC->uc.pCursor, pOp->p3);
        if( rc==SQLITE_OK ){ pOp = &aOp[pOp->p2 - 1]; }   /* loop back */
        ...
      }
      ...
    }
  }
}
```

Performance engineering you will notice while reading:
- `/* out2 */`, `/* in1 in2 out3 */`, `/* jump */` comments after each `case` — these are
  **parsed by `mkopcodeh.tcl`** to generate opcode property bitmasks
  (`OPFLG_JUMP`, `OPFLG_IN1`, `OPFLG_OUT2`…), used by assertions and by `sqlite3VdbeAddOp`.
- `out2Prerelease()` avoids re-initializing a register that is about to be overwritten.
- The hot opcodes (`Column`, `Next`, `Integer`, `Eq`) are placed early by the opcode
  numbering script to get better jump-table locality.
- `#ifdef SQLITE_DEBUG` blocks with `REGISTER_TRACE()` let you dump register contents per
  instruction (`PRAGMA vdbe_trace=ON`).

### `OP_Column` — the single most important opcode

```
OP_Column P1 P2 P3 P4 P5:   r[P3] = the value of column P2 from the row that cursor P1
                            points at
```
What it actually does:
1. Get the payload pointer for the current cell (`sqlite3BtreePayloadFetch`).
2. Parse *just enough* of the record header to find column P2 — caching the offsets in
   `VdbeCursor.aType[]`/`aOffset[]` so that a second `OP_Column` on the same row is cheap.
3. If the column is past the end of the record (a row written before `ALTER TABLE ADD
   COLUMN`), return the column's DEFAULT (P4) or NULL.
4. If the payload overflows, read through the overflow chain.

Consequence: **reading column 20 costs more than reading column 0 on the same row**,
because the header must be walked. Measurable; put hot columns early in wide tables.

---

## 5.7 Subprograms, coroutines, and frames

- **Triggers** compile to a `SubProgram`, invoked by `OP_Program`. A `VdbeFrame` saves the
  caller's registers/cursors/pc; `OP_Param` reaches into the parent frame for OLD/NEW values.
- **Co-routines** (`OP_InitCoroutine` / `OP_Yield` / `OP_EndCoroutine`) implement subqueries
  in the FROM clause that are consumed row-at-a-time without materializing. This is how
  SQLite avoids a temp table for `SELECT * FROM (SELECT ... ORDER BY ...)` in many cases.
- **Recursive CTEs** use a queue table plus `OP_Yield` loops.

```
   Coroutine pattern:

   InitCoroutine  r[1], end, start    ; set up
   ...
   loop:  Yield r[1]                  ; run the producer until it yields a row
          <consume the row>
          Goto loop
   start: <producer body>  ... Yield r[1] ... EndCoroutine r[1]
```

---

## 5.8 Statement lifecycle

```
sqlite3_prepare_v2()  → parse → codegen → sqlite3VdbeMakeReady() (allocate aMem, apCsr)
sqlite3_bind_*()      → write into p->aVar[]
sqlite3_step()        → sqlite3VdbeExec(): runs until OP_ResultRow (returns SQLITE_ROW)
                        or OP_Halt (SQLITE_DONE)
sqlite3_column_*()    → read p->pResultRow[i]
sqlite3_reset()       → rewind pc, keep bindings, release cursors
sqlite3_finalize()    → free everything
```

`sqlite3_step()` returning `SQLITE_ROW` is literally "the VM hit `OP_ResultRow` and
returned to you mid-program". The VM state is preserved; the next `step()` continues at the
following instruction. That is why SQLite can stream a 10 GB result with no memory growth,
and why **holding a statement open holds a read transaction open**.

---

## 5.9 Optimizations visible in bytecode

| Optimization | What you see |
|---|---|
| constant factoring | `Integer`/`String8` in the prologue, not in the loop |
| `count(*)` optimization | `OP_Count` on the smallest index instead of a scan |
| min/max optimization | `OP_Last`/`OP_Rewind` + one `OP_Column`, no loop |
| covering index | table cursor never opened |
| deferred seek | `OP_DeferredSeek` instead of an immediate table seek |
| `OP_Once` | one-time initialization of a subquery result (`materialize once`) |
| short-circuit conditions | jump chains instead of boolean registers |
| index satisfies ORDER BY | no `SorterOpen` in the program |
| index satisfies DISTINCT | no ephemeral dedup table |
| automatic index | `OP_OpenAutoindex` — the planner built a transient index for a join |
| Bloom filter (3.38+) | `OP_FilterAdd` / `OP_Filter` on the outer loop of a star join |
| transfer optimization | `INSERT INTO t2 SELECT * FROM t1` copies raw records with `OP_RowData`/`OP_Insert` when schemas match |
