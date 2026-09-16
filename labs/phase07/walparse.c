/* walparse.c — decode and VERIFY a SQLite -wal file with no SQLite linkage.
 *
 * Build: cc -Wall -O0 -g -o walparse walparse.c
 * Usage: ./walparse mydb.db-wal
 *
 * Prints the WAL header, every frame header, marks commit frames, and
 * recomputes the checksum chain exactly as wal.c does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint32_t be32(const unsigned char *p){
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint32_t le32(const unsigned char *p){
  return ((uint32_t)p[3]<<24)|((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0];
}

/* The WAL checksum: a Fibonacci-weighted running sum over 32-bit words.
 * Mirrors walChecksumBytes() in wal.c. nByte must be a multiple of 8. */
static void walChecksum(int bigEndian, const unsigned char *a, int nByte,
                        uint32_t inCksum[2], uint32_t out[2]){
  uint32_t s1 = inCksum[0], s2 = inCksum[1];
  int i;
  for(i=0; i<nByte; i+=8){
    uint32_t x0 = bigEndian ? be32(a+i)   : le32(a+i);
    uint32_t x1 = bigEndian ? be32(a+i+4) : le32(a+i+4);
    s1 += x0 + s2;
    s2 += x1 + s1;
  }
  out[0] = s1; out[1] = s2;
}

int main(int argc, char **argv){
  FILE *f;
  long size;
  unsigned char *w;
  uint32_t magic, fmt, pgsz, ckptSeq, salt1, salt2, hc1, hc2;
  uint32_t cksum[2], out[2];
  int bigEndian, frame = 0;
  long off;

  if( argc<2 ){ fprintf(stderr, "usage: %s file.db-wal\n", argv[0]); return 1; }
  f = fopen(argv[1], "rb");
  if( !f ){ perror("open"); return 1; }
  fseek(f,0,SEEK_END); size = ftell(f); fseek(f,0,SEEK_SET);
  if( size < 32 ){ printf("WAL is empty (%ld bytes) — nothing to recover\n", size); return 0; }
  w = malloc((size_t)size);
  if( fread(w,1,(size_t)size,f)!=(size_t)size ){ perror("read"); return 1; }
  fclose(f);

  magic   = be32(w+0);
  fmt     = be32(w+4);
  pgsz    = be32(w+8);
  ckptSeq = be32(w+12);
  salt1   = be32(w+16);
  salt2   = be32(w+20);
  hc1     = be32(w+24);
  hc2     = be32(w+28);
  /* magic LSB==1 means big-endian word reads; LSB==0 means little-endian.
   * (wal.c: hdr.bigEndCksum = magic&1, then walChecksumBytes(native = (bigEndCksum==SQLITE_BIGENDIAN))) */
  bigEndian = (magic & 1);

  printf("WAL header\n");
  printf("  magic          : 0x%08x  (%s-endian checksums)%s\n", magic,
         bigEndian ? "big" : "little",
         (magic|1)==0x377f0683 ? "" : "   *** BAD MAGIC ***");
  printf("  format version : %u %s\n", fmt, fmt==3007000 ? "(ok)" : "(UNEXPECTED)");
  printf("  page size      : %u%s\n", pgsz,
         (pgsz<512 || pgsz>65536 || (pgsz&(pgsz-1))) ? "   *** INVALID ***" : "");
  if( pgsz<512 || pgsz>65536 || (pgsz&(pgsz-1)) ){
    printf("\nPage size invalid: SQLite would ignore this WAL entirely.\n");
    return 1;
  }
  printf("  ckpt sequence  : %u\n", ckptSeq);
  printf("  salt-1 / salt-2: 0x%08x / 0x%08x\n", salt1, salt2);
  printf("  header cksum   : 0x%08x / 0x%08x", hc1, hc2);

  cksum[0] = cksum[1] = 0;
  walChecksum(bigEndian, w, 24, cksum, out);
  printf("  -> computed 0x%08x / 0x%08x  %s\n", out[0], out[1],
         (out[0]==hc1 && out[1]==hc2) ? "VALID" : "*** MISMATCH ***");
  if( out[0]!=hc1 || out[1]!=hc2 ){
    printf("\nHeader checksum invalid: SQLite would ignore this WAL entirely.\n");
    return 1;
  }
  cksum[0] = hc1; cksum[1] = hc2;      /* chain starts from the header checksum */

  printf("\nframe  offset     pgno   dbsize  salt1/salt2 ok  cksum        status\n");
  printf("-----  ---------  ------  ------  --------------  -----------  ------\n");
  for(off = 32; off + 24 + (long)pgsz <= size; off += 24 + pgsz){
    unsigned char *fh = w + off;
    uint32_t pgno   = be32(fh+0);
    uint32_t dbsize = be32(fh+4);
    uint32_t s1     = be32(fh+8);
    uint32_t s2     = be32(fh+12);
    uint32_t c1     = be32(fh+16);
    uint32_t c2     = be32(fh+20);
    int saltOk = (s1==salt1 && s2==salt2);
    int ckOk;

    /* checksum covers the first 8 bytes of the frame header + the page data */
    walChecksum(bigEndian, fh, 8, cksum, out);
    walChecksum(bigEndian, fh+24, (int)pgsz, out, out);
    ckOk = (out[0]==c1 && out[1]==c2);

    frame++;
    printf("%5d  %9ld  %6u  %6u  %-14s  0x%08x   %s%s\n",
           frame, off, pgno, dbsize, saltOk ? "ok" : "SALT MISMATCH",
           c1, ckOk ? "valid" : "INVALID",
           dbsize ? "   <<< COMMIT" : "");

    if( !saltOk || !ckOk ){
      printf("\nRecovery would STOP here: frames %d..end are ignored.\n", frame);
      break;
    }
    cksum[0] = c1; cksum[1] = c2;      /* chain forward */
  }
  printf("\n%d frame(s); WAL file is %ld bytes (= 32 + n*(24+%u))\n", frame, size, pgsz);
  free(w);
  return 0;
}
