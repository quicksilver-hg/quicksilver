#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "bitmap.hpp"
#include "compress.hpp"
#include <new>

typedef word_t proof[PROOFSIZE];

// cuck(at)oo graph with given limit on number of edges (and on single partition nodes)
template <typename word_t>
class graph {
public:
  // terminates adjacency lists
  const word_t NIL = ~(word_t)0;	// NOTE: matches last edge when EDGEBITS==32

  struct link { // element of adjacency list
    word_t next;
    word_t to;
  };

  word_t MAXEDGES;
  word_t MAXNODES;
  word_t nlinks; // aka halfedges, twice number of edges
  word_t *adjlist; // index into links array
  link *links;
  bool sharedmem;
  compressor<word_t> *compressu;
  compressor<word_t> *compressv;
  bitmap<u32> visited;
  u32 MAXSOLS;
  proof *sols;
  u32 nsols;

  // Quicksilver (F-265). links[] holds 2*MAXEDGES halfedges and the vendored add_edge
  // never bounded nlinks, so a graph with more than MAXEDGES edges left after trimming
  // wrote past links[] into the compressor tables that follow it in the shared buffer,
  // and a full compressor merged unrelated nodes into bogus cycles. CI's ASan leg saw
  // only the SEGV at the end of that chain. Owner rulings: log the overflow once per
  // graph, with what is needed to replay the graph offline (2026-10-01); refuse the edge
  // and abandon the graph instead of writing past it (2026-10-03) -- the caller yields no
  // solutions for an abandoned graph. The caller sets the replay fields after reset();
  // f265_out == nullptr means stderr, unless this translation unit included the
  // public cuckatoo.h, in which case it means the sink the node installed
  // (debug.log, F-414) or stderr if none is installed.
  const char *f265_hdr = nullptr;
  u32 f265_len = 0, f265_nonce = 0, f265_nthreads = 0;
  FILE *f265_out = nullptr;
  u32 f265_logged = 0; // one bit per kind, cleared by reset()
  static constexpr u32 F265_LINK_OVERFLOW = 1, F265_NODE_OVERFLOW = 2, F265_SKIP = 4;
  bool abandoned = false; // an edge did not fit; cleared by reset()

