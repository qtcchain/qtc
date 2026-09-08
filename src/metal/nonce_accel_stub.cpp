// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <metal/nonce_accel.h>

namespace qtc::metal {

NonceBatch GenerateNonceBatch(uint64_t /*start_nonce*/, uint32_t /*batch_size*/, uint64_t /*seed*/, uint64_t /*threshold*/)
{
    NonceBatch batch;
    batch.available = false;
    batch.error = "Metal acceleration is unavailable on this build";
    return batch;
}

} // namespace qtc::metal
