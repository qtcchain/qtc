#include <metal_stdlib>
using namespace metal;

// QTC MatMul proof-of-work Metal kernels.
//
// Consensus references (device output must stay bit-identical):
//   * oracle v2          -> src/matmul/field.cpp (from_oracle / from_oracle_block)
//   * product digest v4  -> src/matmul/transcript.cpp (HashProductTile,
//                           HashProductTileHashes, FinalizeProductCommittedDigestFromHash)
//
// The device produces the perturbed operands A' = A + E_L*E_R, B' = B + F_L*F_R,
// the full product C' = A'*B' over GF(2^31-1), and the N*N per-tile SHA-256
// hashes of C' (row-major LE32 elements, b*b elements per tile). The root hash
// over the tile hashes and the tagged outer SHA256d are finished on the host.

constant uint MODULUS = 0x7fffffffu;
constant uint MAX_BLOCK_ELEMENTS = 256u;
constant uint PRODUCT_TILE_DIM = 16u;
constant uint FC_SPEC_N [[function_constant(0)]];
constant uint FC_SPEC_B [[function_constant(1)]];
constant uint FC_SPEC_R [[function_constant(2)]];
constant uint FC_SPEC_NBLOCKS [[function_constant(3)]];

struct KernelParams {
    uint n;
    uint b;
    uint r;
    uint N;
};

// ---------------------------------------------------------------------------
// GF(2^31 - 1) arithmetic (mirrors matmul::field)
// ---------------------------------------------------------------------------

inline uint reduce64(ulong x)
{
    const ulong fold1 = (x & (ulong)MODULUS) + (x >> 31);
    const uint lo = (uint)(fold1 & (ulong)MODULUS);
    const uint hi = (uint)(fold1 >> 31);
    uint result = lo + hi;
    const uint ge_mask = result >= MODULUS ? 0xffffffffu : 0u;
    result -= MODULUS & ge_mask;
    return result;
}

inline uint add_mod(uint a, uint b)
{
    uint s = a + b;
    const uint ge_mask = s >= MODULUS ? 0xffffffffu : 0u;
    s -= MODULUS & ge_mask;
    return s;
}

