// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_METAL_ORACLE_ACCEL_H
#define BITCOIN_METAL_ORACLE_ACCEL_H

#include <matmul/field.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

namespace qtc::metal {

struct MatMulInputGenerationRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint256 sigma;
};

struct MatMulInputGenerationResult {
    bool available{false};
    bool success{false};
    /** Oracle-v2 noise operands generated on the device (bit-identical to
     *  matmul::noise::Generate). */
    std::vector<matmul::field::Element> noise_e_l;
    std::vector<matmul::field::Element> noise_e_r;
    std::vector<matmul::field::Element> noise_f_l;
    std::vector<matmul::field::Element> noise_f_r;
    /** Not consensus since product digest v4 and no longer generated on the
     *  device; filled on the host (matmul::transcript::DeriveCompressionVector)
     *  only so the solver's prepared-input shape check keeps passing until it
     *  drops the field. */
    std::vector<matmul::field::Element> compress_vec;
    std::string error;
};

struct MatMulInputGenerationProfile {
    bool available{false};
    bool pool_initialized{false};
    uint64_t samples{0};
    uint64_t allocation_events{0};
    uint64_t reuse_events{0};
    double last_encode_noise_us{0.0};
    /** Always 0 since v4: no compression-vector dispatch remains. */
    double last_encode_compress_us{0.0};
    double last_submit_wait_us{0.0};
    double last_gpu_generation_ms{0.0};
    std::string library_source;
    std::string reason;
};

struct MatMulOracleVectorResult {
    bool available{false};
    bool success{false};
    std::vector<matmul::field::Element> values;
    std::string error;
};

struct MatMulNonceSeedPreHashScanRequest {
    int32_t version{0};
    uint256 previous_block_hash;
    uint256 merkle_root;
    uint32_t time{0};
    uint32_t bits{0};
    uint64_t start_nonce{0};
    uint16_t matmul_dim{0};
    uint32_t block_height{0};
    uint32_t scan_count{0};
    uint256 pre_hash_target;
    uint32_t seed_version{2};
    int64_t parent_median_time_past{0};
};

struct MatMulNonceSeedPreHashScanResult {
    bool available{false};
    bool success{false};
    uint32_t scanned_count{0};
    std::vector<uint8_t> pass_flags;
    std::string error;
};

MatMulInputGenerationProfile ProbeMatMulInputGenerationProfile();
MatMulInputGenerationResult GenerateMatMulInputsGPU(const MatMulInputGenerationRequest& request);
/** values[i] = matmul::field::from_oracle(seed, i) for i in [0, count),
 *  computed on the device with the oracle-v2 kernel (test/diagnostic hook). */
MatMulOracleVectorResult GenerateOracleVectorGPUForTesting(const uint256& seed, uint32_t count);
MatMulNonceSeedPreHashScanResult ScanMatMulNonceSeedPreHashGPU(
    const MatMulNonceSeedPreHashScanRequest& request);

} // namespace qtc::metal

#endif // BITCOIN_METAL_ORACLE_ACCEL_H
