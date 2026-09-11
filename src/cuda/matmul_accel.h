// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_CUDA_MATMUL_ACCEL_H
#define BITCOIN_CUDA_MATMUL_ACCEL_H

#include <cuda/cuda_context.h>
#include <matmul/field.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

namespace qtc::cuda {

struct MatMulGeneratedInputsDevice;

// CUDA backend for the QTC O5 product-committed digest v4 (see
// matmul/transcript.cpp). Every request computes the FULL perturbed product
// C' = A'B' over GF(2^31-1) on the device and hashes each b x b tile of C'
// (row-major LE32 elements, single SHA-256) into a 32-byte tile hash. The
// N^2 = (n/b)^2 tile hashes per request are returned to the host in row-major
// tile order; the caller finishes with
// matmul::transcript::ComputeProductCommittedDigestFromTileHashes().
//
// The v3 linear compression (compress vector, compressed words, factored RHS)
// is gone: it was the H4 mining shortcut and has no place in a v4 kernel. The
// legacy TRANSCRIPT scheme is not accelerated (CPU only).

struct MatMulAccelerationProbe {
    bool available{false};
    std::string reason;
    std::string device_name;
    uint32_t compute_capability_major{0};
    uint32_t compute_capability_minor{0};
    uint64_t global_memory_bytes{0};
    uint32_t multiprocessor_count{0};
    uint32_t driver_api_version{0};
    uint32_t runtime_version{0};
};

struct MatMulBufferPoolStats {
    bool available{false};
    bool initialized{false};
    uint64_t allocation_events{0};
    uint64_t reuse_events{0};
    uint64_t wait_events{0};
    uint64_t completed_submissions{0};
    uint64_t device_capacity_bytes{0};
    uint64_t active_device_capacity_bytes{0};
    uint64_t max_slot_device_capacity_bytes{0};
    uint32_t slot_count{0};
    uint32_t active_slots{0};
    uint32_t high_water_slots{0};
    uint32_t slots_with_device_buffers{0};
    uint32_t inflight_submissions{0};
    uint32_t peak_inflight_submissions{0};
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    std::string reason;
};

struct MatMulDispatchConfig {
    bool available{false};
    uint32_t build_perturbed_threads{0};
    uint32_t gemm_tile_dim{0};
    uint32_t gemm_threads{0};
    uint32_t tile_hash_threads{0};
    uint32_t max_supported_block_size{0};
    bool nonblocking_streams{false};
    std::string reason;
};

struct MatMulKernelProfile {
    bool available{false};
    bool low_rank_perturbation_kernel{false};
    bool tiled_product_digest_v4{false};
    bool pinned_host_staging{false};
    bool base_matrix_cache{false};
    bool shared_buffer_pool{false};
    bool nonblocking_streams{false};
    bool device_prepared_inputs_supported{false};
    bool device_prepared_inputs_default{false};
    bool device_prepared_inputs_enabled{false};
    std::string execution_model;
    std::string staging_strategy;
    std::string device_prepared_inputs_policy;
    std::string reason;
};

struct MatMulProfilingStats {
    bool available{false};
    uint64_t samples{0};
    uint32_t last_n{0};
    uint32_t last_b{0};
    uint32_t last_r{0};
    uint32_t last_batch_size{0};
    double last_host_stage_us{0.0};
    double last_submit_h2d_us{0.0};
    double last_submit_d2d_us{0.0};
    double last_stream_wait_event_us{0.0};
    double last_launch_build_perturbed_us{0.0};
    double last_launch_finalize_us{0.0};
    double last_submit_d2h_us{0.0};
    double last_stream_sync_us{0.0};
    double last_total_wall_ms{0.0};
    bool last_used_low_rank_path{false};
    bool last_used_device_prepared_inputs{false};
    bool last_used_pinned_host_staging{false};
    bool last_base_matrix_cache_hit{false};
    double last_gpu_build_us{0.0};
    double last_gpu_gemm_us{0.0};
    double last_gpu_tile_hash_us{0.0};
    double last_gpu_copy_us{0.0};
    std::string last_mode;
    std::string reason;
};

/** Result of any tile-hash request: tile_hashes holds batch_size *
 *  tiles_per_request entries; request i occupies
 *  [i * tiles_per_request, (i + 1) * tiles_per_request) in row-major tile order
 *  (tile (ti, tj) at ti * (n/b) + tj). */
struct MatMulProductTileHashBatchResult {
    bool available{false};
    bool success{false};
    uint32_t tiles_per_request{0};
    /** Filled when the request supplied sigmas: the finished v4 digest per
     *  request (root + outer SHA256d computed on the device). */
    std::vector<uint256> digests;
    /** Filled when the request supplied no sigmas, or asked for them explicitly
     *  (return_tile_hashes). Device->host traffic is the expensive part of a
     *  request on virtualised GPUs, so the mining path only fetches digests. */
    std::vector<uint256> tile_hashes;
    std::string error;
};

// Common tail of every request: optional per-request sigmas (uint256 internal
// byte order, batch_size entries) enable the device-side digest finish;
// return_tile_hashes additionally copies the per-tile hashes back.
#define QTC_CUDA_PRODUCT_REQUEST_FINISH_FIELDS \
    const uint256* sigmas{nullptr};           \
    bool return_tile_hashes{false};

/** Already-perturbed A'/B' supplied by the host (tests/tooling): no oracle, no
 *  noise, just the GEMM + tile hashing. */
struct MatMulProductTileHashBatchRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t batch_size{0};
    const matmul::field::Element* const* matrix_a_perturbed{nullptr};
    const matmul::field::Element* const* matrix_b_perturbed{nullptr};
    QTC_CUDA_PRODUCT_REQUEST_FINISH_FIELDS
};

