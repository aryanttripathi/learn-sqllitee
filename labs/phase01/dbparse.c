/* dbparse.c — a minimal, dependency-free SQLite database file reader.
 * Build: cc -Wall -O0 -g -o dbparse dbparse.c
 * Usage: ./dbparse file.db            (dumps header + every b-tree page)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char *g_file;      /* whole database in memory */
static long           g_size;
static int            g_pagesz;
static int            g_usable;

/* ---------- big-endian readers ---------- */
static uint32_t be16(const unsigned char *p){ return (p[0]<<8) | p[1]; }
static uint32_t be32(const unsigned char *p){
  return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* ---------- varint ---------- */
static int getVarint(const unsigned char *p, uint64_t *v){
  uint64_t x = 0;
  int i;
  for(i=0; i<8; i++){
    x = (x<<7) | (p[i] & 0x7f);
    if( (p[i] & 0x80)==0 ){ *v = x; return i+1; }
  }
  x = (x<<8) | p[8];
  *v = x;
  return 9;
}

/* ---------- page access (1-based page numbers) ---------- */
static unsigned char *page(int pgno){
  return g_file + (long)(pgno-1) * g_pagesz;
}

/* ---------- serial type helpers ---------- */
static int serialSize(uint64_t t){
  static const int sz[] = {0,1,2,3,4,6,8,8,0,0};
  if( t<10 ) return sz[t];
  return (int)((t - 12 - (t&1)) / 2);
}

static void printValue(uint64_t stype, const unsigned char *p){
  int n = serialSize(stype);
  int i;
  int64_t v = 0;
  switch( stype ){
    case 0: printf("NULL"); return;
    case 8: printf("0");    return;
    case 9: printf("1");    return;
    case 7: {                       /* IEEE754 big-endian double */
      uint64_t u = 0;
      double d;
      for(i=0;i<8;i++) u = (u<<8) | p[i];
      memcpy(&d, &u, 8);
      printf("%g", d);
      return;
    }
    default:
      if( stype>=1 && stype<=6 ){
        v = (p[0] & 0x80) ? -1 : 0;           /* sign extend */
        for(i=0;i<n;i++) v = (v<<8) | p[i];
        printf("%lld", (long long)v);
        return;
      }
      if( stype>=12 && (stype%2)==0 ){        /* BLOB */
        printf("x'");
        for(i=0;i<n;i++) printf("%02x", p[i]);
        printf("'");
        return;
      }
      printf("'%.*s'", n, (const char*)p);    /* TEXT (assumes UTF-8) */
  }
}

/* ---------- record decoder ---------- */
static void printRecord(const unsigned char *rec, int nRec){
  uint64_t hdrSize;
  int n = getVarint(rec, &hdrSize);
  const unsigned char *pHdr = rec + n;
  const unsigned char *pEnd = rec + hdrSize;
  const unsigned char *pBody = rec + hdrSize;
  int col = 0;
  (void)nRec;
  printf("(");
  while( pHdr < pEnd ){
    uint64_t stype;
    pHdr += getVarint(pHdr, &stype);
    if( col++ ) printf(", ");
    printValue(stype, pBody);
    pBody += serialSize(stype);
  }
  printf(")");
}

/* ---------- local payload size (spill formula) ---------- */
static int localPayload(uint64_t P, int isTableLeaf){
  int U = g_usable;
  int X = isTableLeaf ? (U - 35) : (((U-12)*64/255) - 23);
  int M = ((U-12)*32/255) - 23;
  int K;
  if( (int64_t)P <= X ) return (int)P;
  K = M + (int)((P - M) % (U - 4));
  return (K <= X) ? K : M;
}

/* ---------- b-tree walker ---------- */
static void walkPage(int pgno, int depth){
  unsigned char *pg = page(pgno);
  int hdrOff = (pgno==1) ? 100 : 0;
  unsigned char *h = pg + hdrOff;
  int type    = h[0];
  int nCell   = be16(h+3);
  int cellTop = be16(h+5);
  int nFrag   = h[7];
  int isLeaf  = (type==0x0a || type==0x0d);
  int cellHdr = isLeaf ? 8 : 12;
  unsigned char *ptrs = h + cellHdr;
  int i;
  const char *tname =
      type==0x02 ? "interior index" :
      type==0x05 ? "interior table" :
      type==0x0a ? "leaf index"     :
      type==0x0d ? "leaf table"     : "??? NOT A BTREE PAGE";

  printf("%*spage %d: %s  nCell=%d cellTop=%d frag=%d",
         depth*2, "", pgno, tname, nCell, cellTop?cellTop:65536, nFrag);
  if( !isLeaf ) printf(" rightChild=%u", be32(h+8));
  printf("\n");
  if( type!=0x02 && type!=0x05 && type!=0x0a && type!=0x0d ) return;

  for(i=0; i<nCell; i++){
    int off = be16(ptrs + i*2);
    unsigned char *c = pg + off;
    uint64_t P, rowid;
    int nv, local;

    if( type==0x05 ){                       /* interior table: child + key */
      uint32_t child = be32(c);
      getVarint(c+4, &rowid);
      printf("%*s  cell[%d] child=%u key<=%llu\n",
             depth*2, "", i, child, (unsigned long long)rowid);
      walkPage((int)child, depth+1);
      continue;
    }
    if( type==0x02 ){                       /* interior index */
      uint32_t child = be32(c);
      nv = getVarint(c+4, &P);
      local = localPayload(P, 0);
      printf("%*s  cell[%d] child=%u P=%llu ", depth*2, "", i, child,
             (unsigned long long)P);
      printRecord(c+4+nv, local);
      printf("%s\n", (int64_t)P>local ? "  [+overflow]" : "");
      walkPage((int)child, depth+1);
      continue;
    }
    if( type==0x0d ){                       /* leaf table: P, rowid, payload */
      nv  = getVarint(c, &P);
      nv += getVarint(c+nv, &rowid);
      local = localPayload(P, 1);
      printf("%*s  cell[%d] off=%d P=%llu rowid=%llu ",
             depth*2, "", i, off, (unsigned long long)P,
             (unsigned long long)rowid);
      printRecord(c+nv, local);
      if( (int64_t)P > local ){
        printf("  [overflow -> page %u]", be32(c+nv+local));
      }
      printf("\n");
    } else {                                /* 0x0a leaf index */
      nv = getVarint(c, &P);
      local = localPayload(P, 0);
      printf("%*s  cell[%d] off=%d P=%llu key=", depth*2, "", i, off,
             (unsigned long long)P);
      printRecord(c+nv, local);
      printf("%s\n", (int64_t)P>local ? "  [+overflow]" : "");
    }
  }
  if( !isLeaf ) walkPage((int)be32(h+8), depth+1);
}

static void dumpHeader(void){
  unsigned char *h = g_file;
  printf("magic          : %.15s\n", h);
  printf("page size      : %d\n", g_pagesz);
  printf("write/read ver : %d / %d  (%s)\n", h[18], h[19],
         h[18]==2 ? "WAL" : "rollback journal");
  printf("reserved bytes : %d   (usable = %d)\n", h[20], g_usable);
  printf("change counter : %u\n", be32(h+24));
  printf("db size (pages): %u   (file has %ld)\n", be32(h+28), g_size/g_pagesz);
  printf("freelist trunk : %u   count=%u\n", be32(h+32), be32(h+36));
  printf("schema cookie  : %u   format=%u\n", be32(h+40), be32(h+44));
  printf("largest root pg: %u   (auto-vacuum if non-zero)\n", be32(h+52));
  printf("text encoding  : %u   (1=utf8 2=utf16le 3=utf16be)\n", be32(h+56));
  printf("user_version   : %u\n", be32(h+60));
  printf("incr-vacuum    : %u\n", be32(h+64));
  printf("application_id : %u\n", be32(h+68));
  printf("version-valid  : %u\n", be32(h+92));
  printf("written by lib : %u\n", be32(h+96));
  printf("\n");
}

int main(int argc, char **argv){
  FILE *f;
  int rootpg = 1;
  if( argc<2 ){ fprintf(stderr,"usage: %s file.db [rootpage]\n", argv[0]); return 1; }
  if( argc>2 ) rootpg = atoi(argv[2]);

  f = fopen(argv[1], "rb");
  if( !f ){ perror("open"); return 1; }
  fseek(f, 0, SEEK_END); g_size = ftell(f); fseek(f, 0, SEEK_SET);
  g_file = malloc(g_size);
  if( fread(g_file, 1, g_size, f)!=(size_t)g_size ){ perror("read"); return 1; }
  fclose(f);

  g_pagesz = be16(g_file+16);
  if( g_pagesz==1 ) g_pagesz = 65536;
  g_usable = g_pagesz - g_file[20];

  dumpHeader();
  printf("=== walking b-tree rooted at page %d ===\n", rootpg);
  walkPage(rootpg, 0);
  return 0;
}