  void f265_capture(u32 kind, const char *what, unsigned long long a, unsigned long long b) {
    if (f265_logged & kind)
      return;
    f265_logged |= kind;
    char prefix[256]; // the fields below are bounded, the header is not
    int n = snprintf(prefix, sizeof(prefix),
                     "F265-CAPTURE %s a=%llu b=%llu nlinks=%llu MAXEDGES=%llu MAXNODES=%llu EDGEBITS=%d nthreads=%u nonce=%u len=%u hdr=",
                     what, a, b, (unsigned long long)nlinks, (unsigned long long)MAXEDGES,
                     (unsigned long long)MAXNODES, (int)EDGEBITS, f265_nthreads, f265_nonce, f265_len);
    if (n < 0 || n >= (int)sizeof(prefix))
      return;
    // F-413: the line holds the whole header, or the capture cannot be replayed (a vault
    // tx-PoW header is ~12 KB; a fixed 1 KB line once kept 447 of 12,523 bytes). The capture
    // runs before this graph's out-of-bounds write, so the heap is still sound. If even so
    // the allocation fails, write what fits and say so, so a short line never reads as whole.
    static const char HEX[] = "0123456789abcdef";
    const u32 hdrlen = f265_hdr ? f265_len : 0;
    char fallback[1024];
    char *line = (char *)malloc((size_t)n + 2 * (size_t)hdrlen + 1);
    size_t room = line ? (size_t)n + 2 * (size_t)hdrlen + 1 : sizeof(fallback) - sizeof(" hdr_truncated=1");
    if (!line)
      line = fallback;
    memcpy(line, prefix, n);
    size_t len = n;
    u32 i = 0;
    for (; i < hdrlen && len + 2 < room; i++) {
      line[len++] = HEX[(unsigned char)f265_hdr[i] >> 4];
      line[len++] = HEX[(unsigned char)f265_hdr[i] & 15];
    }
    if (i < hdrlen)
      len += snprintf(line + len, sizeof(fallback) - len, " hdr_truncated=1");
    line[len] = 0;
    // F-418. solve_19.cpp, solve_28.cpp, the capture test and the bench template
    // include crypto/cuckatoo/cuckatoo.h at global scope before this header, which
    // declares ::cuckatoo::GetF265CaptureSink and defines
    // QUICKSILVER_CRYPTO_CUCKATOO_CUCKATOO_H. lean.hpp includes the vendored
    // cuckatoo.h, a different file, so the solver chain does not provide that
    // declaration. The GPU helper (lean.cu) and a standalone build of
    // vendor/lean.cpp never include the public header and do not link
    // dispatch.cpp. The include guard is set exactly when the declaration is
    // visible, so neither build grows a -D a builder can forget. __CUDACC__ would
    // fix the helper and miss that CPU standalone build. With the guard set, the
    // line goes to the installed sink, or to f265_out / stderr when there is
    // none (F-414). Without it, the line goes to f265_out or stderr, as it did
    // before F-414.
#ifdef QUICKSILVER_CRYPTO_CUCKATOO_CUCKATOO_H
    // F-414: the node's sink writes to debug.log, unbuffered, in one write.
    ::cuckatoo::F265CaptureSink sink = f265_out ? nullptr : ::cuckatoo::GetF265CaptureSink();
    if (sink) {
      sink(line);
    } else {
      FILE *out = f265_out ? f265_out : stderr;
      fprintf(out, "%s\n", line); // one write, so a crash right after still leaves the whole line
      fflush(out);
    }
#else
    FILE *out = f265_out ? f265_out : stderr;
    fprintf(out, "%s\n", line); // one write, so a crash right after still leaves the whole line
    fflush(out);
#endif
    if (line != fallback)
      free(line);
  }

  graph(word_t maxedges, word_t maxnodes, u32 maxsols, u32 compressbits) : visited(maxedges) {
    MAXEDGES = maxedges;
    MAXNODES = maxnodes;
    MAXSOLS = maxsols;
    adjlist = new word_t[2*MAXNODES]; // index into links array
    links   = new link[2*MAXEDGES];
    compressu = compressbits ? new compressor<word_t>(EDGEBITS, compressbits) : 0;
    compressv = compressbits ? new compressor<word_t>(EDGEBITS, compressbits) : 0;
    sharedmem = false;
    sols    = new proof[MAXSOLS+1]; // extra one for current path
    visited.clear();
  }

  ~graph() {
    if (!sharedmem) {
      delete[] adjlist;
      delete[] links;
    }
    // Quicksilver: both constructors `new` these and the vendored destructor
    // freed neither, leaking one compressor object per partition per graph.
    // `sharedmem` covers this graph's own adjlist/links only -- compressor's
    // destructor already knows whether its node array was placement-new'd into
    // borrowed memory, so deleting here is correct for both constructors.
    delete compressu;
    delete compressv;
    delete[] sols;
  }

  graph(word_t maxedges, word_t maxnodes, u32 maxsols, u32 compressbits, char *bytes) : visited(maxedges) {
    MAXEDGES = maxedges;
    MAXNODES = maxnodes;
    MAXSOLS = maxsols;
    adjlist = new (bytes) word_t[2*MAXNODES]; // index into links array
    links   = new (bytes += (2 * MAXNODES * sizeof(word_t))) link[2*MAXEDGES];
    compressu = compressbits ? new compressor<word_t>(EDGEBITS, compressbits, bytes += (2 * MAXEDGES * sizeof(link))) : 0;
    compressv = compressbits ? new compressor<word_t>(EDGEBITS, compressbits, bytes + compressu->bytes()) : 0;
    sharedmem = true;
    sols    = new  proof[MAXSOLS+1];
    visited.clear();
  }