/** Shared base matrices A/B (host, cached on the device by seed key) plus
 *  host-resident low-rank noise per request. */
struct MatMulLowRankProductBatchRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint32_t batch_size{0};
    const matmul::field::Element* matrix_a{nullptr};
    const matmul::field::Element* matrix_b{nullptr};
    const uint256* matrix_a_cache_key{nullptr};
    const uint256* matrix_b_cache_key{nullptr};
    const matmul::field::Element* const* noise_e_l{nullptr};
    const matmul::field::Element* const* noise_e_r{nullptr};
    const matmul::field::Element* const* noise_f_l{nullptr};
    const matmul::field::Element* const* noise_f_r{nullptr};
    QTC_CUDA_PRODUCT_REQUEST_FINISH_FIELDS
};

/** Shared base matrices A/B plus device-generated noise per request. */
struct MatMulLowRankProductDeviceBatchRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint32_t batch_size{0};
    const matmul::field::Element* matrix_a{nullptr};
    const matmul::field::Element* matrix_b{nullptr};
    const uint256* matrix_a_cache_key{nullptr};
    const uint256* matrix_b_cache_key{nullptr};
    const MatMulGeneratedInputsDevice* const* generated_inputs{nullptr};
    QTC_CUDA_PRODUCT_REQUEST_FINISH_FIELDS
};

/** Variable-base mining path: per-request base matrices are regenerated on
 *  the device from (seed_a, seed_b) with oracle v2; the noise comes either from
 *  device-generated inputs (generated_inputs != nullptr) or from host-resident
 *  noise pointers (all four noise_* arrays non-null). Exactly one of the two
 *  representations must be supplied for the whole batch. */
struct MatMulLowRankVariableBaseProductBatchRequest {
    uint32_t n{0};
    uint32_t b{0};
    uint32_t r{0};
    uint32_t batch_size{0};
    const uint256* matrix_a_seeds{nullptr};
    const uint256* matrix_b_seeds{nullptr};
    const MatMulGeneratedInputsDevice* const* generated_inputs{nullptr};
    const matmul::field::Element* const* noise_e_l{nullptr};
    const matmul::field::Element* const* noise_e_r{nullptr};
    const matmul::field::Element* const* noise_f_l{nullptr};
    const matmul::field::Element* const* noise_f_r{nullptr};
    QTC_CUDA_PRODUCT_REQUEST_FINISH_FIELDS
};

MatMulAccelerationProbe ProbeMatMulDigestAcceleration();
MatMulBufferPoolStats ProbeMatMulBufferPool();
MatMulDispatchConfig ProbeMatMulDispatchConfig();
MatMulKernelProfile ProbeMatMulKernelProfile();
MatMulProfilingStats ProbeMatMulProfilingStats();

MatMulProductTileHashBatchResult ComputeProductTileHashesBatch(
    const MatMulProductTileHashBatchRequest& request);

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatch(
    const MatMulLowRankProductBatchRequest& request);
MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatchOnDevice(
    const MatMulLowRankProductBatchRequest& request,
    int device_index);
MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankBatchMultiDevice(
    const MatMulLowRankProductBatchRequest& request);

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatch(
    const MatMulLowRankProductDeviceBatchRequest& request);
MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatchOnDevice(
    const MatMulLowRankProductDeviceBatchRequest& request,
    int device_index);
MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankDeviceBatchMultiDevice(
    const MatMulLowRankProductDeviceBatchRequest& request);

MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatch(
    const MatMulLowRankVariableBaseProductBatchRequest& request);
MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatchOnDevice(
    const MatMulLowRankVariableBaseProductBatchRequest& request,
    int device_index);
MatMulProductTileHashBatchResult ComputeProductTileHashesLowRankVariableBaseBatchMultiDevice(
    const MatMulLowRankVariableBaseProductBatchRequest& request);

} // namespace qtc::cuda

#endif // BITCOIN_CUDA_MATMUL_ACCEL_H
