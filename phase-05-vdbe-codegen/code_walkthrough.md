# Phase 5 — Code Walkthrough: The VDBE

Source:
[`src/vdbe.c`](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) ·
[`src/vdbeaux.c`](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c) ·
[`src/vdbeInt.h`](https://github.com/sqlite/sqlite/blob/master/src/vdbeInt.h) ·
[`src/expr.c`](https://github.com/sqlite/sqlite/blob/master/src/expr.c) ·
opcode reference: [opcode.html](https://sqlite.org/opcode.html)

---

## Mental model for this phase

> **`vdbe.c` is not one 9,000-line function. It is ~190 independent 5-to-50-line programs
> that share a set of local variables.** Never read it top to bottom. Pick one
> `case OP_X:` and read only that.

Two supporting rules:

1. **`EXPLAIN` tells you which cases to read.** Run the query, get the opcode list, read
   exactly those cases. That turns "understand the VDBE" into ten short reading tasks.
2. **The `/* Opcode: */` comment above each case is compiled.** `mkopcodeh.tcl` parses those
   comments to generate `opcodes.h`, and parses the `/* out2 */`, `/* jump, in1 */`
   annotations after each case label to generate opcode property flags. Comments here are
   source code.

---

## 1. The interpreter loop, stripped to its skeleton

Real code, `src/vdbe.c`:

```c
SQLITE_PRIVATE int sqlite3VdbeExec(
  Vdbe *p                    /* The VDBE */
){
  Op *aOp = p->aOp;          /* Copy of p->aOp */
  Op *pOp = aOp;             /* Current operation */
  int rc = SQLITE_OK;
  sqlite3 *db = p->db;
  u8 encoding = ENC(db);
  int iCompare = 0;          /* Result of last comparison */
  u64 nVmStep = 0;           /* Number of virtual machine steps */
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1 = 0;             /* 1st input operand */
  Mem *pIn2 = 0;
  Mem *pIn3 = 0;
  Mem *pOut = 0;             /* Output operand */
  ...
  for(pOp=&aOp[p->pc]; 1; pOp++){
    /* Errors are detected by individual opcodes, with an immediate
    ** jumps to abort_due_to_error. */
    assert( rc==SQLITE_OK );
    assert( pOp>=aOp && pOp<&aOp[p->nOp]);
    nVmStep++;
    ...
    switch( pOp->opcode ){
      ...
    }
  }
}
```

| Detail | Why it is like that |
|---|---|
| `Op *aOp = p->aOp;` / `Mem *aMem = p->aMem;` | local copies so the compiler can keep them in registers; every opcode dereferences them. Purely a performance shape. |
| `for(pOp=&aOp[p->pc]; 1; pOp++)` | **the default control flow is "next instruction".** Jumps are implemented by assigning `pOp` and letting `pOp++` land on the target — which is why you will see `pOp = &aOp[pOp->p2 - 1];` with the `-1`. |
| `pIn1, pIn2, pIn3, pOut` | shared operand pointers set up by the pre-dispatch code from the opcode's property flags, so individual cases don't repeat `&aMem[pOp->p1]`. |
| `nVmStep++` | drives `sqlite3_progress_handler()` and `SQLITE_STMTSTATUS_VM_STEP`. This counter is how a runaway query becomes interruptible. |
| `assert( rc==SQLITE_OK );` at loop top | the error discipline: an opcode that fails must `goto abort_due_to_error`, never fall through with `rc` set. |
| `goto vdbe_return` / `goto no_mem` / `goto abort_due_to_error` | a handful of shared exit labels at the bottom of the function. Every opcode's error path funnels into them; this is why the function is one giant block rather than 190 functions. |

The debug-only block just before the `switch` is worth one read:

```c
#ifdef SQLITE_DEBUG
    if( db->flags & SQLITE_VdbeTrace ){
      sqlite3VdbePrintOp(stdout, (int)(pOp - aOp), pOp);
      test_trace_breakpoint((int)(pOp - aOp),pOp,p);
    }
    {
      u8 opProperty = sqlite3OpcodeProperty[pOp->opcode];
      if( (opProperty & OPFLG_IN1)!=0 ){
        assert( pOp->p1>0 );
        assert( pOp->p1<=(p->nMem+1 - p->nCursor) );
        assert( memIsValid(&aMem[pOp->p1]) );
        assert( sqlite3VdbeCheckMemInvariants(&aMem[pOp->p1]) );
        REGISTER_TRACE(pOp->p1, &aMem[pOp->p1]);
      }
      ...
    }
#endif
```

That is `PRAGMA vdbe_trace=ON` (Phase 5 Lab 5.3) and the machinery that validates the
`/* in1 out2 jump */` annotations: if you add an opcode and label it `in1` but pass a
non-register in P1, a debug build stops at that assert. **This is the safety net for Lab
5.6.**

---

## 2. The simplest opcode, complete

```c
case OP_Integer: {         /* out2 */
  pOut = out2Prerelease(p, pOp);
  pOut->u.i = pOp->p1;
  break;
}
```

Three lines, and everything about the VDBE is in them:

- `/* out2 */` — the annotation. `mkopcodeh.tcl` turns it into `OPFLG_OUT2` in
  `sqlite3OpcodeProperty[]`, which the debug block above uses, and which
  `sqlite3VdbeAddOp*` uses for validation.
- `out2Prerelease(p, pOp)` — returns `&aMem[pOp->p2]` after releasing whatever that
  register held **and** pre-setting `flags = MEM_Int`. It exists because "overwrite an
  output register with an integer" is so common that the release+init sequence was worth
  factoring into an inline function.
- `pOut->u.i = pOp->p1;` — the actual work. P1 carries a 32-bit literal inside the
  instruction; `OP_Int64` exists for wider values, with the value hanging off P4.

---

## 3. `OP_Next` — the loop primitive

```c
case OP_Next:          /* jump, ncycle */
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pOp->p5==0
       || pOp->p5==SQLITE_STMTSTATUS_FULLSCAN_STEP
       || pOp->p5==SQLITE_STMTSTATUS_AUTOINDEX);
  pC = p->apCsr[pOp->p1];
  assert( pC->deferredMoveto==0 );
  assert( pC->eCurType==CURTYPE_BTREE );
  assert( pC->seekOp==OP_SeekGT || pC->seekOp==OP_SeekGE
       || pC->seekOp==OP_Rewind || pC->seekOp==OP_Found
       || pC->seekOp==OP_NullRow|| pC->seekOp==OP_SeekRowid
       || pC->seekOp==OP_IfNoHope);
  rc = sqlite3BtreeNext(pC->uc.pCursor, pOp->p3);

next_tail:
  pC->cacheStatus = CACHE_STALE;
  VdbeBranchTaken(rc==SQLITE_OK,2);
  if( rc==SQLITE_OK ){
    pC->nullRow = 0;
    p->aCounter[pOp->p5]++;
    goto jump_to_p2_and_check_for_interrupt;
  }
  if( rc!=SQLITE_DONE ) goto abort_due_to_error;
  rc = SQLITE_OK;
  pC->nullRow = 1;
  goto check_for_interrupt;
}
```

| Line | Reading |
|---|---|
| `assert( pC->seekOp==OP_SeekGT || ... )` | **an assert as documentation of the code generator's contract:** a cursor may only be advanced if it was positioned by one of these opcodes. If you write codegen that emits `OP_Next` after something else, a debug build catches it immediately. |
| `rc = sqlite3BtreeNext(pC->uc.pCursor, pOp->p3);` | the one call downward. Everything else in this opcode is bookkeeping. |
| `pC->cacheStatus = CACHE_STALE;` | **the single most important line.** The cursor moved, so the cached record-header offsets (`aType[]`/`aOffset[]`) used by `OP_Column` are no longer valid. Forgetting this invalidation in new code is the classic VDBE patch bug — it produces wrong column values, not crashes. |
| `p->aCounter[pOp->p5]++;` | P5 selects a statistics counter. `SQLITE_STMTSTATUS_FULLSCAN_STEP` is how `sqlite3_stmt_status()` can tell you a query is doing a full scan — set by the code generator when it knows the loop is unconstrained. |
| `goto jump_to_p2_and_check_for_interrupt;` | loop back. The interrupt check lives on the *backward* branch only, so a long loop is interruptible while straight-line code pays nothing. |
| `if( rc!=SQLITE_DONE ) goto abort_due_to_error;` then `rc = SQLITE_OK;` | `SQLITE_DONE` from the b-tree means "no more rows", which is not an error at this level. Translating layer-specific codes is a recurring job in `vdbe.c`. |
| `pC->nullRow = 1;` | past the end: any later `OP_Column` on this cursor returns NULL rather than reading garbage. This is what makes `LEFT JOIN` NULL-extension work. |

`OP_Prev`, `OP_SorterNext`, and `OP_VNext` all fall into the same `next_tail:` label — one
implementation, four entry points.

---

## 4. `OP_ResultRow` — how a row reaches your application

```c
case OP_ResultRow: {
  assert( p->nResColumn==pOp->p2 );
  assert( pOp->p1>0 || CORRUPT_DB );
  assert( pOp->p1+pOp->p2<=(p->nMem+1 - p->nCursor)+1 );

  p->cacheCtr = (p->cacheCtr + 2)|1;
  p->pResultRow = &aMem[pOp->p1];
  ...
  if( db->mallocFailed ) goto no_mem;
  if( db->mTrace & SQLITE_TRACE_ROW ){
    db->trace.xV2(SQLITE_TRACE_ROW, db->pTraceArg, p, 0);
  }
  p->pc = (int)(pOp - aOp) + 1;
  rc = SQLITE_ROW;
  goto vdbe_return;
}
```

- `p->pResultRow = &aMem[pOp->p1];` — it **does not copy anything.** The result row is just
  a pointer into the register array. `sqlite3_column_text(stmt, i)` reads
  `p->pResultRow[i]`. That is why column pointers are invalidated by the next `step()`.
- `p->pc = (int)(pOp - aOp) + 1;` then `rc = SQLITE_ROW; goto vdbe_return;` — **save the
  program counter and return to the caller mid-program.** The next `sqlite3_step()` resumes
  at the saved `pc`. This is the whole mechanism behind SQLite streaming a 10 GB result in
  constant memory, and behind "an open statement holds a read transaction".
- `p->cacheCtr = (p->cacheCtr + 2)|1;` — bumps an odd-valued counter that invalidates the
  `sqlite3_column_*` type-conversion caches from the previous row.
- The `SQLITE_DEBUG` block sets `pMem[i].pScopyFrom = 0` with a comment explaining that
  `sqlite3_column_*` may mutate the registers (e.g. converting an integer to text in place),
  so shallow-copy (`OP_SCopy`) dependency tracking must be broken here. That comment is the
  best short explanation of why `sqlite3_column_text()` can invalidate a pointer you got
  from `sqlite3_column_blob()`.

---

## 5. `OP_Column` — mechanism, then the lines that matter

~200 lines. [Source](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) — search for
`case OP_Column:`. Mechanism:

```
1. pC = apCsr[P1]; if pC->nullRow → result is NULL (or the DEFAULT from P4), done.
2. if pC->cacheStatus != p->cacheCtr:
       the cached header parse is stale → re-read the payload:
         - sqlite3BtreePayloadFetch() gives a direct pointer if the row is local
         - parse the record header varints into pC->aType[] and pC->aOffset[]
         - only as far as needed to reach column P2  ("lazy header parse")
       pC->cacheStatus = p->cacheCtr
3. if P2 >= number of columns IN THE RECORD:
       return P4 default, or NULL      ← this is how ALTER TABLE ADD COLUMN works on
                                          rows written before the column existed
4. sqlite3VdbeSerialGet(payload + aOffset[P2], aType[P2], pDest)
5. if the payload spilled to overflow pages, use sqlite3VdbeMemFromBtree() instead of
   the zero-copy pointer
```

The three consequences you can now state precisely:

- **Column position costs.** Reaching column 20 requires walking 20 header varints; column 0
  requires one. Measured in Phase 5 Lab 5.9.
- **The cache is per-cursor-position.** `pC->cacheStatus == p->cacheCtr` is the test; any
  cursor movement sets `CACHE_STALE`, and `OP_ResultRow` bumps `cacheCtr`. Two `OP_Column`s
  on the same row share one header parse.
- **`OP_Column` can return a pointer into the page buffer** (`MEM_Ephem`). Anything that
  must outlive the cursor calls `sqlite3VdbeMemMakeWriteable()`.

---

## 6. `OP_MakeRecord` — the header-size chicken-and-egg

~150 lines. Mechanism:

```
pass 1: for each input register, compute its serial type and body size;
        accumulate nHdr (bytes of serial-type varints) and nByte (body bytes)
        apply the affinity string from P4 first, if present
        ---- then the tricky bit ----
        nVarint = sqlite3VarintLen(nHdr);
        nHdr += nVarint;
        if( nVarint < sqlite3VarintLen(nHdr) ) nHdr++;   /* crossing 127 grew the varint */
pass 2: write the header-size varint, then each serial type, then each body
```

That three-line fixup is the whole puzzle: the record header must begin with *its own
total size*, but adding that varint can push the total past 127 and make the varint one byte
longer. SQLite handles it by computing, then checking whether the length changed, then
adding one byte of slack. Your Phase 1 `dbparse.c` reads this format; this is the code that
writes it.

Also in there: the `nZero` handling for `zeroblob()` — trailing zero bytes are counted but
never materialized, so `INSERT INTO t VALUES(zeroblob(1000000))` does not allocate a
megabyte.

---

## 7. Code generation: `sqlite3ExprIfTrue()` (mechanism)

[Source](https://github.com/sqlite/sqlite/blob/master/src/expr.c). The function that
explains why SQL boolean logic short-circuits:

```
sqlite3ExprIfTrue(pParse, pExpr, dest, jumpIfNull):
   "emit code that jumps to `dest` if pExpr is true"

   case TK_AND:
        create label d2
        sqlite3ExprIfFalse(pParse, pExpr->pLeft,  d2,   jumpIfNull^SQLITE_JUMPIFNULL);
        sqlite3ExprIfTrue (pParse, pExpr->pRight, dest, jumpIfNull);
        resolve d2
   case TK_OR:
        sqlite3ExprIfTrue(pParse, pExpr->pLeft,  dest, jumpIfNull);
        sqlite3ExprIfTrue(pParse, pExpr->pRight, dest, jumpIfNull);
   case TK_LT/LE/GT/GE/EQ/NE:
        code both operands into registers, emit OP_Lt/OP_Le/... with P2 = dest
   default:
        code the expression into a register, emit OP_If
```

**No boolean value is ever materialized for `AND`/`OR`.** The tree is compiled directly into
a jump graph, so short-circuit evaluation is not an optimization — it is the only thing the
code generator knows how to emit. `jumpIfNull` threading is how SQL's three-valued logic is
preserved through that transformation; get it wrong and `WHERE NOT (x = NULL)` changes
meaning.

Compare with `sqlite3ExprCodeTarget()`, which *does* produce a value, and note that the
WHERE clause always goes through `ExprIfFalse` while a `SELECT` result column goes through
`ExprCode`. Same tree, two compilers.

---

## 8. Program assembly: `sqlite3VdbeAddOp3()` and labels (mechanism)

[Source](https://github.com/sqlite/sqlite/blob/master/src/vdbeaux.c).

```
sqlite3VdbeAddOp3(v, op, p1, p2, p3)
   → growOpArray() if needed (the array doubles)
   → aOp[nOp] = {op, p1, p2, p3, p4type=P4_NOTUSED, p5=0}
   → returns the ADDRESS of the new instruction
```

Forward jumps use one of two idioms:

```c
/* idiom 1: patch later by address */
int addr = sqlite3VdbeAddOp2(v, OP_If, reg, 0);   /* P2 = 0, to be filled in */
... emit the body ...
sqlite3VdbeJumpHere(v, addr);                     /* sets aOp[addr].p2 = current address */

/* idiom 2: labels (negative pseudo-addresses), for targets used more than once */
int lbl = sqlite3VdbeMakeLabel(pParse);
sqlite3VdbeAddOp2(v, OP_Goto, 0, lbl);
...
sqlite3VdbeResolveLabel(v, lbl);
```

Labels are stored as **negative** P2 values and resolved in a final pass
(`resolveP2Values()`), which is also where the code walks every instruction to compute
`p->readOnly`, `p->bIsReader`, and whether a statement journal is needed. That pass is why
`sqlite3VdbeMakeReady()` can allocate registers and cursors in one block.

---

## 9. Exercises against real source

Answer in `labs/phase05/opcodes-read.md` with `src/file.c` + opcode citations:

1. Find `out2Prerelease()`. What does it do about the previous contents of the register, and
   why is it an inline function rather than a macro?
2. In `OP_Next`, explain `pC->cacheStatus = CACHE_STALE;` — construct the wrong-answer bug
   that would occur without it.
3. Find `jump_to_p2_and_check_for_interrupt:` and `check_for_interrupt:`. Why is the
   interrupt test only on backward jumps?
4. In `OP_Column`, find the branch that handles `P2 >= number of columns in the record`.
   Which SQL feature depends on it? Write a two-statement reproduction.
5. In `OP_MakeRecord`, quote the three lines that fix up the header-size varint and explain
   the boundary case with a concrete example (a record whose header is exactly 127 bytes).
6. Find `case OP_Halt:` and explain how `P1`/`P2` encode a constraint violation
   (cross-check with the `Halt 1555 2` you saw in the INSERT bytecode in `knowledge.md`).
7. Find where `p->pResultRow` is read by the `sqlite3_column_*` family in `vdbeapi.c`, and
   explain exactly when those pointers become invalid.
8. Pick any opcode and change its `/* out2 */` annotation to `/* in1 */`, rebuild with
   `SQLITE_DEBUG`, and record which assert fires. That is the annotation system protecting
   you.

```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400
sed -n '/^static Mem \*out2Prerelease(/,/^}/p' $A/sqlite3.c
grep -n "^case OP_Halt:" $A/sqlite3.c
grep -n "sqlite3OpcodeProperty\[\]" $A/sqlite3.c | head -3
```
