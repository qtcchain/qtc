// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QTC_SHIELDED_SPEND_AUTH_H
#define QTC_SHIELDED_SPEND_AUTH_H

#include <consensus/params.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>

namespace shielded {

/** Compute the deterministic spend-authorization signature hash for one shielded input. */
[[nodiscard]] uint256 ComputeShieldedSpendAuthSigHash(const CTransaction& tx, size_t input_index);
[[nodiscard]] uint256 ComputeShieldedSpendAuthSigHash(const CMutableTransaction& tx, size_t input_index);
[[nodiscard]] uint256 ComputeShieldedSpendAuthSigHash(const CTransaction& tx,
                                                      size_t input_index,
                                                      const Consensus::Params& consensus,
                                                      int32_t validation_height);
[[nodiscard]] uint256 ComputeShieldedSpendAuthSigHash(const CMutableTransaction& tx,
                                                      size_t input_index,
                                                      const Consensus::Params& consensus,
                                                      int32_t validation_height);

} // namespace shielded

#endif // QTC_SHIELDED_SPEND_AUTH_H
