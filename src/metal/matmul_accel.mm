// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <metal/matmul_accel.h>
#include <metal/matmul_accel_env.h>

#include <crypto/common.h>
#include <hash.h>
#include <matmul/transcript.h>
#include <span.h>

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <limits.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <unistd.h>

#ifndef kIOMainPortDefault
#define kIOMainPortDefault MACH_PORT_NULL
#endif

namespace {

// Verbatim copy of src/metal/matmul_accel_kernels.metal (spliced by the
// maintainer; keep the two in sync -- the .metal file is the source of truth
// and the precompiled metallib, when present, is built from it).
constexpr const char* KERNEL_SOURCE = R"METAL(
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
)METAL";

void AppendUniquePath(std::vector<std::string>& paths, const char* path)
{
    if (path == nullptr || path[0] == '\0') return;
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.emplace_back(path);
    }
}

std::optional<std::string> ExecutableDirectory()
{
    uint32_t size = PATH_MAX;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return std::nullopt;
        }
    }

    char resolved[PATH_MAX];
    const char* executable_path = realpath(buffer.data(), resolved) != nullptr ? resolved : buffer.data();
    std::string path{executable_path};
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    return path.substr(0, separator);
}

std::vector<std::string> MatMulMetallibCandidatePaths()
{
    std::vector<std::string> paths;
    AppendUniquePath(paths, std::getenv("QTC_MATMUL_METALLIB_PATH"));

    if (const auto executable_dir = ExecutableDirectory()) {
        const std::string packaged_path = *executable_dir + "/metal/matmul_accel_kernels.metallib";
        AppendUniquePath(paths, packaged_path.c_str());
    }

#if defined(QTC_MATMUL_METALLIB_PATH)
    AppendUniquePath(paths, QTC_MATMUL_METALLIB_PATH);
#endif
    return paths;
}

constexpr uint32_t FC_INDEX_N = 0;
constexpr uint32_t FC_INDEX_B = 1;
constexpr uint32_t FC_INDEX_R = 2;
constexpr uint32_t FC_INDEX_NBLOCKS = 3;

constexpr uint32_t MAX_BLOCK_ELEMENTS = 256;
constexpr uint32_t PRODUCT_TILE_DIM = 16;
constexpr size_t TILE_HASH_BYTES = 32;

struct SpecializedKernelShape {
    uint32_t n;
    uint32_t b;
    uint32_t r;
    uint32_t N;
    const char* label;
};

constexpr std::array<SpecializedKernelShape, 3> SPECIALIZED_KERNEL_SHAPES{{
    {512, 16, 8, 32, "mainnet_512_16_8"},
    {256, 8, 4, 32, "testnet_256_8_4"},
    {64, 8, 4, 8, "regtest_64_8_4"},
}};

// Keep the default auto pool conservative enough for Apple Silicon mining while
// still matching the solver fanout policy on hosts that benefit from multiple
// in-flight Metal solves. Explicit env overrides still take precedence.
constexpr uint32_t DEFAULT_METAL_POOL_SLOT_COUNT{1};
constexpr uint32_t MAX_METAL_POOL_SLOT_COUNT{8};

struct SpecializedKernelPipelines {
    SpecializedKernelShape shape{};
    id<MTLComputePipelineState> build_perturbed_pipeline{nil};
    id<MTLComputePipelineState> build_product_pipeline{nil};
    bool available{false};
};

int32_t ResolveApplePerformanceLogicalCpuCount()
{
    const char* override_env = std::getenv("QTC_MATMUL_APPLE_PERFLEVEL0_LOGICALCPU_OVERRIDE");
    if (override_env != nullptr && override_env[0] != '\0') {
        char* override_end{nullptr};
        const long parsed_override = std::strtol(override_env, &override_end, 10);
        if (override_end != override_env && *override_end == '\0') {
            return static_cast<int32_t>(parsed_override);
        }
    }

    int32_t perf_level0_logicalcpu{0};
    size_t perf_level0_size{sizeof(perf_level0_logicalcpu)};
    if (sysctlbyname("hw.perflevel0.logicalcpu",
                     &perf_level0_logicalcpu,
                     &perf_level0_size,
                     nullptr,
                     0) == 0 &&
        perf_level0_size == sizeof(perf_level0_logicalcpu) &&
        perf_level0_logicalcpu > 0) {
        return perf_level0_logicalcpu;
    }
    return 0;
}

bool IsHighPerfAppleMetalHost(int32_t perf_level0_logicalcpu)
{
    return perf_level0_logicalcpu >= 10;
}

bool IsConservativeAppleMetalHost(int32_t perf_level0_logicalcpu)
{
    return perf_level0_logicalcpu > 0 && perf_level0_logicalcpu <= 4;
}

struct MetalGpuCoreCountProbe {
    uint32_t core_count{0};
    std::string source;
};

uint32_t UInt32FromCFNumber(CFTypeRef value)
{
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return 0;
    }

    int64_t signed_value{0};
    if (!CFNumberGetValue(
            static_cast<CFNumberRef>(value),
            kCFNumberSInt64Type,
            &signed_value) ||
        signed_value <= 0 ||
        signed_value > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    return static_cast<uint32_t>(signed_value);
}

MetalGpuCoreCountProbe ReadGpuCoreCountFromRegistryEntry(io_registry_entry_t entry)
{
    CFTypeRef direct_core_count = IORegistryEntryCreateCFProperty(
        entry,
        CFSTR("gpu-core-count"),
        kCFAllocatorDefault,
        0);
    if (direct_core_count != nullptr) {
        const uint32_t core_count = UInt32FromCFNumber(direct_core_count);
        CFRelease(direct_core_count);
        if (core_count > 0) {
            return MetalGpuCoreCountProbe{
                .core_count = core_count,
                .source = "io_registry_gpu_core_count",
            };
        }
    }

    CFTypeRef gpu_config = IORegistryEntryCreateCFProperty(
        entry,
        CFSTR("GPUConfigurationVariable"),
        kCFAllocatorDefault,
        0);
    if (gpu_config != nullptr) {
        MetalGpuCoreCountProbe result;
        if (CFGetTypeID(gpu_config) == CFDictionaryGetTypeID()) {
            const void* num_cores_value = CFDictionaryGetValue(
                static_cast<CFDictionaryRef>(gpu_config),
                CFSTR("num_cores"));
            const uint32_t core_count = UInt32FromCFNumber(
                static_cast<CFTypeRef>(num_cores_value));
            if (core_count > 0) {
                result = MetalGpuCoreCountProbe{
                    .core_count = core_count,
                    .source = "io_registry_gpu_configuration_num_cores",
                };
            }
        }
        CFRelease(gpu_config);
        if (result.core_count > 0) {
            return result;
        }
    }

    return {};
}

MetalGpuCoreCountProbe ResolveGpuCoreCountFromIterator(io_iterator_t iterator)
{
    io_registry_entry_t entry = IO_OBJECT_NULL;
    while ((entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        const MetalGpuCoreCountProbe result = ReadGpuCoreCountFromRegistryEntry(entry);
        IOObjectRelease(entry);
        if (result.core_count > 0) {
            return result;
        }
    }
    return {};
}

MetalGpuCoreCountProbe ResolveAppleMetalGpuCoreCountFromIORegistry()
{
    static const MetalGpuCoreCountProbe cached = [] {
        io_iterator_t iterator = IO_OBJECT_NULL;
        if (IOServiceGetMatchingServices(
                kIOMainPortDefault,
                IOServiceMatching("AGXAccelerator"),
                &iterator) == KERN_SUCCESS &&
            iterator != IO_OBJECT_NULL) {
            const MetalGpuCoreCountProbe result = ResolveGpuCoreCountFromIterator(iterator);
            IOObjectRelease(iterator);
            if (result.core_count > 0) {
                return result;
            }
        }

        iterator = IO_OBJECT_NULL;
        if (IORegistryCreateIterator(
                kIOMainPortDefault,
                kIOServicePlane,
                kIORegistryIterateRecursively,
                &iterator) == KERN_SUCCESS &&
            iterator != IO_OBJECT_NULL) {
            MetalGpuCoreCountProbe result = ResolveGpuCoreCountFromIterator(iterator);
            IOObjectRelease(iterator);
            if (result.core_count > 0) {
                result.source += "_recursive";
                return result;
            }
        }

        return MetalGpuCoreCountProbe{
            .core_count = 0,
            .source = "unavailable",
        };
    }();
    return cached;
}

uint32_t ResolveMetalAutoPoolSlotCount()
{
    const char* solver_threads_env = std::getenv("QTC_MATMUL_SOLVER_THREADS");
    if (solver_threads_env != nullptr && solver_threads_env[0] != '\0') {
        char* solver_threads_end{nullptr};
        const long parsed_solver_threads = std::strtol(solver_threads_env, &solver_threads_end, 10);
        if (solver_threads_end != solver_threads_env &&
            *solver_threads_end == '\0' &&
            parsed_solver_threads > 0) {
            return static_cast<uint32_t>(std::clamp<long>(
                parsed_solver_threads,
                1,
                MAX_METAL_POOL_SLOT_COUNT));
        }
    }

    const int32_t perf_level0_logicalcpu = ResolveApplePerformanceLogicalCpuCount();
    if (perf_level0_logicalcpu > 0) {
        if (IsHighPerfAppleMetalHost(perf_level0_logicalcpu)) {
            return 5;
        }

        if (IsConservativeAppleMetalHost(perf_level0_logicalcpu)) {
            return 1;
        }

        return static_cast<uint32_t>(std::clamp<int32_t>(
            perf_level0_logicalcpu - 1,
            1,
            4));
    }

    return DEFAULT_METAL_POOL_SLOT_COUNT;
}

uint32_t ResolveMetalPoolSlotCount()
{
    const auto parsed = qtc::metal::detail::ParsePoolSlotsEnv(
        std::getenv("QTC_MATMUL_METAL_POOL_SLOTS"),
        MAX_METAL_POOL_SLOT_COUNT,
        DEFAULT_METAL_POOL_SLOT_COUNT);
    if (parsed.has_value()) {
        return *parsed;
    }
    return ResolveMetalAutoPoolSlotCount();
}

bool ParseTruthyEnv(const char* name, bool default_value)
{
    return qtc::metal::detail::ParseTruthyEnv(std::getenv(name), default_value);
}

bool ShouldPrewarmMetalPoolSlots()
{
    return ParseTruthyEnv("QTC_MATMUL_METAL_POOL_PREWARM", true);
}

// Buffer footprint of one pool slot for a given (n, b, r) shape. Batch
// submissions request batch_size multiples of the per-attempt sizes.
struct PoolRequirements {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    size_t matrix_bytes{0};
    size_t noise_bytes{0};
    size_t c_prime_bytes{0};
    size_t tile_hash_bytes{0};
};

struct MetalPoolSlot {
    id<MTLCommandQueue> queue{nil};
    id<MTLBuffer> params_buffer{nil};
    id<MTLBuffer> matrix_a_stage_buffer{nil};
    id<MTLBuffer> matrix_b_stage_buffer{nil};
    id<MTLBuffer> e_l_buffer{nil};
    id<MTLBuffer> e_r_buffer{nil};
    id<MTLBuffer> f_l_buffer{nil};
    id<MTLBuffer> f_r_buffer{nil};
    id<MTLBuffer> a_prime_buffer{nil};
    id<MTLBuffer> b_prime_buffer{nil};
    id<MTLBuffer> c_prime_buffer{nil};
    id<MTLBuffer> tile_hash_buffer{nil};
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    size_t matrix_bytes{0};
    size_t noise_bytes{0};
    size_t c_prime_bytes{0};
    size_t tile_hash_bytes{0};
    bool in_use{false};
};

struct MetalContext {
    bool ready{false};
    std::string error;
    id<MTLDevice> device{nil};
    id<MTLComputePipelineState> generate_base_matrix_pipeline{nil};
    id<MTLComputePipelineState> build_perturbed_pipeline{nil};
    id<MTLComputePipelineState> build_product_pipeline{nil};
    id<MTLComputePipelineState> build_product_tiled_pipeline{nil};
    id<MTLComputePipelineState> hash_product_tiles_pipeline{nil};
    std::array<SpecializedKernelPipelines, SPECIALIZED_KERNEL_SHAPES.size()> specialized_pipelines{};
    uint32_t specialized_pipeline_count{0};
    std::string specialized_pipeline_reason;
    bool using_precompiled_library{false};
    bool capture_supported{false};
    std::mutex pool_mutex;
    std::condition_variable pool_cv;
    std::vector<MetalPoolSlot> pool_slots;
    uint32_t pool_last_n{0};
    uint32_t pool_last_b{0};
    uint32_t pool_last_r{0};
    uint32_t pool_next_slot{0};
    uint32_t pool_active_slots{0};
    uint32_t pool_high_water_slots{0};
    uint64_t pool_allocation_events{0};
    uint64_t pool_reuse_events{0};
    uint64_t pool_wait_events{0};
    uint64_t pool_completed_submissions{0};
    uint32_t pool_inflight_submissions{0};
    uint32_t pool_peak_inflight_submissions{0};
    std::mutex profiling_mutex;
    qtc::metal::MatMulProfilingStats profiling_stats;
    std::mutex resident_base_mutex;
    id<MTLBuffer> resident_matrix_a_buffer{nil};
    id<MTLBuffer> resident_matrix_b_buffer{nil};
    uint32_t resident_matrix_n{0};
    uint64_t resident_data_fingerprint{0};

    MetalContext()
    {
        @autoreleasepool {
            // MTLCreateSystemDefaultDevice() is restricted to interactive apps on macOS 14+; it
            // returns nil from CLI/daemon contexts (qtcd, qtc-genesis, qtc-matmul-*). MTLCopyAllDevices()
            // works in any context -- Apple's documented replacement for non-interactive apps.
            NSArray<id<MTLDevice>>* allDevices = MTLCopyAllDevices();
            if (allDevices == nil || allDevices.count == 0) {
                error = "No Metal-compatible GPU device found";
                return;
            }
            device = allDevices[0];
            capture_supported = [MTLCaptureManager sharedCaptureManager] != nil;
            pool_slots.resize(ResolveMetalPoolSlotCount());
            for (auto& slot : pool_slots) {
                slot.queue = [device newCommandQueue];
                if (slot.queue == nil) {
                    error = "Failed to create Metal command queue";
                    return;
                }
            }

            NSError* library_error = nil;
            id<MTLLibrary> library = nil;
            for (const auto& candidate_path : MatMulMetallibCandidatePaths()) {
                NSString* precompiled_path = [NSString stringWithUTF8String:candidate_path.c_str()];
                if (![[NSFileManager defaultManager] fileExistsAtPath:precompiled_path]) {
                    continue;
                }
                library_error = nil;
                NSURL* precompiled_url = [NSURL fileURLWithPath:precompiled_path];
                library = [device newLibraryWithURL:precompiled_url error:&library_error];
                using_precompiled_library = (library != nil);
                if (library != nil) {
                    break;
                }
            }
            if (library == nil) {
                library_error = nil;
                library = [device newLibraryWithSource:[NSString stringWithUTF8String:KERNEL_SOURCE]
                                               options:nil
                                                 error:&library_error];
                using_precompiled_library = false;
            }
            if (library == nil) {
                error = library_error != nil ? [[library_error localizedDescription] UTF8String]
                                             : "Failed to compile Metal MatMul kernel source";
                return;
            }

            auto make_pipeline = [&](NSString* function_name) -> id<MTLComputePipelineState> {
                id<MTLFunction> function = [library newFunctionWithName:function_name];
                if (function == nil) {
                    error = std::string("Failed to load Metal kernel function: ") + [function_name UTF8String];
                    return nil;
                }
                NSError* pipeline_error = nil;
                id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&pipeline_error];
                if (pipeline == nil) {
                    error = pipeline_error != nil
                        ? [[pipeline_error localizedDescription] UTF8String]
                        : std::string("Failed to create Metal pipeline: ") + [function_name UTF8String];
                }
                return pipeline;
            };

            generate_base_matrix_pipeline = make_pipeline(@"generate_base_matrix_from_seed");
            if (generate_base_matrix_pipeline == nil) return;
            build_perturbed_pipeline = make_pipeline(@"build_perturbed");
            if (build_perturbed_pipeline == nil) return;
            build_product_pipeline = make_pipeline(@"build_product");
            if (build_product_pipeline == nil) return;
            build_product_tiled_pipeline = make_pipeline(@"build_product_tiled");
            if (build_product_tiled_pipeline == nil) return;
            hash_product_tiles_pipeline = make_pipeline(@"hash_product_tiles");
            if (hash_product_tiles_pipeline == nil) return;

            auto make_specialized_pipeline = [&](NSString* function_name,
                                                 const SpecializedKernelShape& shape,
                                                 std::string& out_error) -> id<MTLComputePipelineState> {
                MTLFunctionConstantValues* values = [[MTLFunctionConstantValues alloc] init];
                const uint32_t n = shape.n;
                const uint32_t b = shape.b;
                const uint32_t r = shape.r;
                const uint32_t N = shape.N;
                [values setConstantValue:&n type:MTLDataTypeUInt atIndex:FC_INDEX_N];
                [values setConstantValue:&b type:MTLDataTypeUInt atIndex:FC_INDEX_B];
                [values setConstantValue:&r type:MTLDataTypeUInt atIndex:FC_INDEX_R];
                [values setConstantValue:&N type:MTLDataTypeUInt atIndex:FC_INDEX_NBLOCKS];

                NSError* fn_error = nil;
                id<MTLFunction> function = [library newFunctionWithName:function_name constantValues:values error:&fn_error];
                if (function == nil) {
                    out_error = fn_error != nil ? [[fn_error localizedDescription] UTF8String]
                                                : "Failed to create specialized Metal function";
                    return nil;
                }

                NSError* local_pipeline_error = nil;
                id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&local_pipeline_error];
                if (pipeline == nil) {
                    out_error = local_pipeline_error != nil ? [[local_pipeline_error localizedDescription] UTF8String]
                                                            : "Failed to create specialized Metal pipeline";
                }
                return pipeline;
            };

            specialized_pipeline_count = 0;
            specialized_pipeline_reason = "function_constant_specialization_unavailable";
            for (size_t i = 0; i < SPECIALIZED_KERNEL_SHAPES.size(); ++i) {
                auto& entry = specialized_pipelines[i];
                entry = SpecializedKernelPipelines{};
                entry.shape = SPECIALIZED_KERNEL_SHAPES[i];

                std::string local_error;
                entry.build_perturbed_pipeline = make_specialized_pipeline(@"build_perturbed_specialized", entry.shape, local_error);
                if (entry.build_perturbed_pipeline == nil) {
                    specialized_pipeline_reason = local_error.empty() ? "build_perturbed_specialized_unavailable" : local_error;
                    continue;
                }

                entry.build_product_pipeline = make_specialized_pipeline(@"build_product_specialized", entry.shape, local_error);
                if (entry.build_product_pipeline == nil) {
                    specialized_pipeline_reason = local_error.empty() ? "build_product_specialized_unavailable" : local_error;
                    continue;
                }

                entry.available = true;
                ++specialized_pipeline_count;
            }
            if (specialized_pipeline_count > 0) {
                specialized_pipeline_reason = "function_constant_specialization_ready";
            }

            ready = true;
        }
    }
};