inline uint mul_mod(uint a, uint b)
{
    return reduce64((ulong)a * (ulong)b);
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

inline uint rotr(uint x, uint n)
{
    return (x >> n) | (x << (32u - n));
}

inline uint sha_ch(uint x, uint y, uint z)
{
    return (x & y) ^ ((~x) & z);
}

inline uint sha_maj(uint x, uint y, uint z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint sha_bsig0(uint x)
{
    return rotr(x, 2u) ^ rotr(x, 13u) ^ rotr(x, 22u);
}

inline uint sha_bsig1(uint x)
{
    return rotr(x, 6u) ^ rotr(x, 11u) ^ rotr(x, 25u);
}

inline uint sha_ssig0(uint x)
{
    return rotr(x, 7u) ^ rotr(x, 18u) ^ (x >> 3u);
}

inline uint sha_ssig1(uint x)
{
    return rotr(x, 17u) ^ rotr(x, 19u) ^ (x >> 10u);
}

constant uint SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline void sha256_init(thread uint state[8])
{
    state[0] = 0x6a09e667u;
    state[1] = 0xbb67ae85u;
    state[2] = 0x3c6ef372u;
    state[3] = 0xa54ff53au;
    state[4] = 0x510e527fu;
    state[5] = 0x9b05688cu;
    state[6] = 0x1f83d9abu;
    state[7] = 0x5be0cd19u;
}

inline void sha256_compress(thread uint state[8], thread uint w[64])
{
    for (uint t = 16; t < 64; ++t) {
        w[t] = sha_ssig1(w[t - 2]) + w[t - 7] + sha_ssig0(w[t - 15]) + w[t - 16];
    }

    uint a = state[0];
    uint b = state[1];
    uint c = state[2];
    uint d = state[3];
    uint e = state[4];
    uint f = state[5];
    uint g = state[6];
    uint h = state[7];

    for (uint t = 0; t < 64; ++t) {
        const uint t1 = h + sha_bsig1(e) + sha_ch(e, f, g) + SHA256_K[t] + w[t];
        const uint t2 = sha_bsig0(a) + sha_maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

inline void set_msg_byte(thread uint w[64], uint offset, uint byte)
{
    const uint word_index = offset >> 2u;
    const uint shift = (3u - (offset & 3u)) * 8u;
    w[word_index] |= (byte & 0xffu) << shift;
}

inline uint bswap32(uint x)
{
    return ((x & 0x000000ffu) << 24u) |
           ((x & 0x0000ff00u) << 8u) |
           ((x & 0x00ff0000u) >> 8u) |
           ((x & 0xff000000u) >> 24u);
}

// ---------------------------------------------------------------------------
// Oracle v2 (matmul::field::from_oracle / from_oracle_block)
//
//   block = index >> 3, lane = index & 7
//   H = SHA-256(seed_canonical || LE32(block) [|| LE32(retry) if retry > 0])
//   candidate = LE32(H[4*lane .. 4*lane+3]) & 0x7FFFFFFF
//   candidate == 0x7FFFFFFF is rejected; retry 1..255 re-hash with the SAME lane.
//   After 256 rejections: LE32(SHA-256(seed_canonical || LE32(block) ||
//   "oracle-fallback")[lane]) mod M31.
//
// `seed_internal` is the uint256 internal byte order; the canonical seed
// bytes are its reverse (seed.data()[31 - i]).
// ---------------------------------------------------------------------------

inline void oracle_block_state(constant uchar* seed_internal, uint block, uint retry, thread uint state[8])
{
    thread uint w[64];
    for (uint i = 0; i < 64; ++i) {
        w[i] = 0u;
    }

    for (uint i = 0; i < 32; ++i) {
        set_msg_byte(w, i, seed_internal[31u - i]);
    }

    set_msg_byte(w, 32u, block & 0xffu);
    set_msg_byte(w, 33u, (block >> 8u) & 0xffu);
    set_msg_byte(w, 34u, (block >> 16u) & 0xffu);
    set_msg_byte(w, 35u, (block >> 24u) & 0xffu);

    uint message_len = 36u;
    if (retry > 0u) {
        set_msg_byte(w, 36u, retry & 0xffu);
        set_msg_byte(w, 37u, (retry >> 8u) & 0xffu);
        set_msg_byte(w, 38u, (retry >> 16u) & 0xffu);
        set_msg_byte(w, 39u, (retry >> 24u) & 0xffu);
        message_len = 40u;
    }

    set_msg_byte(w, message_len, 0x80u);
    w[15] = message_len * 8u;

    sha256_init(state);
    sha256_compress(state, w);
}

inline uint oracle_lane_candidate(thread const uint state[8], uint lane)
{
    // state[lane] holds hash bytes 4*lane..4*lane+3 big-endian; LE32 of those
    // bytes is the byte-swapped word.
    return bswap32(state[lane]) & MODULUS;
}

inline uint oracle_fallback_lane(constant uchar* seed_internal, uint block, uint lane)
{
    thread uint w[64];
    for (uint i = 0; i < 64; ++i) {
        w[i] = 0u;
    }

    for (uint i = 0; i < 32; ++i) {
        set_msg_byte(w, i, seed_internal[31u - i]);
    }

    set_msg_byte(w, 32u, block & 0xffu);
    set_msg_byte(w, 33u, (block >> 8u) & 0xffu);
    set_msg_byte(w, 34u, (block >> 16u) & 0xffu);
    set_msg_byte(w, 35u, (block >> 24u) & 0xffu);

    const uchar fallback_tag[15] = {
        'o', 'r', 'a', 'c', 'l', 'e', '-', 'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k'
    };
    for (uint i = 0; i < 15; ++i) {
        set_msg_byte(w, 36u + i, fallback_tag[i]);
    }

    set_msg_byte(w, 51u, 0x80u);
    w[15] = 51u * 8u;

    thread uint state[8];
    sha256_init(state);
    sha256_compress(state, w);
    return bswap32(state[lane]) % MODULUS;
}

inline uint oracle_lane_with_retries(constant uchar* seed_internal, uint block, uint lane, uint first_retry)
{
    for (uint retry = first_retry; retry < 256u; ++retry) {
        thread uint state[8];
        oracle_block_state(seed_internal, block, retry, state);
        const uint candidate = oracle_lane_candidate(state, lane);
        if (candidate < MODULUS) {
            return candidate;
        }
    }
    return oracle_fallback_lane(seed_internal, block, lane);
}

// All eight lanes of one oracle block (== from_oracle(seed, 8*block + lane)).
inline void oracle_fill_block(constant uchar* seed_internal, uint block, thread uint out[8])
{
    thread uint state[8];
    oracle_block_state(seed_internal, block, 0u, state);
    for (uint lane = 0; lane < 8u; ++lane) {
        const uint candidate = oracle_lane_candidate(state, lane);
        out[lane] = candidate < MODULUS
            ? candidate
            : oracle_lane_with_retries(seed_internal, block, lane, 1u);
    }
}

// One thread per oracle block (8 elements). Grid = ceil(n*n / 8).
kernel void generate_base_matrix_from_seed(
    constant KernelParams& p [[buffer(0)]],
    constant uchar* seed_internal [[buffer(1)]],
    device uint* output [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    const uint nn = p.n * p.n;
    const uint base = gid * 8u;
    if (base >= nn) {
        return;
    }
    thread uint lanes[8];
    oracle_fill_block(seed_internal, gid, lanes);
    for (uint lane = 0; lane < 8u; ++lane) {
        const uint index = base + lane;
        if (index < nn) {
            output[index] = lanes[lane];
        }
    }
}

// ---------------------------------------------------------------------------
// Perturbed operands: A' = A + E_L*E_R, B' = B + F_L*F_R
// ---------------------------------------------------------------------------

kernel void build_perturbed(
    constant KernelParams& p [[buffer(0)]],
    device const uint* matrix_a [[buffer(1)]],
    device const uint* matrix_b [[buffer(2)]],
    device const uint* e_l [[buffer(3)]],
    device const uint* e_r [[buffer(4)]],
    device const uint* f_l [[buffer(5)]],
    device const uint* f_r [[buffer(6)]],
    device uint* a_prime [[buffer(7)]],
    device uint* b_prime [[buffer(8)]],
    uint gid [[thread_position_in_grid]])
{
    const uint nn = p.n * p.n;
    if (gid >= nn) {
        return;
    }

    const uint row = gid / p.n;
    const uint col = gid - row * p.n;

    uint e_acc = 0;
    uint f_acc = 0;
    for (uint k = 0; k < p.r; ++k) {
        const uint el = e_l[row * p.r + k];
        const uint er = e_r[k * p.n + col];
        e_acc = add_mod(e_acc, mul_mod(el, er));

        const uint fl = f_l[row * p.r + k];
        const uint fr = f_r[k * p.n + col];
        f_acc = add_mod(f_acc, mul_mod(fl, fr));
    }

    a_prime[gid] = add_mod(matrix_a[gid], e_acc);
    b_prime[gid] = add_mod(matrix_b[gid], f_acc);
}

kernel void build_perturbed_specialized(
    constant KernelParams& p [[buffer(0)]],
    device const uint* matrix_a [[buffer(1)]],
    device const uint* matrix_b [[buffer(2)]],
    device const uint* e_l [[buffer(3)]],
    device const uint* e_r [[buffer(4)]],
    device const uint* f_l [[buffer(5)]],
    device const uint* f_r [[buffer(6)]],
    device uint* a_prime [[buffer(7)]],
    device uint* b_prime [[buffer(8)]],
    uint gid [[thread_position_in_grid]])
{
    const uint n = FC_SPEC_N;
    const uint r = FC_SPEC_R;
    if (p.n != n || p.r != r) {
        return;
    }

    const uint nn = n * n;
    if (gid >= nn) {
        return;
    }

    const uint row = gid / n;
    const uint col = gid - row * n;

    uint e_acc = 0;
    uint f_acc = 0;
    for (uint k = 0; k < FC_SPEC_R; ++k) {
        const uint el = e_l[row * r + k];
        const uint er = e_r[k * n + col];
        e_acc = add_mod(e_acc, mul_mod(el, er));

        const uint fl = f_l[row * r + k];
        const uint fr = f_r[k * n + col];
        f_acc = add_mod(f_acc, mul_mod(fl, fr));
    }

    a_prime[gid] = add_mod(matrix_a[gid], e_acc);
    b_prime[gid] = add_mod(matrix_b[gid], f_acc);
}

// ---------------------------------------------------------------------------
// Product C' = A' * B' (full matrix, row-major). The result is exact modulo
// 2^31-1 whatever the accumulation order, so every kernel below is consensus
// equivalent to matmul::Matrix::operator*.
//
// Four 62-bit products plus a reduced carry fit in 64 bits:
//   2^31 + 4 * (2^31 - 1)^2 < 2^64
// ---------------------------------------------------------------------------

inline ulong dot4_accumulate(ulong acc, uint a, uint b, thread uint& pending)
{
    acc += (ulong)a * (ulong)b;
    if (++pending == 4u) {
        acc = reduce64(acc);
        pending = 0u;
    }
    return acc;
}

// One threadgroup per b x b output tile, one thread per element. Works for any
// b with b*b <= MAX_BLOCK_ELEMENTS and any n divisible by b.
kernel void build_product(
    constant KernelParams& p [[buffer(0)]],
    device const uint* a_prime [[buffer(1)]],
    device const uint* b_prime [[buffer(2)]],
    device uint* c_prime [[buffer(3)]],
    uint tid [[thread_index_in_threadgroup]],
    uint2 tgid [[threadgroup_position_in_grid]])
{
    const uint block_elements = p.b * p.b;
    if (block_elements == 0 || block_elements > MAX_BLOCK_ELEMENTS || tid >= block_elements) {
        return;
    }

    const uint tile_i = tgid.y;
    const uint tile_j = tgid.x;
    if (tile_i >= p.N || tile_j >= p.N) {
        return;
    }

    const uint br = tid / p.b;
    const uint bc = tid - br * p.b;
    const uint row = tile_i * p.b + br;
    const uint col = tile_j * p.b + bc;

    ulong acc = 0;
    uint pending = 0;
    for (uint k = 0; k < p.n; ++k) {
        acc = dot4_accumulate(acc, a_prime[row * p.n + k], b_prime[k * p.n + col], pending);
    }
    c_prime[row * p.n + col] = reduce64(acc);
}

kernel void build_product_specialized(
    constant KernelParams& p [[buffer(0)]],
    device const uint* a_prime [[buffer(1)]],
    device const uint* b_prime [[buffer(2)]],
    device uint* c_prime [[buffer(3)]],
    uint tid [[thread_index_in_threadgroup]],
    uint2 tgid [[threadgroup_position_in_grid]])
{
    const uint n = FC_SPEC_N;
    const uint b = FC_SPEC_B;
    const uint N = FC_SPEC_NBLOCKS;
    if (p.n != n || p.b != b || p.N != N) {
        return;
    }

    const uint block_elements = FC_SPEC_B * FC_SPEC_B;
    if (block_elements == 0 || block_elements > MAX_BLOCK_ELEMENTS || tid >= block_elements) {
        return;
    }

    const uint tile_i = tgid.y;
    const uint tile_j = tgid.x;
    if (tile_i >= N || tile_j >= N) {
        return;
    }

    const uint br = tid / b;
    const uint bc = tid - br * b;
    const uint row = tile_i * b + br;
    const uint col = tile_j * b + bc;

    ulong acc = 0;
    uint pending = 0;
    for (uint ell = 0; ell < FC_SPEC_NBLOCKS; ++ell) {
        const uint k_base = ell * b;
        for (uint k = 0; k < FC_SPEC_B; ++k) {
            acc = dot4_accumulate(acc, a_prime[row * n + (k_base + k)], b_prime[(k_base + k) * n + col], pending);
        }
    }
    c_prime[row * n + col] = reduce64(acc);
}

// 16x16 threadgroup-memory tiled variant: requires n % 16 == 0 (independent of
// the transcript block size b). Grid = (n/16, n/16) threadgroups of 16x16.
kernel void build_product_tiled(
    constant KernelParams& p [[buffer(0)]],
    device const uint* a_prime [[buffer(1)]],
    device const uint* b_prime [[buffer(2)]],
    device uint* c_prime [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]],
    uint2 tid [[thread_position_in_threadgroup]])
{
    const uint n = p.n;
    if ((n % PRODUCT_TILE_DIM) != 0u) {
        return;
    }
    const uint row = gid.y;
    const uint col = gid.x;
    if (row >= n || col >= n) {
        return;
    }

    threadgroup uint tile_a[PRODUCT_TILE_DIM][PRODUCT_TILE_DIM];
    threadgroup uint tile_b[PRODUCT_TILE_DIM][PRODUCT_TILE_DIM];

    ulong acc = 0;
    uint pending = 0;
    const uint k_tiles = n / PRODUCT_TILE_DIM;
    for (uint kt = 0; kt < k_tiles; ++kt) {
        const uint k_base = kt * PRODUCT_TILE_DIM;
        tile_a[tid.y][tid.x] = a_prime[row * n + (k_base + tid.x)];
        tile_b[tid.y][tid.x] = b_prime[(k_base + tid.y) * n + col];
        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint k = 0; k < PRODUCT_TILE_DIM; ++k) {
            acc = dot4_accumulate(acc, tile_a[tid.y][k], tile_b[k][tid.x], pending);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    c_prime[row * n + col] = reduce64(acc);
}

// ---------------------------------------------------------------------------
// Product digest v4 tile hashes: one thread per b x b tile of C'. The message
// is the tile's elements row-major as LE32 (4*b*b bytes); output is the raw
// 32-byte SHA-256 digest at tile_hashes[32 * (tile_i * N + tile_j)].
// (matmul::transcript::HashProductTile)
// ---------------------------------------------------------------------------

kernel void hash_product_tiles(
    constant KernelParams& p [[buffer(0)]],
    device const uint* c_prime [[buffer(1)]],
    device uchar* tile_hashes [[buffer(2)]],
    uint gid [[thread_position_in_grid]])
{
    const uint tile_count = p.N * p.N;
    if (gid >= tile_count) {
        return;
    }

    const uint tile_i = gid / p.N;
    const uint tile_j = gid - tile_i * p.N;
    const uint row_base = tile_i * p.b;
    const uint col_base = tile_j * p.b;

    const uint msg_len_bytes = p.b * p.b * 4u;
    const uint total_blocks = (msg_len_bytes + 9u + 63u) / 64u;
    const uint total_bytes = total_blocks * 64u;
    const ulong bit_len = (ulong)msg_len_bytes * 8u;

    thread uint state[8];
    sha256_init(state);

    thread uint w[64];
    for (uint block = 0; block < total_blocks; ++block) {
        for (uint i = 0; i < 16; ++i) {
            const uint off = block * 64u + i * 4u;
            uint word = 0u;
            if (off < msg_len_bytes) {
                const uint element = off >> 2u;
                const uint er = element / p.b;
                const uint ec = element - er * p.b;
                // LE32 bytes of the element form a big-endian message word.
                word = bswap32(c_prime[(row_base + er) * p.n + (col_base + ec)]);
            } else if (off == msg_len_bytes) {
                word = 0x80000000u;
            } else if (off == total_bytes - 8u) {
                word = (uint)(bit_len >> 32u);
            } else if (off == total_bytes - 4u) {
                word = (uint)(bit_len & 0xffffffffu);
            }
            w[i] = word;
        }
        sha256_compress(state, w);
    }

    device uchar* out = tile_hashes + gid * 32u;
    for (uint i = 0; i < 8; ++i) {
        const uint word = state[i];
        out[i * 4u + 0u] = (uchar)((word >> 24u) & 0xffu);
        out[i * 4u + 1u] = (uchar)((word >> 16u) & 0xffu);
        out[i * 4u + 2u] = (uchar)((word >> 8u) & 0xffu);
        out[i * 4u + 3u] = (uchar)(word & 0xffu);
    }
}
