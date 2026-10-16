// Cuck(at)oo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2020 John Tromp
// OpenCL C 1.2 port of the count_node_deg and kill_leaf_edges kernels of lean.cu.
// Offered under the same two licences as lean.cu: see LICENSE.txt and README.md.
#define NEDGES ((uint)1 << EDGEBITS)
#define NODEMASK (NEDGES - 1)

inline ulong rotl64(ulong a, uint o) { return rotate(a, (ulong)o); }
inline uint dipnode(ulong k0, ulong k1, ulong k2, ulong k3, uint edge, uint uorv) {
  ulong v0 = k0, v1 = k1, v2 = k2, v3 = k3;
  const ulong nonce = ((ulong)edge << 1) | uorv;
#define ROUND v0 += v1; v2 += v3; v1 = rotl64(v1,13); \
  v3 = rotl64(v3,16); v1 ^= v0; v3 ^= v2; \
  v0 = rotl64(v0,32); v2 += v1; v0 += v3; \
  v1 = rotl64(v1,17); v3 = rotl64(v3,21); \
  v1 ^= v2; v3 ^= v0; v2 = rotl64(v2,32);
  v3 ^= nonce; ROUND ROUND v0 ^= nonce; v2 ^= 0xff; ROUND ROUND ROUND ROUND
#undef ROUND
  return (uint)((v0 ^ v1 ^ v2 ^ v3) & NODEMASK);
}

// 1-based find-first-set; x != 0. OpenCL 1.2 has clz but not ctz.
inline uint ffs1(uint x) { return 32 - clz(x & (0u - x)); }
// OpenCL takes shift counts modulo 32, so x >> 32 is x, not 0.
inline uint shr_clamped(uint x, uint s) { return s >= 32 ? 0u : x >> s; }

__kernel void count_node_deg(ulong k0, ulong k1, ulong k2, ulong k3,
                             __global const uint* alive, __global uint* nonleaf, uint uorv) {
  const uint nthreads = get_global_size(0), id = get_global_id(0);
  for (uint block = id * 32; block < NEDGES; block += nthreads * 32) {
    uint alive32 = ~alive[block / 32];
    for (uint nonce = block - 1; alive32; ) {
      const uint f = ffs1(alive32); nonce += f; alive32 = shr_clamped(alive32, f);
      const uint u = dipnode(k0, k1, k2, k3, nonce, uorv);
      atomic_or(&nonleaf[u / 32], 1u << (u % 32));
    }
  }
}

__kernel void kill_leaf_edges(ulong k0, ulong k1, ulong k2, ulong k3,
                              __global uint* alive, __global const uint* nonleaf, uint uorv) {
  const uint nthreads = get_global_size(0), id = get_global_id(0);
  for (uint block = id * 32; block < NEDGES; block += nthreads * 32) {
    uint alive32 = ~alive[block / 32];
    for (uint nonce = block - 1; alive32; ) {
      const uint f = ffs1(alive32); nonce += f; alive32 = shr_clamped(alive32, f);
      const uint u = dipnode(k0, k1, k2, k3, nonce, uorv) ^ 1;
      if (!((nonleaf[u / 32] >> (u % 32)) & 1)) alive[nonce / 32] |= 1u << (nonce % 32);
    }
  }
}