MetalContext& GetContext()
{
    static MetalContext context;
    return context;
}

const SpecializedKernelPipelines* FindSpecializedPipelines(const MetalContext& context,
                                                           uint32_t n,
                                                           uint32_t b,
                                                           uint32_t r,
                                                           uint32_t N)
{
    for (const auto& entry : context.specialized_pipelines) {
        if (!entry.available) {
            continue;
        }
        if (entry.shape.n == n && entry.shape.b == b && entry.shape.r == r && entry.shape.N == N) {
            return &entry;
        }
    }
    return nullptr;
}

bool IsPoolSlotInitialized(const MetalPoolSlot& slot)
{
    return slot.params_buffer != nil &&
        slot.matrix_a_stage_buffer != nil &&
        slot.matrix_b_stage_buffer != nil &&
        slot.e_l_buffer != nil &&
        slot.e_r_buffer != nil &&
        slot.f_l_buffer != nil &&
        slot.f_r_buffer != nil &&
        slot.a_prime_buffer != nil &&
        slot.b_prime_buffer != nil &&
        slot.c_prime_buffer != nil &&
        slot.tile_hash_buffer != nil;
}

bool DoesPoolSlotSatisfyRequest(const MetalPoolSlot& slot, const PoolRequirements& req)
{
    return IsPoolSlotInitialized(slot) &&
        slot.n == req.n &&
        slot.b == req.b &&
        slot.r == req.r &&
        slot.matrix_bytes >= req.matrix_bytes &&
        slot.noise_bytes >= req.noise_bytes &&
        slot.c_prime_bytes >= req.c_prime_bytes &&
        slot.tile_hash_bytes >= req.tile_hash_bytes;
}

