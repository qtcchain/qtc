// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_METAL_MATMUL_ACCEL_H
#define BITCOIN_METAL_MATMUL_ACCEL_H

#include <matmul/field.h>
#include <uint256.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Metal MatMul proof-of-work backend.
//
// Consensus (QTC O5 / v0.0.2): oracle v2 base matrices and the product-committed
// digest v4. The device computes A' = A + E_L*E_R, B' = B + F_L*F_R, the full
// product C' = A'*B' over GF(2^31-1) and the N*N per-tile SHA-256 hashes of C'
// (matmul::transcript::HashProductTile). The host finishes the digest with
// matmul::transcript::ComputeProductCommittedDigestFromTileHashes (root over
// the tile hashes + tagged SHA256d), so the returned digest is consensus-final.
//
// Only MatMulDigestMode::PRODUCT_COMMITTED is served on the device. The legacy
// TRANSCRIPT scheme (pre-activation regtest heights only) is rejected with a
// clean, non-submitted error so callers fall back to the CPU reference.

namespace qtc::metal {

struct MatMulAccelerationProbe {
    bool available{false};
    std::string reason;
};

struct MatMulDeviceInfo {
    bool available{false};
    std::string device_name;
    uint32_t gpu_core_count{0};
    std::string gpu_core_count_source;
    std::string reason;
};

struct MatMulBaseMatricesRequest {
    uint32_t n{0};
    const matmul::field::Element* matrix_a{nullptr};
    const matmul::field::Element* matrix_b{nullptr};
};

struct MatMulBaseMatricesResult {
    bool available{false};
    bool success{false};
    std::string error;
};

struct MatMulGeneratedBaseMatrixResult {
    bool available{false};
    bool success{false};
    std::vector<matmul::field::Element> matrix;
    std::string error;
};

/** Test/diagnostic request: run the whole variable-base product pipeline for one
 *  attempt and return every intermediate (oracle-v2 base matrices, perturbed
 *  operands, the full C' and its v4 tile hashes). */
struct MatMulVariableBaseProductRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint256 matrix_a_seed;
    uint256 matrix_b_seed;
    const matmul::field::Element* noise_e_l{nullptr};
    const matmul::field::Element* noise_e_r{nullptr};
    const matmul::field::Element* noise_f_l{nullptr};
    const matmul::field::Element* noise_f_r{nullptr};
};

struct MatMulVariableBaseProductResult {
    bool available{false};
    bool success{false};
    std::vector<matmul::field::Element> matrix_a;
    std::vector<matmul::field::Element> matrix_b;
    std::vector<matmul::field::Element> a_prime;
    std::vector<matmul::field::Element> b_prime;
    std::vector<matmul::field::Element> c_prime;
    /** Row-major tile order, (n/b)^2 entries; raw SHA-256 bytes as uint256. */
    std::vector<uint256> tile_hashes;
    std::string error;
};

struct MatMulBufferPoolStats {
    bool available{false};
    bool initialized{false};
    uint64_t allocation_events{0};
    uint64_t reuse_events{0};
    uint64_t wait_events{0};
    uint64_t completed_submissions{0};
    uint32_t slot_count{0};
    uint32_t active_slots{0};
    uint32_t high_water_slots{0};
    uint32_t inflight_submissions{0};
    uint32_t peak_inflight_submissions{0};
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    std::string reason;
};

/** Threadgroup sizes selected for the digest pipeline stages. The field names
 *  predate the v4 port (they are reported by qtc-matmul-backend-info):
 *    build_prefix_threads    -> product GEMM (build_product) threadgroup size
 *    compress_prefix_threads -> per-tile SHA-256 (hash_product_tiles) size */
struct MatMulDispatchConfig {
    bool available{false};
    uint32_t build_perturbed_threads{0};
    uint32_t build_prefix_threads{0};
    uint32_t compress_prefix_threads{0};
    std::string reason;
};

/** Kernel availability profile. Field names predate the v4 port and are kept
 *  for tooling compatibility; their v4 meaning is:
 *    tiled_build_prefix     -> 16x16 threadgroup-memory product GEMM available
 *    fused_prefix_compress  -> threadgroup-per-tile product GEMM available
 *    gpu_transcript_hash    -> per-tile SHA-256 kernel available
 *    uses_prefix_buffer     -> always false (no N*n*n prefix buffer in v4) */
struct MatMulKernelProfile {
    bool available{false};
    bool tiled_build_prefix{false};
    bool fused_prefix_compress{false};
    bool gpu_transcript_hash{false};
    bool function_constant_specialization{false};
    bool cooperative_tensor_prepared{false};
    bool cooperative_tensor_active{false};
    bool uses_prefix_buffer{false};
    uint32_t specialized_shape_count{0};
    uint32_t build_prefix_threadgroup_width{0};
    uint32_t build_prefix_threadgroup_height{0};
    uint32_t fused_prefix_threadgroup_threads{0};
    std::string specialization_reason;
    std::string cooperative_tensor_reason;
    std::string library_source;
    std::string reason;
};

/** Last-sample encode/submit timings. Legacy field names:
 *    last_encode_fused_prefix_compress_us -> product GEMM encode time
 *    last_encode_transcript_sha256_us     -> tile-hash kernel encode time
 *    last_cpu_finalize_us                 -> host root + outer SHA256d time */
struct MatMulProfilingStats {
    bool available{false};
    bool capture_supported{false};
    uint64_t samples{0};
    double last_encode_build_perturbed_us{0.0};
    double last_encode_fused_prefix_compress_us{0.0};
    double last_encode_transcript_sha256_us{0.0};
    double last_submit_wait_us{0.0};
    double last_gpu_execution_ms{0.0};
    double last_cpu_finalize_us{0.0};
    bool last_zero_copy_inputs{false};
    bool last_async_submission{false};
    std::string reason;
};

