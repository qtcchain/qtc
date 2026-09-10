// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QTC_MATMUL_FIELD_H
#define QTC_MATMUL_FIELD_H

#include <cstdint>
#include <string>

class uint256;

namespace matmul::field {

using Element = uint32_t;
constexpr Element MODULUS = 0x7FFFFFFFU;

struct DotKernelInfo {
    bool neon_compiled{false};
    std::string reason;
};

Element add(Element a, Element b);
Element sub(Element a, Element b);
Element mul(Element a, Element b);
Element inv(Element a);
Element neg(Element a);
Element from_uint32(uint32_t x);
/** Oracle v2 (QTC O5): one SHA-256 yields eight 31-bit lanes.
 *  block = index >> 3, lane = index & 7;
 *  preimage = seed_canonical(32) || LE32(block) [|| LE32(retry) when retry > 0];
 *  candidate = ReadLE32(SHA256(preimage) + 4*lane) & MODULUS, rejected (retry++) when == MODULUS.
 *  Index 0 is therefore identical to the v1 oracle (TV1 unchanged). */
Element from_oracle(const uint256& seed, uint32_t index);
/** All eight lanes of one oracle block, with per-lane rejection handling identical to from_oracle. */
void from_oracle_block(const uint256& seed, uint32_t block, Element out[8]);
/** out[k] = from_oracle(seed, start_index + k) for k in [0, count), computed one SHA-256 per 8 lanes. */
void fill_from_oracle(const uint256& seed, uint32_t start_index, uint32_t count, Element* out);
Element dot(const Element* a, const Element* b, uint32_t len);
DotKernelInfo ProbeDotKernel();

} // namespace matmul::field

#endif // QTC_MATMUL_FIELD_H