bool AllocateBufferPoolSlot(MetalContext& context,
                            MetalPoolSlot& slot,
                            const PoolRequirements& req,
                            std::string& error)
{
    id<MTLBuffer> params_buffer = [context.device newBufferWithLength:sizeof(uint32_t) * 4
                                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> matrix_a_stage_buffer = [context.device newBufferWithLength:req.matrix_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> matrix_b_stage_buffer = [context.device newBufferWithLength:req.matrix_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> e_l_buffer = [context.device newBufferWithLength:req.noise_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> e_r_buffer = [context.device newBufferWithLength:req.noise_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> f_l_buffer = [context.device newBufferWithLength:req.noise_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> f_r_buffer = [context.device newBufferWithLength:req.noise_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> a_prime_buffer = [context.device newBufferWithLength:req.matrix_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_prime_buffer = [context.device newBufferWithLength:req.matrix_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> c_prime_buffer = [context.device newBufferWithLength:req.c_prime_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> tile_hash_buffer = [context.device newBufferWithLength:req.tile_hash_bytes options:MTLResourceStorageModeShared];
    if (params_buffer == nil || matrix_a_stage_buffer == nil || matrix_b_stage_buffer == nil ||
        e_l_buffer == nil || e_r_buffer == nil || f_l_buffer == nil || f_r_buffer == nil ||
        a_prime_buffer == nil || b_prime_buffer == nil || c_prime_buffer == nil || tile_hash_buffer == nil) {
        error = "Failed to allocate Metal buffer pool slot for MatMul digest";
        return false;
    }

    slot.params_buffer = params_buffer;
    slot.matrix_a_stage_buffer = matrix_a_stage_buffer;
    slot.matrix_b_stage_buffer = matrix_b_stage_buffer;
    slot.e_l_buffer = e_l_buffer;
    slot.e_r_buffer = e_r_buffer;
    slot.f_l_buffer = f_l_buffer;
    slot.f_r_buffer = f_r_buffer;
    slot.a_prime_buffer = a_prime_buffer;
    slot.b_prime_buffer = b_prime_buffer;
    slot.c_prime_buffer = c_prime_buffer;
    slot.tile_hash_buffer = tile_hash_buffer;
    slot.matrix_bytes = req.matrix_bytes;
    slot.noise_bytes = req.noise_bytes;
    slot.c_prime_bytes = req.c_prime_bytes;
    slot.tile_hash_bytes = req.tile_hash_bytes;
    return true;
}

bool EnsureBufferPoolSlot(MetalContext& context,
                          MetalPoolSlot& slot,
                          const PoolRequirements& req,
                          std::string& error)
{
    if (DoesPoolSlotSatisfyRequest(slot, req)) {
        ++context.pool_reuse_events;
        context.pool_last_n = req.n;
        context.pool_last_b = req.b;
        context.pool_last_r = req.r;
        return true;
    }

    if (!AllocateBufferPoolSlot(context, slot, req, error)) {
        return false;
    }
    slot.n = req.n;
    slot.b = req.b;
    slot.r = req.r;
    context.pool_last_n = req.n;
    context.pool_last_b = req.b;
    context.pool_last_r = req.r;
    ++context.pool_allocation_events;
    return true;
}

void PrewarmAvailableBufferPoolSlots(MetalContext& context,
                                     const PoolRequirements& req,
                                     size_t selected_slot_index)
{
    if (!ShouldPrewarmMetalPoolSlots() || context.pool_slots.size() <= 1) {
        return;
    }

    std::string ignored_error;
    for (size_t slot_index = 0; slot_index < context.pool_slots.size(); ++slot_index) {
        if (slot_index == selected_slot_index) {
            continue;
        }

        auto& slot = context.pool_slots[slot_index];
        if (slot.in_use || DoesPoolSlotSatisfyRequest(slot, req)) {
            continue;
        }

        if (!EnsureBufferPoolSlot(context, slot, req, ignored_error)) {
            return;
        }
    }
}

struct BufferPoolLease {
    MetalContext* context{nullptr};
    MetalPoolSlot* slot{nullptr};
    size_t slot_index{0};

    BufferPoolLease(MetalContext* context_in, MetalPoolSlot* slot_in, size_t slot_index_in)
        : context(context_in), slot(slot_in), slot_index(slot_index_in)
    {
    }

    BufferPoolLease(const BufferPoolLease&) = delete;
    BufferPoolLease& operator=(const BufferPoolLease&) = delete;

    BufferPoolLease(BufferPoolLease&& other) noexcept
        : context(other.context), slot(other.slot), slot_index(other.slot_index)
    {
        other.context = nullptr;
        other.slot = nullptr;
    }

    ~BufferPoolLease()
    {
        Release();
    }

    void Release()
    {
        if (context == nullptr || slot == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(context->pool_mutex);
            if (slot->in_use) {
                slot->in_use = false;
                if (context->pool_active_slots > 0) {
                    --context->pool_active_slots;
                }
            }
        }
        context->pool_cv.notify_one();
        context = nullptr;
        slot = nullptr;
    }
};

template <typename Result>
struct AsyncDigestSubmissionState {
    MetalContext* context{nullptr};
    std::mutex mutex;
    std::condition_variable cv;
    bool completed{false};
    Result result{};
    std::optional<BufferPoolLease> lease;
    CFMutableArrayRef retained_inputs{nullptr};
    double encode_build_perturbed_us{0.0};
    double encode_product_us{0.0};
    double encode_tile_hash_us{0.0};
    double submit_wait_us{0.0};
    double gpu_execution_ms{0.0};
    double cpu_finalize_us{0.0};
    bool any_zero_copy_input{false};

    ~AsyncDigestSubmissionState()
    {
        if (retained_inputs != nullptr) {
            CFRelease(retained_inputs);
            retained_inputs = nullptr;
        }
    }
};

struct AsyncSingleDigestState final : public AsyncDigestSubmissionState<qtc::metal::MatMulDigestResult> {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t N{0};
    uint256 sigma;
    id<MTLBuffer> tile_hash_buffer{nil};
};

struct AsyncBatchDigestState final : public AsyncDigestSubmissionState<qtc::metal::MatMulDigestBatchResult> {
    uint32_t batch_size{0};
    uint32_t n{0};
    uint32_t b{0};
    uint32_t N{0};
    size_t tile_hash_bytes_per_item{0};
    std::vector<uint256> sigmas;
    id<MTLBuffer> tile_hash_buffer{nil};
};

CFMutableArrayRef CreateRetainedInputArray(CFIndex capacity)
{
    return CFArrayCreateMutable(kCFAllocatorDefault, capacity, &kCFTypeArrayCallBacks);
}

void RetainTemporaryInputBuffer(CFMutableArrayRef array, id<MTLBuffer> buffer)
{
    if (array == nullptr || buffer == nil) {
        return;
    }
    CFArrayAppendValue(array, (__bridge const void*)buffer);
}

// Host side of the v4 digest: root = SHA-256(tile hashes in row-major tile
// order), digest = SHA256d(tag || sigma || root || LE32(n) || LE32(b)).
bool FinalizeProductDigestFromTileHashBytes(const unsigned char* tile_hash_bytes,
                                            uint32_t blocks_per_axis,
                                            uint32_t n,
                                            uint32_t b,
                                            const uint256& sigma,
                                            uint256& out_digest,
                                            std::string& error)
{
    if (tile_hash_bytes == nullptr) {
        error = "product digest finalize missing Metal tile hash buffer contents";
        return false;
    }
    if (blocks_per_axis == 0) {
        error = "product digest finalize requires non-zero blocks_per_axis";
        return false;
    }

    try {
        const size_t tile_count = static_cast<size_t>(blocks_per_axis) * blocks_per_axis;
        std::vector<uint256> tile_hashes;
        tile_hashes.reserve(tile_count);
        for (size_t tile = 0; tile < tile_count; ++tile) {
            tile_hashes.emplace_back(Span<const unsigned char>{
                tile_hash_bytes + tile * TILE_HASH_BYTES,
                TILE_HASH_BYTES,
            });
        }
        out_digest = matmul::transcript::ComputeProductCommittedDigestFromTileHashes(
            Span<const uint256>{tile_hashes.data(), tile_hashes.size()},
            sigma,
            n,
            b);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

template <typename State>
void FinalizeAsyncSubmissionState(const std::shared_ptr<State>& state,
                                  const char* profiling_reason)
{
    if (state == nullptr || state->context == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->context->pool_mutex);
        if (state->context->pool_inflight_submissions > 0) {
            --state->context->pool_inflight_submissions;
        }
        ++state->context->pool_completed_submissions;
    }

    if (state->lease.has_value()) {
        state->lease->Release();
        state->lease.reset();
    }

    {
        std::lock_guard<std::mutex> lock(state->context->profiling_mutex);
        state->context->profiling_stats.available = true;
        state->context->profiling_stats.capture_supported = state->context->capture_supported;
        ++state->context->profiling_stats.samples;
        state->context->profiling_stats.last_encode_build_perturbed_us = state->encode_build_perturbed_us;
        state->context->profiling_stats.last_encode_fused_prefix_compress_us = state->encode_product_us;
        state->context->profiling_stats.last_encode_transcript_sha256_us = state->encode_tile_hash_us;
        state->context->profiling_stats.last_submit_wait_us = state->submit_wait_us;
        state->context->profiling_stats.last_gpu_execution_ms = state->gpu_execution_ms;
        state->context->profiling_stats.last_cpu_finalize_us = state->cpu_finalize_us;
        state->context->profiling_stats.last_zero_copy_inputs = state->any_zero_copy_input;
        state->context->profiling_stats.last_async_submission = true;
        state->context->profiling_stats.reason = profiling_reason;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->completed = true;
    }
    state->cv.notify_all();
}

std::optional<BufferPoolLease> AcquireBufferPoolLease(MetalContext& context,
                                                      const PoolRequirements& req,
                                                      std::string& error)
{
    std::unique_lock<std::mutex> lock(context.pool_mutex);
    if (context.pool_slots.empty()) {
        error = "No Metal buffer pool slots are configured";
        return std::nullopt;
    }

    bool waited{false};
    // Cap the number of wait iterations to prevent indefinite blocking
    // when all pool slots are occupied (e.g. by parallel solver threads).
    // After the deadline the caller falls back to CPU, which also lets
    // the mining loop re-check its abort flag on tip-change or shutdown.
    constexpr uint32_t kMaxWaitRounds = 40; // 40 * 50ms = 2 seconds
    uint32_t wait_rounds{0};
    while (true) {
        for (size_t offset = 0; offset < context.pool_slots.size(); ++offset) {
            const size_t slot_index = (context.pool_next_slot + offset) % context.pool_slots.size();
            auto& slot = context.pool_slots[slot_index];
            if (slot.in_use) {
                continue;
            }
            if (!EnsureBufferPoolSlot(context, slot, req, error)) {
                return std::nullopt;
            }

            PrewarmAvailableBufferPoolSlots(context, req, slot_index);

            slot.in_use = true;
            ++context.pool_active_slots;
            context.pool_high_water_slots = std::max(context.pool_high_water_slots, context.pool_active_slots);
            context.pool_next_slot = static_cast<uint32_t>((slot_index + 1) % context.pool_slots.size());
            if (waited) {
                ++context.pool_wait_events;
            }
            return BufferPoolLease{&context, &slot, slot_index};
        }

        waited = true;
        if (++wait_rounds > kMaxWaitRounds) {
            error = "Metal buffer pool exhausted after timeout";
            return std::nullopt;
        }
        context.pool_cv.wait_for(lock, std::chrono::milliseconds{50});
    }
}

struct ShapeParams {
    uint32_t N{0};
    uint64_t matrix_words{0};
    uint64_t noise_words{0};
    uint64_t tile_count{0};
};

bool BuildKernelParamsForShape(uint32_t n_in,
                               uint32_t b_in,
                               uint32_t r_in,
                               ShapeParams& out,
                               std::string& error)
{
    if (n_in == 0 || b_in == 0 || r_in == 0) {
        error = "invalid MatMul request dimensions";
        return false;
    }
    if (r_in > n_in) {
        error = "noise rank exceeds matrix dimension";
        return false;
    }
    if ((n_in % b_in) != 0) {
        error = "matrix dimension must be divisible by transcript block size";
        return false;
    }
    if ((static_cast<uint64_t>(b_in) * b_in) > MAX_BLOCK_ELEMENTS) {
        error = "transcript block size exceeds product tile threadgroup capacity";
        return false;
    }

    const uint64_t n = n_in;
    const uint64_t N = n / b_in;
    const uint64_t matrix_words = n * n;
    const uint64_t noise_words = n * r_in;
    const uint64_t tile_count = N * N;

    if (matrix_words > std::numeric_limits<uint32_t>::max() ||
        tile_count > std::numeric_limits<uint32_t>::max()) {
        error = "matrix dimensions exceed supported Metal launch bounds";
        return false;
    }

    out.N = static_cast<uint32_t>(N);
    out.matrix_words = matrix_words;
    out.noise_words = noise_words;
    out.tile_count = tile_count;
    return true;
}

bool ValidateDigestMode(qtc::metal::MatMulDigestMode mode, std::string& error)
{
    if (mode != qtc::metal::MatMulDigestMode::PRODUCT_COMMITTED) {
        error = "metal_transcript_digest_mode_unsupported_since_product_digest_v4_use_cpu";
        return false;
    }
    return true;
}

bool BuildKernelParams(const qtc::metal::MatMulDigestRequest& request,
                       ShapeParams& out,
                       std::string& error)
{
    if (!ValidateDigestMode(request.digest_mode, error)) {
        return false;
    }
    if (!BuildKernelParamsForShape(request.n, request.b, request.r, out, error)) {
        return false;
    }

    if (request.use_uploaded_base_matrices) {
        if (request.matrix_a != nullptr || request.matrix_b != nullptr) {
            error = "explicit base matrix buffers are not allowed when using uploaded matrices";
            return false;
        }
    } else if (request.matrix_a == nullptr || request.matrix_b == nullptr) {
        error = "missing MatMul base matrix buffer";
        return false;
    }

    if (request.noise_e_l == nullptr || request.noise_e_r == nullptr ||
        request.noise_f_l == nullptr || request.noise_f_r == nullptr) {
        error = "missing MatMul input buffer";
        return false;
    }

    return true;
}

PoolRequirements MakePoolRequirements(uint32_t n,
                                      uint32_t b,
                                      uint32_t r,
                                      const ShapeParams& shape,
                                      uint32_t batch_multiplier)
{
    const size_t matrix_bytes = static_cast<size_t>(shape.matrix_words) * sizeof(uint32_t);
    const size_t noise_bytes = static_cast<size_t>(shape.noise_words) * sizeof(uint32_t);
    const size_t tile_hash_bytes = static_cast<size_t>(shape.tile_count) * TILE_HASH_BYTES;
    return PoolRequirements{
        .n = n,
        .b = b,
        .r = r,
        .matrix_bytes = matrix_bytes * batch_multiplier,
        .noise_bytes = noise_bytes * batch_multiplier,
        .c_prime_bytes = matrix_bytes * batch_multiplier,
        .tile_hash_bytes = tile_hash_bytes * batch_multiplier,
    };
}

NSUInteger SelectThreadGroupSize(id<MTLComputePipelineState> pipeline, NSUInteger preferred)
{
    const NSUInteger max_threads = std::max<NSUInteger>(pipeline.maxTotalThreadsPerThreadgroup, 1);
    const NSUInteger requested = std::max<NSUInteger>(preferred, 1);
    return std::min<NSUInteger>(requested, max_threads);
}

id<MTLBuffer> WrapSharedNoCopyBuffer(id<MTLDevice> device, const void* bytes, size_t length)
{
    if (device == nil || bytes == nullptr || length == 0) {
        return nil;
    }
    // Metal's newBufferWithBytesNoCopy requires a page-aligned pointer and
    // internally rounds the length up to the page boundary.  When malloc'd
    // memory (e.g. std::vector) coincidentally sits on a page boundary but
    // the allocation is smaller than a page, Metal's page-rounded view can
    // extend past the actual allocation, causing non-deterministic GPU reads.
    // Guard: require both pointer alignment AND length >= page_size so that
    // only properly VM-backed, full-page-or-larger allocations use zero-copy.
    const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
    if (reinterpret_cast<uintptr_t>(bytes) % page_size != 0 || length < page_size) {
        return nil;
    }
    return [device newBufferWithBytesNoCopy:const_cast<void*>(bytes)
                                      length:length
                                     options:MTLResourceStorageModeShared
                                 deallocator:nil];
}

struct BufferBinding {
    id<MTLBuffer> buffer{nil};
    NSUInteger offset{0};
};

template <size_t N>
void SetEncoderBindings(id<MTLComputeCommandEncoder> encoder, const std::array<BufferBinding, N>& bindings)
{
    std::array<id<MTLBuffer>, N> buffers{};
    std::array<NSUInteger, N> offsets{};
    for (NSUInteger i = 0; i < bindings.size(); ++i) {
        buffers[i] = bindings[i].buffer;
        offsets[i] = bindings[i].offset;
    }
    [encoder setBuffers:buffers.data() offsets:offsets.data() withRange:NSMakeRange(0, bindings.size())];
}

template <size_t N>
bool EncodeComputeBindings(id<MTLCommandBuffer> command,
                           id<MTLComputePipelineState> pipeline,
                           NSUInteger grid_size,
                           NSUInteger preferred_thread_group_size,
                           const std::array<BufferBinding, N>& bindings,
                           std::string& error)
{
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (encoder == nil) {
        error = "Failed to create Metal compute encoder";
        return false;
    }

    [encoder setComputePipelineState:pipeline];
    SetEncoderBindings(encoder, bindings);

    const NSUInteger thread_group_size = SelectThreadGroupSize(pipeline, preferred_thread_group_size);
    const MTLSize grid = MTLSizeMake(grid_size, 1, 1);
    const MTLSize group = MTLSizeMake(thread_group_size, 1, 1);

    [encoder dispatchThreads:grid threadsPerThreadgroup:group];
    [encoder endEncoding];
    return true;
}

template <size_t N>
bool EncodeComputeThreadgroupsBindings(id<MTLCommandBuffer> command,
                                       id<MTLComputePipelineState> pipeline,
                                       NSUInteger threadgroups_width,
                                       NSUInteger threadgroups_height,
                                       NSUInteger threads_per_group,
                                       const std::array<BufferBinding, N>& bindings,
                                       std::string& error)
{
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (encoder == nil) {
        error = "Failed to create Metal compute encoder";
        return false;
    }

    [encoder setComputePipelineState:pipeline];
    SetEncoderBindings(encoder, bindings);

    const NSUInteger group_threads = std::max<NSUInteger>(threads_per_group, 1);
    const NSUInteger max_threads = std::max<NSUInteger>(pipeline.maxTotalThreadsPerThreadgroup, 1);
    if (group_threads > max_threads) {
        [encoder endEncoding];
        error = "Requested threadgroup size exceeds pipeline limit";
        return false;
    }

    const MTLSize group_count = MTLSizeMake(std::max<NSUInteger>(threadgroups_width, 1), std::max<NSUInteger>(threadgroups_height, 1), 1);
    const MTLSize group_size = MTLSizeMake(group_threads, 1, 1);
    [encoder dispatchThreadgroups:group_count threadsPerThreadgroup:group_size];
    [encoder endEncoding];
    return true;
}

template <size_t N>
bool EncodeCompute2DTiles(id<MTLCommandBuffer> command,
                          id<MTLComputePipelineState> pipeline,
                          NSUInteger tiles_per_axis,
                          NSUInteger tile_dim,
                          const std::array<BufferBinding, N>& bindings,
                          std::string& error)
{
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (encoder == nil) {
        error = "Failed to create Metal compute encoder";
        return false;
    }

    [encoder setComputePipelineState:pipeline];
    SetEncoderBindings(encoder, bindings);

    const NSUInteger max_threads = std::max<NSUInteger>(pipeline.maxTotalThreadsPerThreadgroup, 1);
    if (tile_dim * tile_dim > max_threads) {
        [encoder endEncoding];
        error = "Requested 2D threadgroup size exceeds pipeline limit";
        return false;
    }

    [encoder dispatchThreadgroups:MTLSizeMake(tiles_per_axis, tiles_per_axis, 1)
            threadsPerThreadgroup:MTLSizeMake(tile_dim, tile_dim, 1)];
    [encoder endEncoding];
    return true;
}

id<MTLCommandBuffer> CreatePerformanceCommandBuffer(id<MTLCommandQueue> queue)
{
    if (queue == nil) {
        return nil;
    }
    // The Metal context owns the queue, pipelines, and pooled buffers for the
    // entire command lifetime, so unretained command buffers safely trim CPU
    // bookkeeping in the hot digest paths.
    return [queue commandBufferWithUnretainedReferences];
}

using FunctionConstantSpecializationMode = qtc::metal::detail::FunctionConstantMode;

FunctionConstantSpecializationMode ResolveFunctionConstantSpecializationMode()
{
    return qtc::metal::detail::ParseFunctionConstantEnv(
        std::getenv("QTC_MATMUL_METAL_FUNCTION_CONSTANTS"));
}

bool ShouldUseFunctionConstantSpecialization(uint32_t n)
{
    switch (ResolveFunctionConstantSpecializationMode()) {
    case FunctionConstantSpecializationMode::ENABLED:
        return true;
    case FunctionConstantSpecializationMode::DISABLED:
        return false;
    case FunctionConstantSpecializationMode::AUTO:
        // Compile-time (n, b, N) let the compiler fully unroll the b-wide inner
        // product loop for the production and regtest shapes.
        if (n >= 256) return true;
        return n <= 64;
    }
    return false;
}

enum class ProductKernelMode {
    AUTO,
    TILED,
    SIMPLE,
};

ProductKernelMode ResolveProductKernelMode()
{
    const char* env = std::getenv("QTC_MATMUL_METAL_PRODUCT_KERNEL");
    if (env == nullptr || env[0] == '\0') {
        return ProductKernelMode::AUTO;
    }
    const std::string value{env};
    if (value == "tiled") return ProductKernelMode::TILED;
    if (value == "simple") return ProductKernelMode::SIMPLE;
    return ProductKernelMode::AUTO;
}

// Pipeline selection for one attempt. Every candidate produces bit-identical
// C' (exact modular arithmetic); the choice is purely a performance one.
struct ProductPipelineSelection {
    id<MTLComputePipelineState> build_perturbed{nil};
    id<MTLComputePipelineState> build_product{nil};
    bool tiled{false};
};

ProductPipelineSelection SelectProductPipelines(const MetalContext& context,
                                                uint32_t n,
                                                uint32_t b,
                                                uint32_t r,
                                                uint32_t N)
{
    ProductPipelineSelection selection;
    selection.build_perturbed = context.build_perturbed_pipeline;
    selection.build_product = context.build_product_pipeline;

    const SpecializedKernelPipelines* specialized = nullptr;
    if (ShouldUseFunctionConstantSpecialization(n)) {
        specialized = FindSpecializedPipelines(context, n, b, r, N);
    }
    if (specialized != nullptr) {
        if (specialized->build_perturbed_pipeline != nil) {
            selection.build_perturbed = specialized->build_perturbed_pipeline;
        }
        if (specialized->build_product_pipeline != nil) {
            selection.build_product = specialized->build_product_pipeline;
        }
    }

    const bool tiled_supported = context.build_product_tiled_pipeline != nil && (n % PRODUCT_TILE_DIM) == 0;
    switch (ResolveProductKernelMode()) {
    case ProductKernelMode::TILED:
        selection.tiled = tiled_supported;
        break;
    case ProductKernelMode::SIMPLE:
        selection.tiled = false;
        break;
    case ProductKernelMode::AUTO:
        // Measured on Apple M5 at n=512/b=16/r=8: the per-element kernel with
        // function-constant specialization (~965 attempts/s) edges out the
        // 16x16 threadgroup-memory GEMM (~915 attempts/s); both are within
        // noise of each other and produce identical C'. Keep the tiled kernel
        // selectable via QTC_MATMUL_METAL_PRODUCT_KERNEL=tiled.
        selection.tiled = false;
        break;
    }
    if (selection.tiled) {
        selection.build_product = context.build_product_tiled_pipeline;
    }
    return selection;
}

// Encodes C' = A' * B' and the v4 per-tile hashes. a_prime/b_prime/c_prime are
// n*n word regions; tile_hashes is N*N*32 bytes.
bool EncodeProductAndTileHashes(id<MTLCommandBuffer> command,
                                const MetalContext& context,
                                const ProductPipelineSelection& selection,
                                id<MTLBuffer> params_buffer,
                                const BufferBinding& a_prime,
                                const BufferBinding& b_prime,
                                const BufferBinding& c_prime,
                                const BufferBinding& tile_hashes,
                                uint32_t n,
                                uint32_t b,
                                uint32_t N,
                                double& encode_product_us,
                                double& encode_tile_hash_us,
                                std::string& error)
{
    const std::array<BufferBinding, 4> product_bindings{{
        {params_buffer, 0},
        a_prime,
        b_prime,
        c_prime,
    }};
    const auto encode_product_start = std::chrono::steady_clock::now();
    bool encoded{false};
    if (selection.tiled) {
        encoded = EncodeCompute2DTiles(command,
                                       selection.build_product,
                                       static_cast<NSUInteger>(n / PRODUCT_TILE_DIM),
                                       PRODUCT_TILE_DIM,
                                       product_bindings,
                                       error);
    } else {
        encoded = EncodeComputeThreadgroupsBindings(command,
                                                    selection.build_product,
                                                    static_cast<NSUInteger>(N),
                                                    static_cast<NSUInteger>(N),
                                                    static_cast<NSUInteger>(b) * b,
                                                    product_bindings,
                                                    error);
    }
    if (!encoded) {
        return false;
    }
    encode_product_us += std::chrono::duration<double, std::micro>(
                             std::chrono::steady_clock::now() - encode_product_start)
                             .count();

    const std::array<BufferBinding, 3> hash_bindings{{
        {params_buffer, 0},
        c_prime,
        tile_hashes,
    }};
    const auto encode_hash_start = std::chrono::steady_clock::now();
    if (!EncodeComputeBindings(command,
                               context.hash_product_tiles_pipeline,
                               static_cast<NSUInteger>(N) * N,
                               256,
                               hash_bindings,
                               error)) {
        return false;
    }
    encode_tile_hash_us += std::chrono::duration<double, std::micro>(
                               std::chrono::steady_clock::now() - encode_hash_start)
                               .count();
    return true;
}

NSUInteger OracleBlockGridSize(uint64_t element_count)
{
    return static_cast<NSUInteger>((element_count + 7) / 8);
}

bool EncodeGenerateBaseMatrix(id<MTLCommandBuffer> command,
                              const MetalContext& context,
                              id<MTLBuffer> params_buffer,
                              id<MTLBuffer> seed_buffer,
                              size_t seed_offset,
                              id<MTLBuffer> output_buffer,
                              size_t output_offset,
                              uint64_t matrix_words,
                              std::string& error)
{
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (encoder == nil) {
        error = "Failed to create Metal base matrix generation encoder";
        return false;
    }
    [encoder setComputePipelineState:context.generate_base_matrix_pipeline];
    [encoder setBuffer:params_buffer offset:0 atIndex:0];
    [encoder setBuffer:seed_buffer offset:static_cast<NSUInteger>(seed_offset) atIndex:1];
    [encoder setBuffer:output_buffer offset:static_cast<NSUInteger>(output_offset) atIndex:2];
    const NSUInteger group_size = SelectThreadGroupSize(context.generate_base_matrix_pipeline, 256);
    [encoder dispatchThreads:MTLSizeMake(OracleBlockGridSize(matrix_words), 1, 1)
        threadsPerThreadgroup:MTLSizeMake(group_size, 1, 1)];
    [encoder endEncoding];
    return true;
}

struct KernelParamsHost {
    uint32_t n;
    uint32_t b;
    uint32_t r;
    uint32_t N;
};

} // namespace

namespace qtc::metal {

bool ShouldUseFunctionConstantSpecializationPolicy(uint32_t n, bool /*legacy_unused*/)
{
    return ShouldUseFunctionConstantSpecialization(n);
}

MatMulAccelerationProbe ProbeMatMulDigestAcceleration()
{
    MatMulAccelerationProbe probe;
    const MetalContext& context = GetContext();
    probe.available = context.ready;
    probe.reason = context.ready ? "runtime_probe_ok"
                                 : (context.error.empty() ? "runtime_probe_failed" : context.error);
    return probe;
}

MatMulDeviceInfo ProbeMatMulDeviceInfo()
{
    MatMulDeviceInfo info;
    const MetalContext& context = GetContext();
    info.available = context.ready;
    info.reason = context.ready ? "runtime_probe_ok"
                                : (context.error.empty() ? "runtime_probe_failed" : context.error);
    if (!context.ready) {
        return info;
    }

    if (context.device != nil) {
        NSString* device_name = [context.device name];
        if (device_name != nil) {
            info.device_name = [device_name UTF8String];
        }
    }

    const MetalGpuCoreCountProbe gpu_core_count = ResolveAppleMetalGpuCoreCountFromIORegistry();
    info.gpu_core_count = gpu_core_count.core_count;
    info.gpu_core_count_source = gpu_core_count.source;
    return info;
}

// Compute a lightweight data fingerprint for safe cache invalidation.
// Uses dimension + sampled data points so we never compare dangling pointers.
uint64_t ComputeMatrixDataFingerprint(uint32_t n,
                                       const matmul::field::Element* matrix_a,
                                       const matmul::field::Element* matrix_b)
{
    const uint64_t nn = static_cast<uint64_t>(n) * n;
    // Mix dimension, first word, last word, and middle words of both matrices.
    uint64_t fp = static_cast<uint64_t>(n);
    if (nn > 0) {
        fp ^= static_cast<uint64_t>(matrix_a[0]) * 0x9E3779B97F4A7C15ULL;
        fp ^= static_cast<uint64_t>(matrix_b[0]) * 0x517CC1B727220A95ULL;
        fp ^= static_cast<uint64_t>(matrix_a[nn - 1]) * 0x6C62272E07BB0142ULL;
        fp ^= static_cast<uint64_t>(matrix_b[nn - 1]) * 0x62B821756295C58DULL;
        if (nn > 2) {
            const uint64_t mid = nn / 2;
            fp ^= static_cast<uint64_t>(matrix_a[mid]) * 0x3C6EF372FE94F82BULL;
            fp ^= static_cast<uint64_t>(matrix_b[mid]) * 0x27BB2EE687B0B0FDULL;
        }
    }
    return fp;
}

MatMulBaseMatricesResult UploadBaseMatrices(const MatMulBaseMatricesRequest& request)
{
    MatMulBaseMatricesResult result;

    MetalContext& context = GetContext();
    if (!context.ready) {
        result.available = false;
        result.success = false;
        result.error = context.error.empty() ? "Metal context initialization failed" : context.error;
        return result;
    }
    result.available = true;

    if (request.n == 0 || request.matrix_a == nullptr || request.matrix_b == nullptr) {
        result.success = false;
        result.error = "invalid base matrix upload request";
        return result;
    }

    const uint64_t matrix_words = static_cast<uint64_t>(request.n) * request.n;
    if (matrix_words > std::numeric_limits<uint32_t>::max()) {
        result.success = false;
        result.error = "base matrix dimensions exceed supported Metal upload bounds";
        return result;
    }
    const size_t matrix_bytes = static_cast<size_t>(matrix_words * sizeof(uint32_t));

    // Use a data fingerprint instead of raw pointer comparison to detect
    // when the caller's matrix data has changed.  Raw pointer comparison
    // is unsafe because a freed matrix allocation can be reused at the
    // same address with different data (use-after-free cache stale hit).
    const uint64_t fingerprint = ComputeMatrixDataFingerprint(request.n, request.matrix_a, request.matrix_b);

    std::lock_guard<std::mutex> lock(context.resident_base_mutex);
    if (context.resident_matrix_a_buffer != nil &&
        context.resident_matrix_b_buffer != nil &&
        context.resident_matrix_n == request.n &&
        context.resident_data_fingerprint == fingerprint) {
        result.success = true;
        return result;
    }

    // Reuse existing resident buffers when the matrix dimension is unchanged.
    // This avoids repeated per-block Metal buffer allocation churn during
    // mining while preserving uploaded-base functionality.
    if (context.resident_matrix_a_buffer != nil &&
        context.resident_matrix_b_buffer != nil &&
        context.resident_matrix_n == request.n) {
        std::memcpy(context.resident_matrix_a_buffer.contents, request.matrix_a, matrix_bytes);
        std::memcpy(context.resident_matrix_b_buffer.contents, request.matrix_b, matrix_bytes);
        context.resident_data_fingerprint = fingerprint;
        result.success = true;
        return result;
    }

    id<MTLBuffer> matrix_a_buffer = [context.device newBufferWithLength:matrix_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> matrix_b_buffer = [context.device newBufferWithLength:matrix_bytes options:MTLResourceStorageModeShared];
    if (matrix_a_buffer == nil || matrix_b_buffer == nil) {
        result.success = false;
        result.error = "failed to allocate resident Metal base matrix buffers";
        return result;
    }

    std::memcpy(matrix_a_buffer.contents, request.matrix_a, matrix_bytes);
    std::memcpy(matrix_b_buffer.contents, request.matrix_b, matrix_bytes);

    context.resident_matrix_a_buffer = matrix_a_buffer;
    context.resident_matrix_b_buffer = matrix_b_buffer;
    context.resident_matrix_n = request.n;
    context.resident_data_fingerprint = fingerprint;

    result.success = true;
    return result;
}

MatMulGeneratedBaseMatrixResult GenerateBaseMatrixFromSeedForTesting(uint32_t n, const uint256& seed)
{
    MatMulGeneratedBaseMatrixResult result;

    MetalContext& context = GetContext();
    if (!context.ready) {
        result.available = false;
        result.success = false;
        result.error = context.error.empty() ? "Metal context initialization failed" : context.error;
        return result;
    }
    result.available = true;

    if (n == 0) {
        result.error = "invalid base matrix generation request";
        return result;
    }
    const uint64_t matrix_words64 = static_cast<uint64_t>(n) * n;
    if (matrix_words64 > std::numeric_limits<uint32_t>::max()) {
        result.error = "base matrix dimensions exceed supported Metal generation bounds";
        return result;
    }
    const uint32_t matrix_words = static_cast<uint32_t>(matrix_words64);
    const size_t matrix_bytes = static_cast<size_t>(matrix_words) * sizeof(matmul::field::Element);

    const KernelParamsHost params{n, 1, 1, n};

    @autoreleasepool {
        id<MTLBuffer> output_buffer = [context.device newBufferWithLength:matrix_bytes
                                                                  options:MTLResourceStorageModeShared];
        if (output_buffer == nil) {
            result.error = "failed to allocate Metal base matrix generation output buffer";
            return result;
        }

        id<MTLCommandBuffer> command = CreatePerformanceCommandBuffer(context.pool_slots.empty()
            ? nil
            : context.pool_slots.front().queue);
        if (command == nil) {
            command = CreatePerformanceCommandBuffer([context.device newCommandQueue]);
        }
        if (command == nil) {
            result.error = "failed to create Metal base matrix generation command buffer";
            return result;
        }

        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (encoder == nil) {
            result.error = "failed to create Metal base matrix generation encoder";
            return result;
        }
        [encoder setComputePipelineState:context.generate_base_matrix_pipeline];
        [encoder setBytes:&params length:sizeof(params) atIndex:0];
        [encoder setBytes:seed.data() length:uint256::size() atIndex:1];
        [encoder setBuffer:output_buffer offset:0 atIndex:2];
        const NSUInteger group_size = SelectThreadGroupSize(context.generate_base_matrix_pipeline, 256);
        [encoder dispatchThreads:MTLSizeMake(OracleBlockGridSize(matrix_words), 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group_size, 1, 1)];
        [encoder endEncoding];

        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            NSString* description = command.error != nil ? [command.error localizedDescription] : @"unknown Metal command failure";
            result.error = [description UTF8String];
            return result;
        }

        const auto* words = static_cast<const matmul::field::Element*>(output_buffer.contents);
        if (words == nullptr) {
            result.error = "Metal base matrix generation output buffer has no contents";
            return result;
        }
        result.matrix.assign(words, words + matrix_words);
        result.success = true;
        return result;
    }
}

MatMulVariableBaseProductResult GenerateVariableBaseProductForTesting(
    const MatMulVariableBaseProductRequest& request)
{
    MatMulVariableBaseProductResult result;

    MetalContext& context = GetContext();
    if (!context.ready) {
        result.available = false;
        result.success = false;
        result.error = context.error.empty() ? "Metal context initialization failed" : context.error;
        return result;
    }
    result.available = true;

    if (request.noise_e_l == nullptr || request.noise_e_r == nullptr ||
        request.noise_f_l == nullptr || request.noise_f_r == nullptr) {
        result.error = "invalid variable-base product request: missing input pointer";
        return result;
    }

    ShapeParams shape;
    if (!BuildKernelParamsForShape(request.n, request.b, request.r, shape, result.error)) {
        return result;
    }
    const uint32_t N = shape.N;
    const KernelParamsHost params{request.n, request.b, request.r, N};
    const ProductPipelineSelection selection = SelectProductPipelines(context, request.n, request.b, request.r, N);

    @autoreleasepool {
        const size_t matrix_bytes = static_cast<size_t>(shape.matrix_words) * sizeof(uint32_t);
        const size_t noise_bytes = static_cast<size_t>(shape.noise_words) * sizeof(uint32_t);
        const size_t tile_hash_bytes = static_cast<size_t>(shape.tile_count) * TILE_HASH_BYTES;

        id<MTLBuffer> params_buffer = [context.device newBufferWithLength:sizeof(params)
                                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> seed_buffer = [context.device newBufferWithLength:2 * uint256::size()
                                                                 options:MTLResourceStorageModeShared];
        id<MTLBuffer> matrix_a_buffer = [context.device newBufferWithLength:matrix_bytes
                                                                    options:MTLResourceStorageModeShared];
        id<MTLBuffer> matrix_b_buffer = [context.device newBufferWithLength:matrix_bytes
                                                                    options:MTLResourceStorageModeShared];
        id<MTLBuffer> e_l_buffer = [context.device newBufferWithLength:noise_bytes
                                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> e_r_buffer = [context.device newBufferWithLength:noise_bytes
                                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> f_l_buffer = [context.device newBufferWithLength:noise_bytes
                                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> f_r_buffer = [context.device newBufferWithLength:noise_bytes
                                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> a_prime_buffer = [context.device newBufferWithLength:matrix_bytes
                                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_prime_buffer = [context.device newBufferWithLength:matrix_bytes
                                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> c_prime_buffer = [context.device newBufferWithLength:matrix_bytes
                                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> tile_hash_buffer = [context.device newBufferWithLength:tile_hash_bytes
                                                                     options:MTLResourceStorageModeShared];
        if (params_buffer == nil || seed_buffer == nil || matrix_a_buffer == nil || matrix_b_buffer == nil ||
            e_l_buffer == nil || e_r_buffer == nil || f_l_buffer == nil || f_r_buffer == nil ||
            a_prime_buffer == nil || b_prime_buffer == nil || c_prime_buffer == nil || tile_hash_buffer == nil) {
            result.error = "failed to allocate Metal variable-base product diagnostic buffers";
            return result;
        }

        std::memcpy(params_buffer.contents, &params, sizeof(params));
        auto* seed_bytes = static_cast<unsigned char*>(seed_buffer.contents);
        std::memcpy(seed_bytes, request.matrix_a_seed.data(), uint256::size());
        std::memcpy(seed_bytes + uint256::size(), request.matrix_b_seed.data(), uint256::size());
        std::memcpy(e_l_buffer.contents, request.noise_e_l, noise_bytes);
        std::memcpy(e_r_buffer.contents, request.noise_e_r, noise_bytes);
        std::memcpy(f_l_buffer.contents, request.noise_f_l, noise_bytes);
        std::memcpy(f_r_buffer.contents, request.noise_f_r, noise_bytes);

        id<MTLCommandBuffer> command = CreatePerformanceCommandBuffer(context.pool_slots.empty()
            ? nil
            : context.pool_slots.front().queue);
        if (command == nil) {
            command = CreatePerformanceCommandBuffer([context.device newCommandQueue]);
        }
        if (command == nil) {
            result.error = "failed to create Metal variable-base product diagnostic command buffer";
            return result;
        }

        std::string encode_error;
        if (!EncodeGenerateBaseMatrix(command, context, params_buffer, seed_buffer, 0, matrix_a_buffer, 0, shape.matrix_words, encode_error) ||
            !EncodeGenerateBaseMatrix(command, context, params_buffer, seed_buffer, uint256::size(), matrix_b_buffer, 0, shape.matrix_words, encode_error)) {
            result.error = encode_error;
            return result;
        }

        const std::array<BufferBinding, 9> build_bindings{{
            {params_buffer, 0},
            {matrix_a_buffer, 0},
            {matrix_b_buffer, 0},
            {e_l_buffer, 0},
            {e_r_buffer, 0},
            {f_l_buffer, 0},
            {f_r_buffer, 0},
            {a_prime_buffer, 0},
            {b_prime_buffer, 0},
        }};
        if (!EncodeComputeBindings(command,
                                   selection.build_perturbed,
                                   static_cast<NSUInteger>(shape.matrix_words),
                                   256,
                                   build_bindings,
                                   encode_error)) {
            result.error = encode_error;
            return result;
        }

        double encode_product_us{0.0};
        double encode_tile_hash_us{0.0};
        if (!EncodeProductAndTileHashes(command,
                                        context,
                                        selection,
                                        params_buffer,
                                        BufferBinding{a_prime_buffer, 0},
                                        BufferBinding{b_prime_buffer, 0},
                                        BufferBinding{c_prime_buffer, 0},
                                        BufferBinding{tile_hash_buffer, 0},
                                        request.n,
                                        request.b,
                                        N,
                                        encode_product_us,
                                        encode_tile_hash_us,
                                        encode_error)) {
            result.error = encode_error;
            return result;
        }

        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            NSString* description = command.error != nil ? [command.error localizedDescription] : @"unknown Metal command failure";
            result.error = [description UTF8String];
            return result;
        }

        const auto* matrix_a_words = static_cast<const matmul::field::Element*>(matrix_a_buffer.contents);
        const auto* matrix_b_words = static_cast<const matmul::field::Element*>(matrix_b_buffer.contents);
        const auto* a_prime_words = static_cast<const matmul::field::Element*>(a_prime_buffer.contents);
        const auto* b_prime_words = static_cast<const matmul::field::Element*>(b_prime_buffer.contents);
        const auto* c_prime_words = static_cast<const matmul::field::Element*>(c_prime_buffer.contents);
        const auto* tile_hash_bytes_ptr = static_cast<const unsigned char*>(tile_hash_buffer.contents);
        if (matrix_a_words == nullptr || matrix_b_words == nullptr ||
            a_prime_words == nullptr || b_prime_words == nullptr ||
            c_prime_words == nullptr || tile_hash_bytes_ptr == nullptr) {
            result.error = "Metal variable-base product diagnostic output buffer has no contents";
            return result;
        }

        result.matrix_a.assign(matrix_a_words, matrix_a_words + shape.matrix_words);
        result.matrix_b.assign(matrix_b_words, matrix_b_words + shape.matrix_words);
        result.a_prime.assign(a_prime_words, a_prime_words + shape.matrix_words);
        result.b_prime.assign(b_prime_words, b_prime_words + shape.matrix_words);
        result.c_prime.assign(c_prime_words, c_prime_words + shape.matrix_words);
        result.tile_hashes.reserve(static_cast<size_t>(shape.tile_count));
        for (uint64_t tile = 0; tile < shape.tile_count; ++tile) {
            result.tile_hashes.emplace_back(Span<const unsigned char>{
                tile_hash_bytes_ptr + static_cast<size_t>(tile) * TILE_HASH_BYTES,
                TILE_HASH_BYTES,
            });
        }
        result.success = true;
        return result;
    }
}

MatMulBufferPoolStats ProbeMatMulBufferPool()
{
    MatMulBufferPoolStats stats;

    MetalContext& context = GetContext();
    if (!context.ready) {
        stats.available = false;
        stats.initialized = false;
        stats.reason = context.error.empty() ? "Metal context initialization failed" : context.error;
        return stats;
    }
    stats.available = true;

    std::lock_guard<std::mutex> lock(context.pool_mutex);
    stats.slot_count = static_cast<uint32_t>(context.pool_slots.size());
    stats.active_slots = context.pool_active_slots;
    stats.high_water_slots = context.pool_high_water_slots;
    stats.inflight_submissions = context.pool_inflight_submissions;
    stats.peak_inflight_submissions = context.pool_peak_inflight_submissions;
    stats.initialized = std::any_of(context.pool_slots.begin(), context.pool_slots.end(), [](const MetalPoolSlot& slot) {
        return IsPoolSlotInitialized(slot);
    });
    stats.allocation_events = context.pool_allocation_events;
    stats.reuse_events = context.pool_reuse_events;
    stats.wait_events = context.pool_wait_events;
    stats.completed_submissions = context.pool_completed_submissions;
    stats.n = context.pool_last_n;
    stats.b = context.pool_last_b;
    stats.r = context.pool_last_r;
    stats.reason = stats.initialized ? "buffer_pool_slots_ready" : "buffer_pool_uninitialized";
    return stats;
}

MatMulDispatchConfig ProbeMatMulDispatchConfig()
{
    MatMulDispatchConfig config;

    MetalContext& context = GetContext();
    if (!context.ready) {
        config.available = false;
        config.reason = context.error.empty() ? "Metal context initialization failed" : context.error;
        return config;
    }
    config.available = true;

    config.build_perturbed_threads = SelectThreadGroupSize(context.build_perturbed_pipeline, 256);
    config.build_prefix_threads = SelectThreadGroupSize(context.build_product_pipeline, 256);
    config.compress_prefix_threads = SelectThreadGroupSize(context.hash_product_tiles_pipeline, 256);
    config.reason = "dispatch_probe_ok";
    return config;
}

MatMulKernelProfile ProbeMatMulKernelProfile()
{
    MatMulKernelProfile profile;

    MetalContext& context = GetContext();
    if (!context.ready) {
        profile.available = false;
        profile.reason = context.error.empty() ? "Metal context initialization failed" : context.error;
        return profile;
    }

    profile.available = true;
    profile.tiled_build_prefix = context.build_product_tiled_pipeline != nil;
    profile.fused_prefix_compress = context.build_product_pipeline != nil;
    profile.gpu_transcript_hash = context.hash_product_tiles_pipeline != nil;
    profile.function_constant_specialization = context.specialized_pipeline_count > 0;
    profile.cooperative_tensor_prepared = true;
    profile.cooperative_tensor_active = false;
    profile.uses_prefix_buffer = false;
    profile.specialized_shape_count = context.specialized_pipeline_count;
    profile.build_prefix_threadgroup_width = PRODUCT_TILE_DIM;
    profile.build_prefix_threadgroup_height = PRODUCT_TILE_DIM;
    profile.fused_prefix_threadgroup_threads = SelectThreadGroupSize(context.build_product_pipeline, 256);
    profile.specialization_reason = context.specialized_pipeline_reason;
    profile.cooperative_tensor_reason = "simdgroup_uint32_reduce_active_integer_cooperative_tensor_unavailable";
    profile.library_source = context.using_precompiled_library ? "precompiled_metallib" : "inline_source_fallback";
    profile.reason = profile.function_constant_specialization
        ? "product_digest_v4_gemm_gpu_tile_hash_host_root_pipeline_with_function_constants"
        : "product_digest_v4_gemm_gpu_tile_hash_host_root_pipeline";
    return profile;
}

MatMulProfilingStats ProbeMatMulProfilingStats()
{
    MatMulProfilingStats stats;

    MetalContext& context = GetContext();
    if (!context.ready) {
        stats.available = false;
        stats.reason = context.error.empty() ? "Metal context initialization failed" : context.error;
        return stats;
    }

    std::lock_guard<std::mutex> lock(context.profiling_mutex);
    stats = context.profiling_stats;
    stats.available = true;
    stats.capture_supported = context.capture_supported;
    if (stats.reason.empty()) {
        stats.reason = "profiling_probe_ok";
    }
    return stats;
}

namespace {

template <typename State>
void RecordAsyncSubmissionStart(MetalContext& context, const std::shared_ptr<State>& state)
{
    state->context = &context;
    std::lock_guard<std::mutex> lock(context.pool_mutex);
    ++context.pool_inflight_submissions;
    context.pool_peak_inflight_submissions =
        std::max(context.pool_peak_inflight_submissions, context.pool_inflight_submissions);
}

} // namespace

MatMulDigestSubmission SubmitCanonicalTranscriptDigest(const MatMulDigestRequest& request)
{
    MatMulDigestSubmission submission;

    MetalContext& context = GetContext();
    if (!context.ready) {
        submission.available = false;
        submission.error = context.error.empty() ? "Metal context initialization failed" : context.error;
        return submission;
    }
    submission.available = true;

    ShapeParams shape;
    if (!BuildKernelParams(request, shape, submission.error)) {
        return submission;
    }
    const uint32_t N = shape.N;
    const KernelParamsHost params{request.n, request.b, request.r, N};
    const ProductPipelineSelection selection = SelectProductPipelines(context, request.n, request.b, request.r, N);

    auto state = std::make_shared<AsyncSingleDigestState>();
    state->result.available = true;

    @autoreleasepool {
        const PoolRequirements pool_req = MakePoolRequirements(request.n, request.b, request.r, shape, 1);
        const size_t matrix_bytes = pool_req.matrix_bytes;
        const size_t noise_bytes = pool_req.noise_bytes;

        auto pool_lease = AcquireBufferPoolLease(context, pool_req, submission.error);
        if (!pool_lease.has_value()) {
            return submission;
        }
        state->lease.emplace(std::move(pool_lease.value()));

        MetalPoolSlot& pool_slot = *state->lease->slot;
        id<MTLBuffer> params_buffer = pool_slot.params_buffer;
        id<MTLBuffer> matrix_a_buffer = pool_slot.matrix_a_stage_buffer;
        id<MTLBuffer> matrix_b_buffer = pool_slot.matrix_b_stage_buffer;
        id<MTLBuffer> e_l_buffer = pool_slot.e_l_buffer;
        id<MTLBuffer> e_r_buffer = pool_slot.e_r_buffer;
        id<MTLBuffer> f_l_buffer = pool_slot.f_l_buffer;
        id<MTLBuffer> f_r_buffer = pool_slot.f_r_buffer;
        id<MTLBuffer> a_prime_buffer = pool_slot.a_prime_buffer;
        id<MTLBuffer> b_prime_buffer = pool_slot.b_prime_buffer;
        id<MTLBuffer> c_prime_buffer = pool_slot.c_prime_buffer;
        id<MTLBuffer> tile_hash_buffer = pool_slot.tile_hash_buffer;
        state->retained_inputs = CreateRetainedInputArray(6);

        if (request.use_uploaded_base_matrices) {
            std::lock_guard<std::mutex> lock(context.resident_base_mutex);
            if (context.resident_matrix_a_buffer == nil || context.resident_matrix_b_buffer == nil || context.resident_matrix_n != request.n) {
                submission.error = "uploaded base matrices are unavailable or stale for requested dimension";
                return submission;
            }
            matrix_a_buffer = context.resident_matrix_a_buffer;
            matrix_b_buffer = context.resident_matrix_b_buffer;
        } else {
            id<MTLBuffer> matrix_a_no_copy = WrapSharedNoCopyBuffer(context.device, request.matrix_a, matrix_bytes);
            id<MTLBuffer> matrix_b_no_copy = WrapSharedNoCopyBuffer(context.device, request.matrix_b, matrix_bytes);
            if (matrix_a_no_copy != nil && matrix_b_no_copy != nil) {
                matrix_a_buffer = matrix_a_no_copy;
                matrix_b_buffer = matrix_b_no_copy;
                state->any_zero_copy_input = true;
                RetainTemporaryInputBuffer(state->retained_inputs, matrix_a_no_copy);
                RetainTemporaryInputBuffer(state->retained_inputs, matrix_b_no_copy);
            } else {
                std::memcpy(matrix_a_buffer.contents, request.matrix_a, matrix_bytes);
                std::memcpy(matrix_b_buffer.contents, request.matrix_b, matrix_bytes);
            }
        }

        auto copy_or_wrap = [&](const matmul::field::Element* source,
                                id<MTLBuffer> staging_buffer,
                                size_t bytes) -> id<MTLBuffer> {
            id<MTLBuffer> no_copy = WrapSharedNoCopyBuffer(context.device, source, bytes);
            if (no_copy != nil) {
                state->any_zero_copy_input = true;
                RetainTemporaryInputBuffer(state->retained_inputs, no_copy);
                return no_copy;
            }
            std::memcpy(staging_buffer.contents, source, bytes);
            return staging_buffer;
        };

        id<MTLBuffer> e_l_input_buffer = copy_or_wrap(request.noise_e_l, e_l_buffer, noise_bytes);
        id<MTLBuffer> e_r_input_buffer = copy_or_wrap(request.noise_e_r, e_r_buffer, noise_bytes);
        id<MTLBuffer> f_l_input_buffer = copy_or_wrap(request.noise_f_l, f_l_buffer, noise_bytes);
        id<MTLBuffer> f_r_input_buffer = copy_or_wrap(request.noise_f_r, f_r_buffer, noise_bytes);

        std::memcpy(params_buffer.contents, &params, sizeof(params));
        id<MTLCommandBuffer> command = CreatePerformanceCommandBuffer(pool_slot.queue);
        if (command == nil) {
            submission.error = "Failed to create Metal command buffer";
            return submission;
        }

        std::string encode_error;
        const std::array<BufferBinding, 9> buffers_1{{
            {params_buffer, 0},
            {matrix_a_buffer, 0},
            {matrix_b_buffer, 0},
            {e_l_input_buffer, 0},
            {e_r_input_buffer, 0},
            {f_l_input_buffer, 0},
            {f_r_input_buffer, 0},
            {a_prime_buffer, 0},
            {b_prime_buffer, 0},
        }};
        const auto encode_build_start = std::chrono::steady_clock::now();
        if (!EncodeComputeBindings(command,
                                   selection.build_perturbed,
                                   static_cast<NSUInteger>(shape.matrix_words),
                                   256,
                                   buffers_1,
                                   encode_error)) {
            submission.error = encode_error;
            return submission;
        }
        state->encode_build_perturbed_us = std::chrono::duration<double, std::micro>(
                                               std::chrono::steady_clock::now() - encode_build_start)
                                               .count();

        if (!EncodeProductAndTileHashes(command,
                                        context,
                                        selection,
                                        params_buffer,
                                        BufferBinding{a_prime_buffer, 0},
                                        BufferBinding{b_prime_buffer, 0},
                                        BufferBinding{c_prime_buffer, 0},
                                        BufferBinding{tile_hash_buffer, 0},
                                        request.n,
                                        request.b,
                                        N,
                                        state->encode_product_us,
                                        state->encode_tile_hash_us,
                                        encode_error)) {
            submission.error = encode_error;
            return submission;
        }

        state->n = request.n;
        state->b = request.b;
        state->N = N;
        state->sigma = request.sigma;
        state->tile_hash_buffer = tile_hash_buffer;

        RecordAsyncSubmissionStart(context, state);
        const auto submit_wait_start = std::chrono::steady_clock::now();
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            @autoreleasepool {
                state->submit_wait_us = std::chrono::duration<double, std::micro>(
                                            std::chrono::steady_clock::now() - submit_wait_start)
                                            .count();
                state->gpu_execution_ms = state->submit_wait_us / 1000.0;
                state->result.available = true;

                if (completed.status != MTLCommandBufferStatusCompleted) {
                    NSString* description = completed.error != nil ? [completed.error localizedDescription] : @"unknown Metal command failure";
                    state->result.success = false;
                    state->result.error = [description UTF8String];
                    FinalizeAsyncSubmissionState(state, "profiling_samples_ready_async_error");
                    return;
                }

                const auto finalize_start = std::chrono::steady_clock::now();
                if (!FinalizeProductDigestFromTileHashBytes(
                        static_cast<const unsigned char*>(state->tile_hash_buffer.contents),
                        state->N,
                        state->n,
                        state->b,
                        state->sigma,
                        state->result.digest,
                        state->result.error)) {
                    state->result.success = false;
                    FinalizeAsyncSubmissionState(state, "profiling_samples_ready_async_error");
                    return;
                }
                state->cpu_finalize_us = std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now() - finalize_start)
                                             .count();
                state->result.success = true;
                FinalizeAsyncSubmissionState(state, "profiling_samples_ready_async");
            }
        }];
        [command commit];
    }

    submission.submitted = true;
    submission.opaque = state;
    return submission;
}

bool IsCanonicalTranscriptDigestSubmissionReady(const MatMulDigestSubmission& submission)
{
    if (!submission.submitted || !submission.opaque) {
        return false;
    }
    auto state = std::static_pointer_cast<AsyncSingleDigestState>(submission.opaque);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->completed;
}

MatMulDigestResult WaitForCanonicalTranscriptDigestSubmission(MatMulDigestSubmission&& submission)
{
    MatMulDigestResult result;
    result.available = submission.available;
    if (!submission.submitted || !submission.opaque) {
        result.success = false;
        result.error = submission.error.empty() ? "Metal digest submission was not started" : submission.error;
        return result;
    }

    auto state = std::static_pointer_cast<AsyncSingleDigestState>(submission.opaque);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state] { return state->completed; });
    return state->result;
}

MatMulDigestResult ComputeCanonicalTranscriptDigest(const MatMulDigestRequest& request)
{
    auto submission = SubmitCanonicalTranscriptDigest(request);
    return WaitForCanonicalTranscriptDigestSubmission(std::move(submission));
}

MatMulDigestBatchSubmission SubmitCanonicalTranscriptDigestBatch(const MatMulDigestBatchRequest& request)
{
    MatMulDigestBatchSubmission submission;

    MetalContext& context = GetContext();
    if (!context.ready) {
        submission.available = false;
        submission.error = context.error.empty() ? "Metal context initialization failed" : context.error;
        return submission;
    }
    submission.available = true;

    if (request.batch_size == 0) {
        submission.error = "invalid MatMul batch request: batch_size must be non-zero";
        return submission;
    }
    if (!ValidateDigestMode(request.digest_mode, submission.error)) {
        return submission;
    }
    if (request.noise_e_l == nullptr || request.noise_e_r == nullptr ||
        request.noise_f_l == nullptr || request.noise_f_r == nullptr) {
        submission.error = "invalid MatMul batch request: missing batch input pointers";
        return submission;
    }

    for (uint32_t i = 0; i < request.batch_size; ++i) {
        if (request.noise_e_l[i] == nullptr || request.noise_e_r[i] == nullptr ||
            request.noise_f_l[i] == nullptr || request.noise_f_r[i] == nullptr) {
            submission.error = "invalid MatMul batch request: null per-batch input pointer";
            return submission;
        }
    }

    MatMulDigestRequest validation_request{
        .n = request.n,
        .b = request.b,
        .r = request.r,
        .digest_mode = request.digest_mode,
        .sigma = request.sigmas != nullptr ? request.sigmas[0] : uint256{},
        .matrix_a = request.matrix_a,
        .matrix_b = request.matrix_b,
        .use_uploaded_base_matrices = request.use_uploaded_base_matrices,
        .noise_e_l = request.noise_e_l[0],
        .noise_e_r = request.noise_e_r[0],
        .noise_f_l = request.noise_f_l[0],
        .noise_f_r = request.noise_f_r[0],
    };

    ShapeParams shape;
    if (!BuildKernelParams(validation_request, shape, submission.error)) {
        return submission;
    }
    const uint32_t N = shape.N;
    const KernelParamsHost params{request.n, request.b, request.r, N};
    const ProductPipelineSelection selection = SelectProductPipelines(context, request.n, request.b, request.r, N);

    auto state = std::make_shared<AsyncBatchDigestState>();
    state->result.available = true;
    state->batch_size = request.batch_size;

    @autoreleasepool {
        const PoolRequirements per_attempt = MakePoolRequirements(request.n, request.b, request.r, shape, 1);
        const size_t matrix_bytes = per_attempt.matrix_bytes;
        const size_t noise_bytes = per_attempt.noise_bytes;
        const size_t tile_hash_bytes = per_attempt.tile_hash_bytes;

        // The base matrices are shared across the batch (one staged copy); the
        // perturbed operands, C' and tile hashes get a private region per item.
        PoolRequirements pool_req = MakePoolRequirements(request.n, request.b, request.r, shape, request.batch_size);
        auto pool_lease = AcquireBufferPoolLease(context, pool_req, submission.error);
        if (!pool_lease.has_value()) {
            return submission;
        }
        state->lease.emplace(std::move(pool_lease.value()));

        MetalPoolSlot& pool_slot = *state->lease->slot;
        id<MTLBuffer> params_buffer = pool_slot.params_buffer;
        id<MTLBuffer> matrix_a_buffer = pool_slot.matrix_a_stage_buffer;
        id<MTLBuffer> matrix_b_buffer = pool_slot.matrix_b_stage_buffer;
        id<MTLBuffer> e_l_stage_buffer = pool_slot.e_l_buffer;
        id<MTLBuffer> e_r_stage_buffer = pool_slot.e_r_buffer;
        id<MTLBuffer> f_l_stage_buffer = pool_slot.f_l_buffer;
        id<MTLBuffer> f_r_stage_buffer = pool_slot.f_r_buffer;
        id<MTLBuffer> a_prime_buffer = pool_slot.a_prime_buffer;
        id<MTLBuffer> b_prime_buffer = pool_slot.b_prime_buffer;
        id<MTLBuffer> c_prime_buffer = pool_slot.c_prime_buffer;
        id<MTLBuffer> tile_hash_buffer = pool_slot.tile_hash_buffer;
        state->retained_inputs = CreateRetainedInputArray(static_cast<CFIndex>(request.batch_size * 4 + 2));

        if (request.use_uploaded_base_matrices) {
            std::lock_guard<std::mutex> lock(context.resident_base_mutex);
            if (context.resident_matrix_a_buffer == nil || context.resident_matrix_b_buffer == nil || context.resident_matrix_n != request.n) {
                submission.error = "uploaded base matrices are unavailable or stale for requested dimension";
                return submission;
            }
            matrix_a_buffer = context.resident_matrix_a_buffer;
            matrix_b_buffer = context.resident_matrix_b_buffer;
        } else {
            id<MTLBuffer> matrix_a_no_copy = WrapSharedNoCopyBuffer(context.device, request.matrix_a, matrix_bytes);
            id<MTLBuffer> matrix_b_no_copy = WrapSharedNoCopyBuffer(context.device, request.matrix_b, matrix_bytes);
            if (matrix_a_no_copy != nil && matrix_b_no_copy != nil) {
                matrix_a_buffer = matrix_a_no_copy;
                matrix_b_buffer = matrix_b_no_copy;
                state->any_zero_copy_input = true;
                RetainTemporaryInputBuffer(state->retained_inputs, matrix_a_no_copy);
                RetainTemporaryInputBuffer(state->retained_inputs, matrix_b_no_copy);
            } else {
                std::memcpy(matrix_a_buffer.contents, request.matrix_a, matrix_bytes);
                std::memcpy(matrix_b_buffer.contents, request.matrix_b, matrix_bytes);
            }
        }

        std::memcpy(params_buffer.contents, &params, sizeof(params));
        id<MTLCommandBuffer> command = CreatePerformanceCommandBuffer(pool_slot.queue);
        if (command == nil) {
            submission.error = "Failed to create Metal command buffer";
            return submission;
        }

        std::string encode_error;
        for (uint32_t i = 0; i < request.batch_size; ++i) {
            auto make_input_binding = [&](const matmul::field::Element* source,
                                          id<MTLBuffer> staging_buffer,
                                          size_t bytes,
                                          size_t stage_index) -> BufferBinding {
                id<MTLBuffer> no_copy = WrapSharedNoCopyBuffer(context.device, source, bytes);
                if (no_copy != nil) {
                    state->any_zero_copy_input = true;
                    RetainTemporaryInputBuffer(state->retained_inputs, no_copy);
                    return BufferBinding{no_copy, 0};
                }
                if (staging_buffer == nil) {
                    return BufferBinding{};
                }
                const size_t offset = stage_index * bytes;
                std::memcpy(static_cast<unsigned char*>(staging_buffer.contents) + offset, source, bytes);
                return BufferBinding{staging_buffer, static_cast<NSUInteger>(offset)};
            };

            const BufferBinding e_l_input = make_input_binding(request.noise_e_l[i], e_l_stage_buffer, noise_bytes, i);
            const BufferBinding e_r_input = make_input_binding(request.noise_e_r[i], e_r_stage_buffer, noise_bytes, i);
            const BufferBinding f_l_input = make_input_binding(request.noise_f_l[i], f_l_stage_buffer, noise_bytes, i);
            const BufferBinding f_r_input = make_input_binding(request.noise_f_r[i], f_r_stage_buffer, noise_bytes, i);
            if (e_l_input.buffer == nil || e_r_input.buffer == nil || f_l_input.buffer == nil || f_r_input.buffer == nil) {
                submission.error = "Failed to allocate Metal batch input buffers";
                return submission;
            }
            const NSUInteger matrix_offset = static_cast<NSUInteger>(static_cast<size_t>(i) * matrix_bytes);
            const NSUInteger tile_hash_offset = static_cast<NSUInteger>(static_cast<size_t>(i) * tile_hash_bytes);

            const std::array<BufferBinding, 9> buffers_1{{
                {params_buffer, 0},
                {matrix_a_buffer, 0},
                {matrix_b_buffer, 0},
                e_l_input,
                e_r_input,
                f_l_input,
                f_r_input,
                {a_prime_buffer, matrix_offset},
                {b_prime_buffer, matrix_offset},
            }};
            const auto encode_build_start = std::chrono::steady_clock::now();
            if (!EncodeComputeBindings(command,
                                       selection.build_perturbed,
                                       static_cast<NSUInteger>(shape.matrix_words),
                                       256,
                                       buffers_1,
                                       encode_error)) {
                submission.error = encode_error;
                return submission;
            }
            state->encode_build_perturbed_us += std::chrono::duration<double, std::micro>(
                                                    std::chrono::steady_clock::now() - encode_build_start)
                                                    .count();

            if (!EncodeProductAndTileHashes(command,
                                            context,
                                            selection,
                                            params_buffer,
                                            BufferBinding{a_prime_buffer, matrix_offset},
                                            BufferBinding{b_prime_buffer, matrix_offset},
                                            BufferBinding{c_prime_buffer, matrix_offset},
                                            BufferBinding{tile_hash_buffer, tile_hash_offset},
                                            request.n,
                                            request.b,
                                            N,
                                            state->encode_product_us,
                                            state->encode_tile_hash_us,
                                            encode_error)) {
                submission.error = encode_error;
                return submission;
            }
        }

        state->n = request.n;
        state->b = request.b;
        state->N = N;
        state->tile_hash_bytes_per_item = tile_hash_bytes;
        if (request.sigmas != nullptr) {
            state->sigmas.assign(request.sigmas, request.sigmas + request.batch_size);
        } else {
            state->sigmas.assign(request.batch_size, uint256{});
        }
        state->tile_hash_buffer = tile_hash_buffer;

        RecordAsyncSubmissionStart(context, state);
        const auto submit_wait_start = std::chrono::steady_clock::now();
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            @autoreleasepool {
                state->submit_wait_us = std::chrono::duration<double, std::micro>(
                                            std::chrono::steady_clock::now() - submit_wait_start)
                                            .count();
                state->gpu_execution_ms = state->submit_wait_us / 1000.0;
                state->result.available = true;

                if (completed.status != MTLCommandBufferStatusCompleted) {
                    NSString* description = completed.error != nil ? [completed.error localizedDescription] : @"unknown Metal command failure";
                    state->result.success = false;
                    state->result.error = [description UTF8String];
                    FinalizeAsyncSubmissionState(state, "profiling_samples_ready_batch_async_error");
                    return;
                }

                const auto finalize_start = std::chrono::steady_clock::now();
                state->result.digests.reserve(state->batch_size);
                const auto* tile_hash_bytes_ptr = static_cast<const unsigned char*>(state->tile_hash_buffer.contents);
                for (uint32_t i = 0; i < state->batch_size; ++i) {
                    uint256 digest;
                    std::string finalize_error;
                    if (!FinalizeProductDigestFromTileHashBytes(
                            tile_hash_bytes_ptr == nullptr
                                ? nullptr
                                : tile_hash_bytes_ptr + static_cast<size_t>(i) * state->tile_hash_bytes_per_item,
                            state->N,
                            state->n,
                            state->b,
                            state->sigmas[i],
                            digest,
                            finalize_error)) {
                        state->result.success = false;
                        state->result.error = finalize_error;
                        FinalizeAsyncSubmissionState(state, "profiling_samples_ready_batch_async_error");
                        return;
                    }
                    state->result.digests.push_back(digest);
                }
                state->cpu_finalize_us = std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now() - finalize_start)
                                             .count();
                state->result.success = true;
                FinalizeAsyncSubmissionState(state, "profiling_samples_ready_batch_async");
            }
        }];
        [command commit];
    }

    submission.submitted = true;
    submission.opaque = state;
    return submission;
}

bool IsCanonicalTranscriptDigestBatchSubmissionReady(const MatMulDigestBatchSubmission& submission)
{
    if (!submission.submitted || !submission.opaque) {
        return false;
    }
    auto state = std::static_pointer_cast<AsyncBatchDigestState>(submission.opaque);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->completed;
}

MatMulDigestBatchResult WaitForCanonicalTranscriptDigestBatchSubmission(MatMulDigestBatchSubmission&& submission)
{
    MatMulDigestBatchResult result;
    result.available = submission.available;
    if (!submission.submitted || !submission.opaque) {
        result.success = false;
        result.error = submission.error.empty() ? "Metal batch digest submission was not started" : submission.error;
        return result;
    }

    auto state = std::static_pointer_cast<AsyncBatchDigestState>(submission.opaque);
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state] { return state->completed; });
    return state->result;
}

MatMulDigestBatchResult ComputeCanonicalTranscriptDigestBatch(const MatMulDigestBatchRequest& request)
{
    auto submission = SubmitCanonicalTranscriptDigestBatch(request);
    return WaitForCanonicalTranscriptDigestBatchSubmission(std::move(submission));
}

MatMulDigestBatchResult ComputeCanonicalTranscriptDigestVariableBaseBatch(
    const MatMulVariableBaseDigestBatchRequest& request)
{
    MatMulDigestBatchResult result;

    MetalContext& context = GetContext();
    if (!context.ready) {
        result.available = false;
        result.success = false;
        result.error = context.error.empty() ? "Metal context initialization failed" : context.error;
        return result;
    }
    result.available = true;

    if (request.batch_size == 0) {
        result.error = "invalid Metal variable-base batch request: batch_size must be non-zero";
        return result;
    }
    if (!ValidateDigestMode(request.digest_mode, result.error)) {
        return result;
    }
    if (request.sigmas == nullptr) {
        result.error = "invalid Metal variable-base batch request: missing per-batch sigma values";
        return result;
    }
    if (request.matrix_a_seeds == nullptr || request.matrix_b_seeds == nullptr) {
        result.error = "invalid Metal variable-base batch request: missing matrix seeds";
        return result;
    }
    if (request.noise_e_l == nullptr || request.noise_e_r == nullptr ||
        request.noise_f_l == nullptr || request.noise_f_r == nullptr) {
        result.error = "invalid Metal variable-base batch request: missing batch input pointers";
        return result;
    }
    for (uint32_t i = 0; i < request.batch_size; ++i) {
        if (request.noise_e_l[i] == nullptr || request.noise_e_r[i] == nullptr ||
            request.noise_f_l[i] == nullptr || request.noise_f_r[i] == nullptr) {
            result.error = "invalid Metal variable-base batch request: null per-batch input pointer";
            return result;
        }
    }

    ShapeParams shape;
    if (!BuildKernelParamsForShape(request.n, request.b, request.r, shape, result.error)) {
        return result;
    }
    const uint32_t N = shape.N;
    const KernelParamsHost params{request.n, request.b, request.r, N};
    const ProductPipelineSelection selection = SelectProductPipelines(context, request.n, request.b, request.r, N);

    struct ScopedPoolSubmissionAccounting {
        MetalContext& context;
        explicit ScopedPoolSubmissionAccounting(MetalContext& context_in) : context(context_in)
        {
            std::lock_guard<std::mutex> lock(context.pool_mutex);
            ++context.pool_inflight_submissions;
            context.pool_peak_inflight_submissions =
                std::max(context.pool_peak_inflight_submissions, context.pool_inflight_submissions);
        }
        ~ScopedPoolSubmissionAccounting()
        {
            std::lock_guard<std::mutex> lock(context.pool_mutex);
            if (context.pool_inflight_submissions > 0) {
                --context.pool_inflight_submissions;
            }
            ++context.pool_completed_submissions;
        }
    };

    @autoreleasepool {
        const PoolRequirements per_attempt = MakePoolRequirements(request.n, request.b, request.r, shape, 1);
        const size_t matrix_bytes = per_attempt.matrix_bytes;
        const size_t noise_bytes = per_attempt.noise_bytes;
        const size_t tile_hash_bytes = per_attempt.tile_hash_bytes;

        const PoolRequirements pool_req = MakePoolRequirements(request.n, request.b, request.r, shape, request.batch_size);
        auto pool_lease = AcquireBufferPoolLease(context, pool_req, result.error);
        if (!pool_lease.has_value()) {
            return result;
        }
        ScopedPoolSubmissionAccounting accounting{context};

        MetalPoolSlot& pool_slot = *pool_lease->slot;
        id<MTLBuffer> params_buffer = pool_slot.params_buffer;
        id<MTLBuffer> matrix_a_buffer = pool_slot.matrix_a_stage_buffer;
        id<MTLBuffer> matrix_b_buffer = pool_slot.matrix_b_stage_buffer;
        id<MTLBuffer> e_l_stage_buffer = pool_slot.e_l_buffer;
        id<MTLBuffer> e_r_stage_buffer = pool_slot.e_r_buffer;
        id<MTLBuffer> f_l_stage_buffer = pool_slot.f_l_buffer;
        id<MTLBuffer> f_r_stage_buffer = pool_slot.f_r_buffer;
        id<MTLBuffer> a_prime_buffer = pool_slot.a_prime_buffer;
        id<MTLBuffer> b_prime_buffer = pool_slot.b_prime_buffer;
        id<MTLBuffer> c_prime_buffer = pool_slot.c_prime_buffer;
        id<MTLBuffer> tile_hash_buffer = pool_slot.tile_hash_buffer;
        const size_t seed_bytes = static_cast<size_t>(request.batch_size) * 2 * uint256::size();
        id<MTLBuffer> seed_stage_buffer = [context.device newBufferWithLength:seed_bytes
                                                                       options:MTLResourceStorageModeShared];
        if (seed_stage_buffer == nil) {
            result.error = "Failed to allocate Metal variable-base seed staging buffer";
            return result;
        }
        for (uint32_t i = 0; i < request.batch_size; ++i) {
            auto* seed_bytes_ptr = static_cast<unsigned char*>(seed_stage_buffer.contents);
            std::memcpy(seed_bytes_ptr + (static_cast<size_t>(i) * 2 * uint256::size()),
                        request.matrix_a_seeds[i].data(),
                        uint256::size());
            std::memcpy(seed_bytes_ptr + ((static_cast<size_t>(i) * 2 + 1) * uint256::size()),
                        request.matrix_b_seeds[i].data(),
                        uint256::size());
        }

        std::memcpy(params_buffer.contents, &params, sizeof(params));
        id<MTLCommandBuffer> command = CreatePerformanceCommandBuffer(pool_slot.queue);
        if (command == nil) {
            result.error = "Failed to create Metal variable-base command buffer";
            return result;
        }

        std::string encode_error;
        double encode_generate_base_us{0.0};
        double encode_build_perturbed_us{0.0};
        double encode_product_us{0.0};
        double encode_tile_hash_us{0.0};

        for (uint32_t i = 0; i < request.batch_size; ++i) {
            const size_t matrix_offset = static_cast<size_t>(i) * matrix_bytes;
            const size_t noise_offset = static_cast<size_t>(i) * noise_bytes;
            const size_t tile_hash_offset = static_cast<size_t>(i) * tile_hash_bytes;

            const auto encode_base_start = std::chrono::steady_clock::now();
            if (!EncodeGenerateBaseMatrix(command, context, params_buffer, seed_stage_buffer,
                                          static_cast<size_t>(i) * 2 * uint256::size(),
                                          matrix_a_buffer, matrix_offset, shape.matrix_words, encode_error) ||
                !EncodeGenerateBaseMatrix(command, context, params_buffer, seed_stage_buffer,
                                          (static_cast<size_t>(i) * 2 + 1) * uint256::size(),
                                          matrix_b_buffer, matrix_offset, shape.matrix_words, encode_error)) {
                result.error = encode_error;
                return result;
            }
            encode_generate_base_us += std::chrono::duration<double, std::micro>(
                                           std::chrono::steady_clock::now() - encode_base_start)
                                           .count();

            std::memcpy(static_cast<unsigned char*>(e_l_stage_buffer.contents) + noise_offset, request.noise_e_l[i], noise_bytes);
            std::memcpy(static_cast<unsigned char*>(e_r_stage_buffer.contents) + noise_offset, request.noise_e_r[i], noise_bytes);
            std::memcpy(static_cast<unsigned char*>(f_l_stage_buffer.contents) + noise_offset, request.noise_f_l[i], noise_bytes);
            std::memcpy(static_cast<unsigned char*>(f_r_stage_buffer.contents) + noise_offset, request.noise_f_r[i], noise_bytes);

            const std::array<BufferBinding, 9> build_bindings{{
                {params_buffer, 0},
                {matrix_a_buffer, static_cast<NSUInteger>(matrix_offset)},
                {matrix_b_buffer, static_cast<NSUInteger>(matrix_offset)},
                {e_l_stage_buffer, static_cast<NSUInteger>(noise_offset)},
                {e_r_stage_buffer, static_cast<NSUInteger>(noise_offset)},
                {f_l_stage_buffer, static_cast<NSUInteger>(noise_offset)},
                {f_r_stage_buffer, static_cast<NSUInteger>(noise_offset)},
                {a_prime_buffer, static_cast<NSUInteger>(matrix_offset)},
                {b_prime_buffer, static_cast<NSUInteger>(matrix_offset)},
            }};
            const auto encode_build_start = std::chrono::steady_clock::now();
            if (!EncodeComputeBindings(command,
                                       selection.build_perturbed,
                                       static_cast<NSUInteger>(shape.matrix_words),
                                       256,
                                       build_bindings,
                                       encode_error)) {
                result.error = encode_error;
                return result;
            }
            encode_build_perturbed_us += std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now() - encode_build_start)
                                             .count();

            if (!EncodeProductAndTileHashes(command,
                                            context,
                                            selection,
                                            params_buffer,
                                            BufferBinding{a_prime_buffer, static_cast<NSUInteger>(matrix_offset)},
                                            BufferBinding{b_prime_buffer, static_cast<NSUInteger>(matrix_offset)},
                                            BufferBinding{c_prime_buffer, static_cast<NSUInteger>(matrix_offset)},
                                            BufferBinding{tile_hash_buffer, static_cast<NSUInteger>(tile_hash_offset)},
                                            request.n,
                                            request.b,
                                            N,
                                            encode_product_us,
                                            encode_tile_hash_us,
                                            encode_error)) {
                result.error = encode_error;
                return result;
            }
        }

        const auto submit_wait_start = std::chrono::steady_clock::now();
        [command commit];
        [command waitUntilCompleted];
        const double submit_wait_us = std::chrono::duration<double, std::micro>(
                                          std::chrono::steady_clock::now() - submit_wait_start)
                                          .count();

        if (command.status != MTLCommandBufferStatusCompleted) {
            NSString* description = command.error != nil ? [command.error localizedDescription] : @"unknown Metal command failure";
            result.success = false;
            result.error = [description UTF8String];
            return result;
        }

        const auto finalize_start = std::chrono::steady_clock::now();
        result.digests.reserve(request.batch_size);
        const auto* tile_hash_bytes_ptr = static_cast<const unsigned char*>(tile_hash_buffer.contents);
        for (uint32_t i = 0; i < request.batch_size; ++i) {
            uint256 digest;
            std::string finalize_error;
            if (!FinalizeProductDigestFromTileHashBytes(
                    tile_hash_bytes_ptr == nullptr
                        ? nullptr
                        : tile_hash_bytes_ptr + static_cast<size_t>(i) * tile_hash_bytes,
                    N,
                    request.n,
                    request.b,
                    request.sigmas[i],
                    digest,
                    finalize_error)) {
                result.success = false;
                result.error = finalize_error;
                return result;
            }
            result.digests.push_back(digest);
        }
        const double cpu_finalize_us = std::chrono::duration<double, std::micro>(
                                           std::chrono::steady_clock::now() - finalize_start)
                                           .count();

        {
            std::lock_guard<std::mutex> lock(context.profiling_mutex);
            context.profiling_stats.available = true;
            context.profiling_stats.capture_supported = context.capture_supported;
            ++context.profiling_stats.samples;
            context.profiling_stats.last_encode_build_perturbed_us =
                encode_generate_base_us + encode_build_perturbed_us;
            context.profiling_stats.last_encode_fused_prefix_compress_us = encode_product_us;
            context.profiling_stats.last_encode_transcript_sha256_us = encode_tile_hash_us;
            context.profiling_stats.last_submit_wait_us = submit_wait_us;
            context.profiling_stats.last_gpu_execution_ms = submit_wait_us / 1000.0;
            context.profiling_stats.last_cpu_finalize_us = cpu_finalize_us;
            context.profiling_stats.last_zero_copy_inputs = false;
            context.profiling_stats.last_async_submission = false;
            context.profiling_stats.reason = "profiling_samples_ready_variable_base_batch";
        }

        result.success = true;
        return result;
    }
}

} // namespace qtc::metal
