# Labs — working code, already built and verified

Every tool here was compiled and run on this machine while the curriculum was written.
They are starting points, not finished products — each phase's `3.implementation.md` lists
the extensions you are expected to add.

| Path | What it is | Verified |
|---|---|---|
| `c-primer/cdemo.c` | Every C technique SQLite uses, printed with real output: struct padding, bit flags, unions, first-member subclassing, flexible arrays, big-endian, sign extension, varints, `goto` cleanup, `memmove` | ✅ all 12 sections run |
| `phase01/dbparse.c` | Dependency-free SQLite **database file** parser: header, b-tree walk, cell + record decode, spill formulas | ✅ decodes real tables and indexes |
| `phase02/tracevfs.c` | **VFS shim** that logs every OS call (read/write/sync/lock/shm) | ✅ shows the full commit protocol |
| `phase03/minibtree.c` | A miniature **slotted-page B+tree** with splits, in SQLite's shape | ✅ 50k seq + 50k random inserts, all checks green |
| `phase04/tokdump.c` | **Tokenizer + parse-tree** dumper via `#include "sqlite3.c"` | ✅ prints tokens and treetrace |
| `phase07/walparse.c` | **WAL** decoder + checksum chain verifier | ✅ validates real WALs, detects corruption |
| `phase08/seriesvtab.c` | A complete **virtual table** (eponymous, xBestIndex, table-valued function) | ✅ loads and answers queries |

## Quick build of everything

```sh
cd ~/Desktop/sqllite
A=$PWD/sqlite-amalgamation-3500400

cc -Wall -O0 -g -o labs/phase01/dbparse   labs/phase01/dbparse.c
cc -Wall      -o labs/phase02/tracevfs    labs/phase02/tracevfs.c -lsqlite3
cc -Wall -O0 -g -o labs/phase03/minibtree labs/phase03/minibtree.c
cc -O0 -g -DSQLITE_DEBUG=1 -DSQLITE_ENABLE_TREETRACE -DSQLITE_ENABLE_WHERETRACE \
   -I$A -o labs/phase04/tokdump labs/phase04/tokdump.c -lpthread -ldl -lm
cc -Wall -O0 -g -o labs/phase07/walparse  labs/phase07/walparse.c
cc -O1 -I$A -o labs/phase08/sqlite3-ext $A/shell.c $A/sqlite3.c -lpthread -ldl -lm
cc -fPIC -shared -I$A -o labs/phase08/myseries.dylib labs/phase08/seriesvtab.c
```

## Smoke test

```sh
cd ~/Desktop/sqllite/labs
phase03/minibtree seq 5000            # expect: all checks YES
phase08/sqlite3-ext :memory: ".load phase08/myseries" "SELECT value FROM myseries(1,7,2);"
```

## Notes that cost real debugging time

- **Apple's `/usr/bin/sqlite3`** sets `reserved bytes = 12` per page (upstream default is 0)
  and is built **without** `.load` support and without `SQLITE_ENABLE_STAT4`. Build your own
  shell from the amalgamation for extension and planner work.
- **A `-wal` file looks empty** unless you inspect it from inside a live session: the last
  connection to close checkpoints and truncates it.
- **WAL checksum byte order** is chosen by the magic number's low bit and is the *opposite*
  convention from the database file (which is always big-endian).
- Internal SQLite functions are `static` in the amalgamation; `#include "sqlite3.c"` into
  your own `.c` file to reach them.
