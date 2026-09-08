// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QTC_SHIELDED_SMILE2_PARAMS_H
#define QTC_SHIELDED_SMILE2_PARAMS_H

#include <cstddef>
#include <cstdint>
#include <array>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace smile2 {

// Ring R_q = Z_q[X] / (X^128 + 1)
static constexpr size_t POLY_DEGREE = 128;           // d = 128

// Prime q = 4294966337 = 2^32 - 959
// q ≡ 1 (mod 64), q ≡ 65 (mod 128)
// X^128+1 splits into exactly 32 irreducible degree-4 factors mod q.
// Primitive root g = 3.
static constexpr int64_t Q = 4294966337LL;

// NTT slot parameters
static constexpr size_t NUM_NTT_SLOTS = 32;           // l = 32
static constexpr size_t SLOT_DEGREE = 4;              // d/l = 4

// Primitive 64th root of unity: ζ = g^((q-1)/64) mod q
static constexpr int64_t ZETA = 3463736836LL;

// Slot roots: ζ^(2j+1) for j = 0..31
// X^128+1 = ∏_{j=0}^{31} (X^4 - SLOT_ROOTS[j])  (mod q)
static constexpr std::array<int64_t, NUM_NTT_SLOTS> SLOT_ROOTS = {
    3463736836LL, // ζ^1
    1624289040LL, // ζ^3
    1783970969LL, // ζ^5
    3137426997LL, // ζ^7
    3896370500LL, // ζ^9
    3624752522LL, // ζ^11
    3464556344LL, // ζ^13
    1669300415LL, // ζ^15
    1639633990LL, // ζ^17
    2533428898LL, // ζ^19
    2579408587LL, // ζ^21
    3875052898LL, // ζ^23
    2028732914LL, // ζ^25
    1655305793LL, // ζ^27
    655737010LL,  // ζ^29
    3259370049LL, // ζ^31
    831229501LL,  // ζ^33
    2670677297LL, // ζ^35
    2510995368LL, // ζ^37
    1157539340LL, // ζ^39
    398595837LL,  // ζ^41
    670213815LL,  // ζ^43
    830409993LL,  // ζ^45
    2625665922LL, // ζ^47
    2655332347LL, // ζ^49
    1761537439LL, // ζ^51
    1715557750LL, // ζ^53
    419913439LL,  // ζ^55
    2266233423LL, // ζ^57
    2639660544LL, // ζ^59
    3639229327LL, // ζ^61
    1035596288LL  // ζ^63
};

// 1/l mod q = 1/32 mod q
static constexpr int64_t INV_NUM_SLOTS = 4160748639LL;

// Module ranks for M-SIS / M-LWE security (128-bit classical)
static constexpr size_t MSIS_RANK = 10;               // α = 10
static constexpr size_t MLWE_RANK = 10;               // β = 10
static constexpr size_t BDLOP_RAND_DIM_BASE = 20;     // α + β

// Key dimensions
static constexpr size_t KEY_ROWS = 5;                  // k = 5
static constexpr size_t KEY_COLS = 4;                  // ℓ = 4

// Secret distribution: ternary {-1, 0, 1}
static constexpr size_t SECRET_ETA = 1;

// Anonymity set parameters
static constexpr size_t ANON_EXP = 15;
static constexpr size_t ANON_SET_SIZE = 1 << ANON_EXP; // 32768
static constexpr size_t RECURSION_LEVELS = 3;           // m = ceil(15/5)

// Masking standard deviations (bimodal technique)
// With monomial challenge c = ±X^k, ||c·r||_∞ ≤ 1 for ternary r,
// so σ only needs to hide a ±1 shift. σ = 55 gives negligible leakage.
static constexpr int64_t SIGMA_MASK = 55;        // σ for main z vector
static constexpr int64_t SIGMA_KEY = 31;          // σ_0 for key proof z_0

