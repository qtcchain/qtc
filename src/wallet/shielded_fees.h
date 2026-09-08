// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef QTC_WALLET_SHIELDED_FEES_H
#define QTC_WALLET_SHIELDED_FEES_H

#include <consensus/amount.h>

#include <cstddef>

namespace wallet {

[[nodiscard]] CAmount BucketShieldedAutoFee(CAmount fee);

[[nodiscard]] size_t EstimateDirectShieldedSendVirtualSize(size_t spend_count,
                                                           size_t shielded_output_count,
                                                           size_t transparent_output_bytes = 0);

} // namespace wallet

#endif // QTC_WALLET_SHIELDED_FEES_H