  // total size of new-operated data, excludes sols and visited bitmap of MAXEDGES bits
  uint64_t bytes() {
    assert(2*MAXNODES != 0 && 2*MAXEDGES != 0); // allocation fails for uncompressed EDGEBITS=31
    return (2 * MAXNODES * sizeof(word_t)) + (2 * MAXEDGES * sizeof(link)) + (compressu ? 2 * compressu->bytes() : 0);
  }

  void reset() {
    memset(adjlist, (char)NIL, (2 * MAXNODES * sizeof(word_t)));
    if (compressu) {
      compressu->reset();
      compressv->reset();
    }
    f265_logged = 0;
    abandoned = false;
    resetcounts();
  }

  void resetcounts() {
    nlinks = nsols = 0;
    // visited has entries set only during cycles() call
  }

  static int nonce_cmp(const void *a, const void *b) {
    u32 x = *(u32 *)a, y = *(u32 *)b;
    // printf("nonce_cmp %x %x\n", x, y);
    return x < y ? -1 : x > y;
  }

  void cycles_with_link(u32 len, word_t u, word_t dest) {
    // assert((u>>1) < MAXEDGES);
    if (visited.test(u >> 1))
      return;
    if ((u ^ 1) == dest) {
      print_log("  %d-cycle found\n", len);
      if (len == PROOFSIZE && nsols < MAXSOLS) {
        memcpy(sols[nsols+1], sols[nsols], sizeof(sols[0]));
        qsort(sols[nsols++], PROOFSIZE, sizeof(word_t), nonce_cmp);
      }
      return;
    }
    if (len == PROOFSIZE)
      return;
    word_t au1 = adjlist[u ^ 1];
    if (au1 != NIL) {
      visited.set(u >> 1);
      for (; au1 != NIL; au1 = links[au1].next) {
        sols[nsols][len] = au1/2;
        cycles_with_link(len+1, links[au1 ^ 1].to, dest);
      }
      visited.reset(u >> 1);
    }
  }

  bool add_edge(word_t u, word_t v) {
    if ((unsigned long long)nlinks + 2 > 2ULL * MAXEDGES) {
      f265_capture(F265_LINK_OVERFLOW, "LINK OVERFLOW", u, v);
      abandoned = true;
      return false;
    }
    assert(u < MAXNODES);
    assert(v < MAXNODES);
    v += MAXNODES; // distinguish partitions
    if (adjlist[u ^ 1] != NIL && adjlist[v ^ 1] != NIL) { // possibly part of a cycle
      sols[nsols][0] = nlinks/2;
      assert(!visited.test(u >> 1));
      cycles_with_link(1, u, v);
    }
    word_t ulink = nlinks++;
    word_t vlink = nlinks++; // the two halfedges of an edge differ only in last bit
    assert(vlink != NIL);    // avoid confusing links with NIL (possible if word_t is u32 and EDGEBITS is 31 or 32)
#ifndef ALLOWDUPES
    for (word_t au = adjlist[u]; au != NIL; au = links[au].next)
      if (links[au ^ 1].to == v) return false; // drop duplicate edge
#endif
    links[ulink].next = adjlist[u];
    links[vlink].next = adjlist[v];
    links[adjlist[u] = ulink].to = u;
    links[adjlist[v] = vlink].to = v;
    return true;
  }

  bool add_compress_edge(word_t u, word_t v) {
    word_t cu = compressu->compress(u), cv = compressv->compress(v);
    if (compressu->overflows || compressv->overflows) {
      f265_capture(F265_NODE_OVERFLOW, "NODE OVERFLOW", compressu->npairs, compressv->npairs);
      abandoned = true;
      return false;
    }
    return add_edge(cu, cv);
  }
};
