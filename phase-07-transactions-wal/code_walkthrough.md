# Phase 7 — Code Walkthrough: `wal.c`

Source: [`src/wal.c`](https://github.com/sqlite/sqlite/blob/master/src/wal.c) ·
spec: [walformat.html](https://sqlite.org/walformat.html) ·
design: [wal.html](https://sqlite.org/wal.html)

---

## Mental model for this phase

> **WAL is a shared-memory protocol with a disk file attached.** The `-wal` file is
> trivial — a header and a list of frames. All the difficulty is in the `-shm` wal-index:
> three counters (`mxFrame`, `nBackfill`, `aReadMark[5]`) and eight lock slots, manipulated
> by three roles (reader, writer, checkpointer) with no central coordinator.

**Read the 400-line header comment at the top of `src/wal.c` before any code.** It is the
design document, it is complete, and nothing else explains the protocol as well:

```sh
head -420 ~/Desktop/sqllite/sqlite-src/src/wal.c
```

While reading protocol code, hold one question: *what does this connection know, and what
must it re-check after taking a lock?* Every retry loop in the file is an answer.

---

## 1. `walChecksumBytes()` — the checksum, and why it is shaped like that

Real code:

```c
static void walChecksumBytes(
  int nativeCksum, /* True for native byte-order, false for non-native */
  u8 *a,           /* Content to be checksummed */
  int nByte,       /* Bytes of content in a[].  Must be a multiple of 8. */
  const u32 *aIn,  /* Initial checksum value input */
  u32 *aOut        /* OUT: Final checksum value output */
){
  u32 s1, s2;
  u32 *aData = (u32 *)a;
  u32 *aEnd = (u32 *)&a[nByte];

  if( aIn ){
    s1 = aIn[0];
    s2 = aIn[1];
  }else{
    s1 = s2 = 0;
  }

  assert( nByte>=8 && (nByte&7)==0 && nByte<=65536 );

  if( !nativeCksum ){
    do {
      s1 += BYTESWAP32(aData[0]) + s2;
      s2 += BYTESWAP32(aData[1]) + s1;
      aData += 2;
    }while( aData<aEnd );
  }else if( nByte%64==0 ){
    do {
      s1 += *aData++ + s2;
      s2 += *aData++ + s1;
      /* ... unrolled 8 times ... */
    }while( aData<aEnd );
  }else{
    do {
      s1 += *aData++ + s2;
      s2 += *aData++ + s1;
    }while( aData<aEnd );
  }
  aOut[0] = s1;
  aOut[1] = s2;
}
```

| Element | Reading |
|---|---|
| `aIn` / `aOut` | **the chain.** The previous frame's checksum seeds the next one, so frame N's checksum depends on every byte of frames 1..N. A frame cannot be moved, reordered, or reused from an older WAL. |
| `s1 += x0 + s2; s2 += x1 + s1;` | Fibonacci-weighted: each word influences all later state. Cheap (two adds per 8 bytes) and order-sensitive, which is exactly what torn-write detection needs. It is **not** cryptographic and is not meant to be. |
| `nativeCksum` | **the endianness switch you fought in Lab 7.1.** The caller computes `nativeCksum = (pWal->hdr.bigEndCksum==SQLITE_BIGENDIAN)` — so the magic's low bit does not name an endianness directly, it says whether the file's convention matches *this machine*. On a little-endian box, magic `0x377f0682` (bit 0 clear) means "native", i.e. little-endian words. That is why your first `walparse.c` mismatched. |
| `nByte%64==0` unrolled branch | a page is always a multiple of 64 bytes, so the page-data pass takes the unrolled loop; the 8-byte frame-header pass takes the plain one. Pure throughput tuning on the hottest write path. |
| `assert( nByte>=8 && (nByte&7)==0 )` | the contract that lets the loop read two words at a time with no tail handling. |

---

## 2. `walEncodeFrame()` — the commit point, written

```c
static void walEncodeFrame(
  Wal *pWal,                      /* The write-ahead log */
  u32 iPage,                      /* Database page number for frame */
  u32 nTruncate,                  /* New db size (or 0 for non-commit frames) */
  u8 *aData,                      /* Pointer to page data */
  u8 *aFrame                      /* OUT: Write encoded frame here */
){
  int nativeCksum;
  u32 *aCksum = pWal->hdr.aFrameCksum;
  assert( WAL_FRAME_HDRSIZE==24 );
  sqlite3Put4byte(&aFrame[0], iPage);
  sqlite3Put4byte(&aFrame[4], nTruncate);
  if( pWal->iReCksum==0 ){
    memcpy(&aFrame[8], pWal->hdr.aSalt, 8);

    nativeCksum = (pWal->hdr.bigEndCksum==SQLITE_BIGENDIAN);
    walChecksumBytes(nativeCksum, aFrame, 8, aCksum, aCksum);
    walChecksumBytes(nativeCksum, aData, pWal->szPage, aCksum, aCksum);

    sqlite3Put4byte(&aFrame[16], aCksum[0]);
    sqlite3Put4byte(&aFrame[20], aCksum[1]);
  }else{
    memset(&aFrame[8], 0, 16);
  }
}
```

This 20-line function is the entire frame format from `knowledge.md` §7.3:

- `aFrame[0]` = page number, `aFrame[4]` = **`nTruncate`**, the "database size after this
  commit". Non-zero marks a commit frame. That is the commit point — **one 4-byte field.**
  Your `walparse.c` prints exactly this and labels it `<<< COMMIT`.
- `memcpy(&aFrame[8], pWal->hdr.aSalt, 8);` — the salts, copied from the WAL header so a
  frame from a previous WAL generation cannot validate.
- Two `walChecksumBytes` calls, chained through `aCksum`: first the **first 8 bytes of the
  frame header** (page number + nTruncate), then the page data. Note the salts themselves
  are *not* checksummed — they are compared directly during recovery.
- `aCksum = pWal->hdr.aFrameCksum` is a pointer into the in-memory header, so the running
  chain state is updated in place, ready for the next frame.
- `pWal->iReCksum` — a subtlety: when a transaction is rewritten (`ROLLBACK TO` inside a
  write transaction), some already-written frames must be re-checksummed later. Until then
  their header bytes are zeroed. Follow `iReCksum` to `walRewriteChecksums()` if you want
  the full story.

**Exercise:** confirm against your Lab 7.1 hexdump that frame 1 has `nTruncate==0` and frame
2 has `nTruncate==2`. You now know the exact lines of C that wrote those bytes.

---

## 3. `walTryBeginRead()` — the reader protocol, with its own retry philosophy

This is the most subtle function in SQLite, and it explains its own design in comments.

### The anti-spin policy

```c
  /* Take steps to avoid spinning forever if there is a protocol error.
  **
  ** Circumstances that cause a RETRY should only last for the briefest
  ** instances of time.  No I/O or other system calls are done while the
  ** locks are held, so the locks should not be held for very long. But
  ** if we are unlucky, another process that is holding a lock might get
  ** paged out or take a page-fault that is time-consuming to resolve,
  ** during the few nanoseconds that it is holding the lock.  In that case,
  ** it might take longer than normal for the lock to free.
  **
  ** After 5 RETRYs, we begin calling sqlite3OsSleep().  The first few
  ** calls to sqlite3OsSleep() have a delay of 1 microsecond.  Really this
  ** is more of a scheduler yield than an actual delay.  But on the 10th
  ** an subsequent retries, the delays start becoming longer and longer,
  ** so that on the 100th (and last) RETRY we delay for 323 milliseconds.
  ** The total delay time before giving up is less than 10 seconds.
  */
  (*pCnt)++;
  if( *pCnt>5 ){
    int nDelay = 1;
    int cnt = (*pCnt & ~WAL_RETRY_BLOCKED_MASK);
    if( cnt>WAL_RETRY_PROTOCOL_LIMIT ){
      VVA_ONLY( pWal->lockError = 1; )
      return SQLITE_PROTOCOL;
    }
    if( *pCnt>=10 ) nDelay = (cnt-9)*(cnt-9)*39;
    ...
```

Read this as a model for lock-free-ish protocol design: the critical sections hold no locks
across I/O, so contention should resolve in nanoseconds; a retry that keeps failing means
something pathological (a descheduled process, or a genuine protocol bug), so back off
quadratically and eventually return `SQLITE_PROTOCOL` rather than hang. `(cnt-9)²*39`
microseconds reaches 323 ms at the 100th try.

### The read-mark-0 fast path, and why it re-checks

```c
    if( !useWal && AtomicLoad(&pInfo->nBackfill)==pWal->hdr.mxFrame ){
      /* The WAL has been completely backfilled (or it is empty).
      ** and can be safely ignored. */
      rc = walLockShared(pWal, WAL_READ_LOCK(0));
      walShmBarrier(pWal);
      if( rc==SQLITE_OK ){
        if( memcmp((void *)walIndexHdr(pWal), &pWal->hdr, sizeof(WalIndexHdr)) ){
          /* It is not safe to allow the reader to continue here if frames
          ** may have been appended to the log before READ_LOCK(0) was obtained.
          ** When holding READ_LOCK(0), the reader ignores the entire log file,
          ** which implies that the database file contains a trustworthy
          ** snapshot. ...
          ** However, if frames have been appended to the log ... before the
          ** READ_LOCK(0) is obtained, that is not necessarily true. A
          ** checkpointer may have started to backfill the appended frames but
          ** crashed before it finished. Leaving a corrupt image in the
          ** database file. */
          walUnlockShared(pWal, WAL_READ_LOCK(0));
          return WAL_RETRY;
        }
        pWal->readLock = 0;
        return SQLITE_OK;
      }
    }
```

This is the pattern the whole file is built on:

1. read shared state **without** a lock,
2. take the lock,
3. `walShmBarrier()` (a memory barrier — the `-shm` is shared memory, not a file you
   `read()`),
4. **re-read and compare** the state you based your decision on,
5. if it changed, drop the lock and `WAL_RETRY`.

`aReadMark[0]` is hard-wired to 0 and means "I will ignore the WAL entirely; the database
file alone is my snapshot". Holding `WAL_READ_LOCK(0)` prevents a checkpointer from
backfilling, which is what makes that claim safe — *provided* nothing was appended in the
window. The comment spells out the exact failure (a crashed mid-backfill checkpointer) that
the re-check prevents.

### Choosing a read mark

```c
    mxReadMark = 0;
    mxI = 0;
    mxFrame = pWal->hdr.mxFrame;
    for(i=1; i<WAL_NREADER; i++){
      u32 thisMark = AtomicLoad(pInfo->aReadMark+i);
      if( mxReadMark<=thisMark && thisMark<=mxFrame ){
        mxReadMark = thisMark;
        mxI = i;
      }
    }
    if( (pWal->readOnly & WAL_SHM_RDONLY)==0
     && (mxReadMark<mxFrame || mxI==0)
    ){
      for(i=1; i<WAL_NREADER; i++){
        rc = walLockExclusive(pWal, WAL_READ_LOCK(i), 1);
        ...    /* claim a slot and set aReadMark[i] = mxFrame */
      }
    }
```

Plain English: *find the largest read mark that does not exceed `mxFrame` and share it. If
none is good enough, briefly take a slot exclusively and set it to `mxFrame`.* Five slots
means at most five distinct snapshots are pinned at once; readers wanting the same snapshot
share a slot, which is why thousands of concurrent readers work fine.

`AtomicLoad()` everywhere: this is genuinely concurrent shared memory, not file I/O.

---

## 4. `walCheckpoint()` — mechanism only

~300 lines. [Source](https://github.com/sqlite/sqlite/blob/master/src/wal.c) — search for
`static int walCheckpoint(`. Mechanism:

```
1. take WAL_CKPT_LOCK exclusively (only one checkpointer)
2. mxSafeFrame = pWal->hdr.mxFrame;  mxPage = pWal->hdr.nPage;
   for i in 1..WAL_NREADER-1:
       y = aReadMark[i]
       if( mxSafeFrame > y ){
            try to lock READ_LOCK(i) exclusively:
              success → nobody is using that mark; reset it to READMARK_NOT_USED
              busy    → a reader is pinned there → mxSafeFrame = y   ← THE CLAMP
       }
3. if( nBackfill < mxSafeFrame ):
       take READ_LOCK(0) exclusively (so no "ignore the WAL" reader starts mid-copy)
       walIteratorInit() → an iterator that yields, for each page, the NEWEST frame
                            <= mxSafeFrame  (a merge over the wal-index blocks)
       for each (pgno, frame): read the frame, write the page into the db file
       fsync the db (if synchronous >= NORMAL)
       nBackfill = mxSafeFrame
4. if the whole WAL was backfilled and no reader is in the WAL:
       walRestartHdr()  → new salts, mxFrame = 0  (the WAL is reused from the start)
   modes RESTART/TRUNCATE additionally wait for readers and ftruncate the file
```

Step 2 is the answer to Lab 7.5's WAL-growth pathology, in code: **a reader holding a read
mark clamps `mxSafeFrame`, so nothing past it can be copied, so the WAL cannot shrink.**
The clamp is not an error path — it is the normal, correct behaviour.

`walIteratorInit()` is worth a look on its own: it builds a merge of the wal-index page
arrays so the copy pass visits each page once, in roughly ascending page order, taking only
the newest version. That ordering is why a checkpoint writes the database file mostly
sequentially.

---

## 5. `walIndexRecover()` — mechanism only

~200 lines. Mechanism:

```
1. take WAL_RECOVER_LOCK exclusively
2. read the 32-byte WAL header
   - magic must be 0x377f068{2,3}; version must be 3007000; page size sane
   - verify the header's own checksum over its first 24 bytes
   - any failure → treat the WAL as EMPTY (not as an error!)
3. aFrameCksum = the header checksum; iFrame = 0
4. for each frame in the file:
       walDecodeFrame(): check salts match the header, check the chained checksum
       if either fails → STOP
       if nTruncate != 0 → remember this as the last valid COMMIT
       walIndexAppend(): add (pgno → frame) to the wal-index hash table
5. mxFrame = the last COMMIT frame seen (NOT the last valid frame)
6. write the rebuilt WalIndexHdr
```

Two design points:

- **A bad header means "no WAL", not "corrupt database".** A garbage `-wal` left by an
  unrelated crash is simply ignored; the database file alone is then the truth.
- **Recovery stops at the last commit frame**, so a half-written transaction is invisible.
  This is what your `walparse.c` reproduces when it prints "Recovery would STOP here".
- Recovery **never writes to the database file.** It only rebuilds the index. That makes it
  fast, idempotent, and safe to run from a read-only-ish context.

---

## 6. The wal-index hash: `walHash()` / `walIndexAppend()` / `sqlite3WalFindFrame()`

```
The -shm is divided into blocks; each covers HASHTABLE_NPAGE (4096) frames and carries
an 8192-slot hash table (load factor <= 0.5, so probes are short).

walIndexAppend(iFrame, iPage):
    aPgno[iFrame] = iPage                        /* frame -> page number */
    iKey = walHash(iPage)
    while( aHash[iKey] ) iKey = walNextHash(iKey)  /* linear probe */
    aHash[iKey] = iFrame - iZero                 /* page -> frame, within this block */

sqlite3WalFindFrame(pgno, &iRead):
    for each block, NEWEST FIRST:
        probe the hash for pgno, walking the collision chain
        take the largest frame number that is <= my read mark
        if found → return it
    not found → the page comes from the database file
```

Reading notes:
- Newest-block-first is what makes "the most recent version of this page" an early exit.
- The hash stores *frame numbers relative to the block* so entries fit in 16 bits.
- `sqlite3WalFindFrame()` is called for **every page read** in WAL mode. That is the price
  of WAL: one hash probe per page fetch, in exchange for never blocking a writer.

---

## 7. Savepoints in WAL mode

```c
SQLITE_PRIVATE void sqlite3WalSavepoint(Wal *pWal, u32 *aWalData){
  aWalData[0] = pWal->hdr.mxFrame;
  aWalData[1] = pWal->hdr.aFrameCksum[0];
  aWalData[2] = pWal->hdr.aFrameCksum[1];
  aWalData[3] = pWal->nCkpt;
}
```
Four integers. Rolling back to a savepoint in WAL mode is
`sqlite3WalSavepointUndo()`: restore `mxFrame` and the checksum chain state, and truncate
the wal-index back. The frames are still in the file but become unreachable, and the next
write overwrites them. Compare with the rollback-journal path, which must replay page
images — WAL savepoints are almost free.

---

## 8. Exercises against real source

Answer in `labs/phase07/source-questions.md` with `src/wal.c` + function citations:

1. In `walChecksumBytes`, explain `nativeCksum` and prove (from `walEncodeFrame`) how the
   magic number's low bit reaches it. Why is the database file always big-endian while the
   WAL is not?
2. Quote the `nTruncate` line in `walEncodeFrame` and explain why a single non-zero field
   is sufficient to mark a commit — what makes it impossible to see a "commit" from a torn
   write?
3. Read the anti-spin comment in `walTryBeginRead`. Compute the delay on retry 20, 50, and
   100. What error is returned at the limit, and what does that error imply about the
   system?
4. In the `READ_LOCK(0)` fast path, describe precisely the crash scenario the `memcmp`
   re-check prevents. Draw the timeline.
5. In `walCheckpoint`, find where `mxSafeFrame` is clamped by a reader's mark, and connect
   it to the WAL-growth experiment in Lab 7.5.
6. Find `walRestartHdr()`. Under exactly what conditions does the WAL get reset, and what
   changes in the header?
7. Find `walIndexAppend()` and `HASHTABLE_NPAGE`/`HASHTABLE_NSLOT`. How many bytes of `-shm`
   are needed per 4096 frames?
8. Find `sqlite3WalFindFrame()`. How many hash probes does a page read cost in a WAL with
   10,000 frames, and why does searching newest-block-first matter?
9. Compare `sqlite3WalSavepointUndo()` with the rollback-journal savepoint path in
   `pager.c`. Which is cheaper, and why?

```sh
A=~/Desktop/sqllite/sqlite-amalgamation-3500400
grep -n "define HASHTABLE_NPAGE\|define HASHTABLE_NSLOT\|define WAL_NREADER\|define WAL_FRAME_HDRSIZE" $A/sqlite3.c
sed -n '/^static int walRestartHdr(/,/^}/p' $A/sqlite3.c
sed -n '/^SQLITE_PRIVATE int sqlite3WalFindFrame(/,/^}/p' $A/sqlite3.c | head -60
```