enum class MatMulDigestMode : uint8_t {
    TRANSCRIPT,        //!< legacy v3 transcript digest: NOT served on Metal (clean CPU fallback)
    PRODUCT_COMMITTED, //!< product digest v4 (consensus)
};

struct MatMulDigestRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    MatMulDigestMode digest_mode{MatMulDigestMode::PRODUCT_COMMITTED};
    uint256 sigma;

    const matmul::field::Element* matrix_a{nullptr};
    const matmul::field::Element* matrix_b{nullptr};
    bool use_uploaded_base_matrices{false};

    const matmul::field::Element* noise_e_l{nullptr};
    const matmul::field::Element* noise_e_r{nullptr};
    const matmul::field::Element* noise_f_l{nullptr};
    const matmul::field::Element* noise_f_r{nullptr};

    /** Ignored. The v3 compression vector is not part of the v4 digest; the
     *  field is retained only so existing call sites keep compiling. */
    const matmul::field::Element* compress_vec{nullptr};
};

struct MatMulDigestResult {
    bool available{false};
    bool success{false};
    /** Consensus-final product digest v4 (host-finished from device tile hashes). */
    uint256 digest;
    std::string error;
};

struct MatMulDigestSubmission {
    bool available{false};
    bool submitted{false};
    std::string error;
    std::shared_ptr<void> opaque;
};

struct MatMulDigestBatchRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint32_t batch_size{0};
    MatMulDigestMode digest_mode{MatMulDigestMode::PRODUCT_COMMITTED};
    const uint256* sigmas{nullptr};

    const matmul::field::Element* matrix_a{nullptr};
    const matmul::field::Element* matrix_b{nullptr};
    bool use_uploaded_base_matrices{false};

    const matmul::field::Element* const* noise_e_l{nullptr};
    const matmul::field::Element* const* noise_e_r{nullptr};
    const matmul::field::Element* const* noise_f_l{nullptr};
    const matmul::field::Element* const* noise_f_r{nullptr};

    /** Ignored (see MatMulDigestRequest::compress_vec). */
    const matmul::field::Element* const* compress_vec{nullptr};
};

struct MatMulDigestBatchResult {
    bool available{false};
    bool success{false};
    std::vector<uint256> digests;
    std::string error;
};

struct MatMulDigestBatchSubmission {
    bool available{false};
    bool submitted{false};
    std::string error;
    std::shared_ptr<void> opaque;
};

struct MatMulVariableBaseDigestBatchRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint32_t batch_size{0};
    MatMulDigestMode digest_mode{MatMulDigestMode::PRODUCT_COMMITTED};
    const uint256* sigmas{nullptr};
    const uint256* matrix_a_seeds{nullptr};
    const uint256* matrix_b_seeds{nullptr};

    const matmul::field::Element* const* noise_e_l{nullptr};
    const matmul::field::Element* const* noise_e_r{nullptr};
    const matmul::field::Element* const* noise_f_l{nullptr};
    const matmul::field::Element* const* noise_f_r{nullptr};

    /** Ignored (see MatMulDigestRequest::compress_vec). */
    const matmul::field::Element* const* compress_vec{nullptr};
};

MatMulAccelerationProbe ProbeMatMulDigestAcceleration();
MatMulDeviceInfo ProbeMatMulDeviceInfo();
MatMulBaseMatricesResult UploadBaseMatrices(const MatMulBaseMatricesRequest& request);
/** Oracle-v2 base matrix generated on the device; equals matmul::FromSeed(seed, n). */
MatMulGeneratedBaseMatrixResult GenerateBaseMatrixFromSeedForTesting(uint32_t n, const uint256& seed);
MatMulVariableBaseProductResult GenerateVariableBaseProductForTesting(
    const MatMulVariableBaseProductRequest& request);
MatMulBufferPoolStats ProbeMatMulBufferPool();
MatMulDispatchConfig ProbeMatMulDispatchConfig();
MatMulKernelProfile ProbeMatMulKernelProfile();
MatMulProfilingStats ProbeMatMulProfilingStats();
/** Function-constant specialization policy for the product GEMM. The second
 *  argument is unused since the v4 port (kept for source compatibility). */
bool ShouldUseFunctionConstantSpecializationPolicy(uint32_t n, bool legacy_unused = false);
MatMulDigestSubmission SubmitCanonicalTranscriptDigest(const MatMulDigestRequest& request);
bool IsCanonicalTranscriptDigestSubmissionReady(const MatMulDigestSubmission& submission);
MatMulDigestResult WaitForCanonicalTranscriptDigestSubmission(MatMulDigestSubmission&& submission);
MatMulDigestResult ComputeCanonicalTranscriptDigest(const MatMulDigestRequest& request);
MatMulDigestBatchSubmission SubmitCanonicalTranscriptDigestBatch(const MatMulDigestBatchRequest& request);
bool IsCanonicalTranscriptDigestBatchSubmissionReady(const MatMulDigestBatchSubmission& submission);
MatMulDigestBatchResult WaitForCanonicalTranscriptDigestBatchSubmission(MatMulDigestBatchSubmission&& submission);
MatMulDigestBatchResult ComputeCanonicalTranscriptDigestBatch(const MatMulDigestBatchRequest& request);
MatMulDigestBatchResult ComputeCanonicalTranscriptDigestVariableBaseBatch(
    const MatMulVariableBaseDigestBatchRequest& request);

} // namespace qtc::metal

#endif // BITCOIN_METAL_MATMUL_ACCEL_H