// Serialization bit widths for centered z coefficients
// z centered in [-SIGMA_MASK-1, SIGMA_MASK+1] = [-56, 56] → 7 bits
// z0 centered in [-SIGMA_KEY-1, SIGMA_KEY+1] = [-32, 32] → 7 bits
static constexpr size_t Z_COEFF_BITS = 7;
static constexpr size_t Z0_COEFF_BITS = 7;

// Compression: drop D low-order bits from t0 polynomials (Dilithium technique)
static constexpr size_t COMPRESS_D = 12;

// Transaction limits
static constexpr size_t MAX_CT_INPUTS = 16;
static constexpr size_t MAX_CT_OUTPUTS = 16;

// Modular arithmetic helpers
inline int64_t mod_q(int64_t x) {
    const int64_t r = x % Q;
    return r + (Q & -static_cast<int64_t>(r < 0));
}

// Barrett reduction constant: R = floor(2^64 / Q)
// Q = 4294966337 = 2^32 - 959, so R = floor(2^64 / (2^32 - 959)) = 4294968255
// Verify: R*Q = (2^32 + 959)*(2^32 - 959) = 2^64 - 959^2 < 2^64 ✓
//         (R+1)*Q > 2^64 ✓
static constexpr uint64_t BARRETT_R = 4294968255ULL;
static constexpr uint64_t QU = static_cast<uint64_t>(Q);

// Barrett reduction with exact 128-bit intermediate arithmetic.
//
// Inputs are first normalized into [0, Q) so negative values never alias into
// unrelated large uint64_t values. The product must stay 128-bit all the way
// through the reduction because q^2 exceeds 2^64 for this modulus.
inline int64_t mul_mod_q(int64_t a, int64_t b) {
    a = mod_q(a);
    b = mod_q(b);
#if defined(_MSC_VER) && !defined(__clang__)
    const uint64_t product = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
    uint64_t high = 0;
    (void)_umul128(product, BARRETT_R, &high);
    const uint64_t q_hat = high;
    uint64_t result = product - (q_hat * QU);
    // For x < q^2 with mu = floor(2^64 / q), q_hat is either floor(x / q) or
    // floor(x / q) - 1, so the provisional residue is always in [0, 2q).
    result -= QU & static_cast<uint64_t>(-static_cast<int64_t>(result >= QU));
    return static_cast<int64_t>(result);
#else
    const __uint128_t product =
        static_cast<__uint128_t>(static_cast<uint64_t>(a)) *
        static_cast<__uint128_t>(static_cast<uint64_t>(b));
    const uint64_t q_hat = static_cast<uint64_t>((product * BARRETT_R) >> 64);
    uint64_t result = static_cast<uint64_t>(product - (static_cast<__uint128_t>(q_hat) * QU));
    // For x < q^2 with mu = floor(2^64 / q), q_hat is either floor(x / q) or
    // floor(x / q) - 1, so the provisional residue is always in [0, 2q).
    result -= QU & static_cast<uint64_t>(-static_cast<int64_t>(result >= QU));
    return static_cast<int64_t>(result);
#endif
}

// M2 audit fix: reduce inputs first so the functions handle arbitrary
// int64_t values, not only those already in [0, Q).
inline int64_t add_mod_q(int64_t a, int64_t b) {
    return mod_q(mod_q(a) + mod_q(b));
}

inline int64_t sub_mod_q(int64_t a, int64_t b) {
    return mod_q(mod_q(a) - mod_q(b));
}

inline int64_t neg_mod_q(int64_t a) {
    const int64_t reduced = mod_q(a);
    return (Q - reduced) & -static_cast<int64_t>(reduced != 0);
}

// Modular inverse via Fermat's little theorem: a^{-1} = a^{q-2} mod q
inline int64_t inv_mod_q(int64_t a) {
    int64_t result = 1;
    int64_t base = mod_q(a);
    int64_t exp = Q - 2;
    while (exp > 0) {
        if (exp & 1) result = mul_mod_q(result, base);
        base = mul_mod_q(base, base);
        exp >>= 1;
    }
    return result;
}

} // namespace smile2

#endif // QTC_SHIELDED_SMILE2_PARAMS_H
