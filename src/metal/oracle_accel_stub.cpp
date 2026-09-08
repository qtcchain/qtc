// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <metal/oracle_accel.h>

namespace qtc::metal {

MatMulInputGenerationProfile ProbeMatMulInputGenerationProfile()
{
    MatMulInputGenerationProfile profile;
    profile.available = false;
    profile.pool_initialized = false;
    profile.library_source = "unavailable";
    profile.reason = "Metal oracle acceleration is unavailable on this build";
    return profile;
}

MatMulInputGenerationResult GenerateMatMulInputsGPU(const MatMulInputGenerationRequest&)
{
    MatMulInputGenerationResult result;
    result.available = false;
    result.success = false;
    result.error = "Metal oracle acceleration is unavailable on this build";
    return result;
}

MatMulNonceSeedPreHashScanResult ScanMatMulNonceSeedPreHashGPU(const MatMulNonceSeedPreHashScanRequest&)
{
    MatMulNonceSeedPreHashScanResult result;
    result.available = false;
    result.success = false;
    result.error = "Metal nonce-seed pre-hash scan is unavailable on this build";
    return result;
}

} // namespace qtc::metal
