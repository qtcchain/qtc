// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <matmul/accelerated_solver.h>

#include <cuda/matmul_accel.h>
#include <cuda/oracle_accel.h>
#include <matmul/matmul_pow.h>
#include <matmul/noise.h>
#include <matmul/transcript.h>
#include <metal/oracle_accel.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

uint256 ParseUint256(std::string_view hex)
{
    const auto parsed = uint256::FromHex(hex);
    BOOST_REQUIRE(parsed.has_value());
    return *parsed;
}

CBlockHeader MakeCandidateHeader()
{
    CBlockHeader header;
    header.nVersion = 2;
    header.hashPrevBlock = ParseUint256("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    header.hashMerkleRoot = ParseUint256("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100");
    header.nTime = 1'700'000'000U;
    header.nBits = 0x207fffffU;
    header.nNonce64 = 42;
    header.nNonce = static_cast<uint32_t>(header.nNonce64);
    header.matmul_dim = 8;
    header.seed_a = ParseUint256("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    header.seed_b = ParseUint256("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");
    header.matmul_digest.SetNull();
    return header;
}

CBlockHeader MakeCandidateHeaderWithDim(uint32_t dim)
{
    CBlockHeader header = MakeCandidateHeader();
    header.matmul_dim = dim;
    return header;
}

CBlockHeader MakeStrictRegtestWarningReproHeader()
{
    CBlockHeader header;
    header.nVersion = 0x20000000;
    header.hashPrevBlock = ParseUint256("a3432bb1ebb8f1a98f5e562008f5570e426b94adc0759f3d9775ab9045918b98");
    header.hashMerkleRoot = ParseUint256("03f67b1aa858ac986a405770f91757b59eaf486823121589dd427a4f53f7e2f9");
    header.nTime = 1776143281U;
    header.nBits = 0x201a6e0fU;
    header.nNonce64 = 5;
    header.nNonce = static_cast<uint32_t>(header.nNonce64);
    header.matmul_dim = 64;
    header.seed_a = ParseUint256("1c11f95cd6c54e39670afeb96dd669a0db35c91319d5ba1776b087566783eac0");
    header.seed_b = ParseUint256("94e1b272422751e260b954ad9c7ba12598c76342855c90cb7030daa235f8b73f");
    header.matmul_digest.SetNull();
    return header;
}

uint256 ComputeReferenceProductDigest(const CBlockHeader& header,
                                      const matmul::Matrix& A,
                                      const matmul::Matrix& B,
                                      uint32_t transcript_block_size,
                                      uint32_t noise_rank)
{
    const uint256 sigma = matmul::DeriveSigma(header);
    const auto np = matmul::noise::Generate(sigma, header.matmul_dim, noise_rank);
    const auto A_prime = A + (np.E_L * np.E_R);
    const auto B_prime = B + (np.F_L * np.F_R);
    return matmul::transcript::ComputeProductCommittedDigestFromPerturbed(
        A_prime,
        B_prime,
        transcript_block_size,
        sigma);
}

class ScopedGpuInputEnv
{
public:
    explicit ScopedGpuInputEnv(const char* value)
    {
#if defined(WIN32)
        _putenv_s("QTC_MATMUL_GPU_INPUTS", value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv("QTC_MATMUL_GPU_INPUTS", value, 1);
        } else {
            unsetenv("QTC_MATMUL_GPU_INPUTS");
        }
#endif
    }

    ~ScopedGpuInputEnv()
    {
#if defined(WIN32)
        _putenv_s("QTC_MATMUL_GPU_INPUTS", "");
#else
        unsetenv("QTC_MATMUL_GPU_INPUTS");
#endif
    }
};

class ScopedCudaDevicePreparedInputsEnv
{
public:
    explicit ScopedCudaDevicePreparedInputsEnv(const char* value)
    {
#if defined(WIN32)
        _putenv_s("QTC_MATMUL_CUDA_DEVICE_PREPARED_INPUTS", value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv("QTC_MATMUL_CUDA_DEVICE_PREPARED_INPUTS", value, 1);
        } else {
            unsetenv("QTC_MATMUL_CUDA_DEVICE_PREPARED_INPUTS");
        }
#endif
    }

    ~ScopedCudaDevicePreparedInputsEnv()
    {
#if defined(WIN32)
        _putenv_s("QTC_MATMUL_CUDA_DEVICE_PREPARED_INPUTS", "");
#else
        unsetenv("QTC_MATMUL_CUDA_DEVICE_PREPARED_INPUTS");
#endif
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(matmul_accelerated_solver_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(cpu_digest_matches_canonical_reference)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank);

    const uint256 sigma = matmul::DeriveSigma(header);
    const auto np = matmul::noise::Generate(sigma, header.matmul_dim, kNoiseRank);
    const auto A_prime = A + (np.E_L * np.E_R);
    const auto B_prime = B + (np.F_L * np.F_R);
    const auto canonical = matmul::transcript::CanonicalMatMul(A_prime, B_prime, kTranscriptBlockSize, sigma);

    BOOST_CHECK_EQUAL(cpu_digest, canonical.transcript_hash);
}

BOOST_AUTO_TEST_CASE(cpu_product_digest_matches_canonical_reference)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_CHECK_EQUAL(
        cpu_digest,
        ComputeReferenceProductDigest(header, A, B, kTranscriptBlockSize, kNoiseRank));
}

BOOST_AUTO_TEST_CASE(metal_digest_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank);

    const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::METAL);

    BOOST_CHECK(digest_result.ok);
    BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);

    if (digest_result.backend == matmul::backend::Kind::METAL) {
        BOOST_CHECK(digest_result.accelerated);
    } else {
        BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
    }
}

BOOST_AUTO_TEST_CASE(prepared_digest_inputs_match_direct_path)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputs(
        header,
        kTranscriptBlockSize,
        kNoiseRank);
    const auto prepared_cpu = matmul::accelerated::ComputeMatMulDigestPrepared(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared,
        matmul::backend::Kind::CPU);
    const auto direct_cpu = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CPU);

    BOOST_REQUIRE(prepared_cpu.ok);
    BOOST_REQUIRE(direct_cpu.ok);
    BOOST_CHECK_EQUAL(prepared_cpu.digest, direct_cpu.digest);
}

BOOST_AUTO_TEST_CASE(prepared_product_digest_inputs_match_direct_path)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputs(
        header,
        kTranscriptBlockSize,
        kNoiseRank);
    const auto prepared_cpu = matmul::accelerated::ComputeMatMulDigestPrepared(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared,
        matmul::backend::Kind::CPU,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    const auto direct_cpu = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CPU,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_REQUIRE(prepared_cpu.ok);
    BOOST_REQUIRE(direct_cpu.ok);
    BOOST_CHECK_EQUAL(prepared_cpu.digest, direct_cpu.digest);
}

BOOST_AUTO_TEST_CASE(prepared_batch_digest_matches_direct_cpu_sequence)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;
    constexpr uint32_t kBatchSize = 3;

    const CBlockHeader base_header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(base_header.seed_a, base_header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(base_header.seed_b, base_header.matmul_dim);

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = base_header;
        header.nNonce64 += i;
        header.nNonce = static_cast<uint32_t>(header.nNonce64);
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputs(
            header,
            kTranscriptBlockSize,
            kNoiseRank));
    }

    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
        headers,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared_inputs,
        matmul::backend::Kind::CPU);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        const auto single = matmul::accelerated::ComputeMatMulDigestPrepared(
            headers[i],
            A,
            B,
            kTranscriptBlockSize,
            kNoiseRank,
            prepared_inputs[i],
            matmul::backend::Kind::CPU);
        BOOST_REQUIRE(single.ok);
        BOOST_REQUIRE(batch[i].ok);
        BOOST_CHECK_EQUAL(batch[i].digest, single.digest);
        BOOST_CHECK_EQUAL(batch[i].backend, single.backend);
    }
}

BOOST_AUTO_TEST_CASE(prepared_batch_product_digest_matches_direct_cpu_sequence)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;
    constexpr uint32_t kBatchSize = 3;

    const CBlockHeader base_header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(base_header.seed_a, base_header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(base_header.seed_b, base_header.matmul_dim);

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = base_header;
        header.nNonce64 += i;
        header.nNonce = static_cast<uint32_t>(header.nNonce64);
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputs(
            header,
            kTranscriptBlockSize,
            kNoiseRank));
    }

    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
        headers,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared_inputs,
        matmul::backend::Kind::CPU,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        const auto single = matmul::accelerated::ComputeMatMulDigestPrepared(
            headers[i],
            A,
            B,
            kTranscriptBlockSize,
            kNoiseRank,
            prepared_inputs[i],
            matmul::backend::Kind::CPU,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
        BOOST_REQUIRE(single.ok);
        BOOST_REQUIRE(batch[i].ok);
        BOOST_CHECK_EQUAL(batch[i].digest, single.digest);
        BOOST_CHECK_EQUAL(batch[i].backend, single.backend);
    }
}

BOOST_AUTO_TEST_CASE(cuda_prepared_batch_digest_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;
    constexpr uint32_t kBatchSize = 3;

    const CBlockHeader base_header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(base_header.seed_a, base_header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(base_header.seed_b, base_header.matmul_dim);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = base_header;
        header.nNonce64 += i;
        header.nNonce = static_cast<uint32_t>(header.nNonce64);
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputs(
            header,
            kTranscriptBlockSize,
            kNoiseRank));
    }

    matmul::accelerated::ResetMatMulBackendRuntimeStats();
    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
        headers,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared_inputs,
        matmul::backend::Kind::CUDA);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    const auto stats = matmul::accelerated::ProbeMatMulBackendRuntimeStats();
    BOOST_CHECK_EQUAL(stats.digest_requests, kBatchSize);
    BOOST_CHECK_EQUAL(stats.requested_cuda, kBatchSize);
    // TRANSCRIPT scheme: CPU-only on every GPU backend, reported as clean fallbacks.
    (void)cuda_capability;
    BOOST_CHECK_EQUAL(stats.cuda_successes, 0U);
    BOOST_CHECK_EQUAL(stats.cuda_fallbacks_to_cpu, kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        const auto single = matmul::accelerated::ComputeMatMulDigestPrepared(
            headers[i],
            A,
            B,
            kTranscriptBlockSize,
            kNoiseRank,
            prepared_inputs[i],
            matmul::backend::Kind::CPU);
        BOOST_REQUIRE(single.ok);
        BOOST_REQUIRE(batch[i].ok);
        BOOST_CHECK_EQUAL(batch[i].digest, single.digest);
        BOOST_CHECK_EQUAL(batch[i].backend, matmul::backend::Kind::CPU);
        BOOST_CHECK(!batch[i].accelerated);
        BOOST_CHECK(!batch[i].error.empty());
    }
}

BOOST_AUTO_TEST_CASE(cuda_prepared_batch_product_digest_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;
    constexpr uint32_t kBatchSize = 3;

    const CBlockHeader base_header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(base_header.seed_a, base_header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(base_header.seed_b, base_header.matmul_dim);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = base_header;
        header.nNonce64 += i;
        header.nNonce = static_cast<uint32_t>(header.nNonce64);
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputs(
            header,
            kTranscriptBlockSize,
            kNoiseRank));
    }

    matmul::accelerated::ResetMatMulBackendRuntimeStats();
    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
        headers,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared_inputs,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    const auto stats = matmul::accelerated::ProbeMatMulBackendRuntimeStats();
    BOOST_CHECK_EQUAL(stats.digest_requests, kBatchSize);
    BOOST_CHECK_EQUAL(stats.requested_cuda, kBatchSize);
    if (cuda_capability.available) {
        BOOST_CHECK_EQUAL(stats.cuda_successes, kBatchSize);
        BOOST_CHECK_EQUAL(stats.cuda_fallbacks_to_cpu, 0U);
    } else {
        BOOST_CHECK_EQUAL(stats.cuda_successes, 0U);
        BOOST_CHECK_EQUAL(stats.cuda_fallbacks_to_cpu, kBatchSize);
    }

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        const auto single = matmul::accelerated::ComputeMatMulDigestPrepared(
            headers[i],
            A,
            B,
            kTranscriptBlockSize,
            kNoiseRank,
            prepared_inputs[i],
            matmul::backend::Kind::CPU,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
        BOOST_REQUIRE(single.ok);
        BOOST_REQUIRE(batch[i].ok);
        BOOST_CHECK_EQUAL(batch[i].digest, single.digest);
        if (cuda_capability.available) {
            BOOST_CHECK_EQUAL(batch[i].backend, matmul::backend::Kind::CUDA);
            BOOST_CHECK(batch[i].accelerated);
            BOOST_CHECK(batch[i].error.empty());
        } else {
            BOOST_CHECK_EQUAL(batch[i].backend, matmul::backend::Kind::CPU);
            BOOST_CHECK(!batch[i].accelerated);
            BOOST_CHECK(!batch[i].error.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(backend_prepared_inputs_gpu_generation_path_preserves_digest)
{
    ScopedGpuInputEnv gpu_env("1");
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::METAL);
    const auto prepared_cpu = matmul::accelerated::ComputeMatMulDigestPrepared(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared,
        matmul::backend::Kind::CPU);
    const auto direct_cpu = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CPU);

    BOOST_REQUIRE(prepared_cpu.ok);
    BOOST_REQUIRE(direct_cpu.ok);
    BOOST_CHECK_EQUAL(prepared_cpu.digest, direct_cpu.digest);
}

BOOST_AUTO_TEST_CASE(backend_prepared_inputs_gpu_generation_auto_mode_records_profile_samples)
{
    ScopedGpuInputEnv gpu_env(nullptr);
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const auto profile_before = qtc::metal::ProbeMatMulInputGenerationProfile();
    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::METAL);
    const auto profile_after = qtc::metal::ProbeMatMulInputGenerationProfile();

    if (!profile_after.available) {
        BOOST_CHECK(!profile_after.reason.empty());
        return;
    }

    const bool expect_gpu_generation = matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::METAL,
        header.matmul_dim,
        kTranscriptBlockSize,
        kNoiseRank);
    if (expect_gpu_generation) {
        BOOST_CHECK_GT(profile_after.samples, profile_before.samples);
    } else {
        BOOST_CHECK_EQUAL(profile_after.samples, profile_before.samples);
    }
    BOOST_CHECK(!profile_after.reason.empty());
    BOOST_CHECK(!prepared.compress_vec.empty());
}

BOOST_AUTO_TEST_CASE(cuda_backend_prepared_inputs_gpu_generation_path_preserves_digest)
{
    ScopedGpuInputEnv gpu_env("1");
    ScopedCudaDevicePreparedInputsEnv device_inputs_env("1");
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA);
    if (cuda_capability.available) {
        BOOST_CHECK(prepared.cuda_generated_inputs != nullptr);
        BOOST_CHECK(!prepared.noise.has_value());
        BOOST_CHECK(prepared.compress_vec.empty());
    } else {
        BOOST_CHECK(prepared.noise.has_value());
        BOOST_CHECK(!prepared.compress_vec.empty());
    }
    const auto prepared_cpu = matmul::accelerated::ComputeMatMulDigestPrepared(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared,
        matmul::backend::Kind::CPU);
    const auto direct_cpu = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CPU);

    BOOST_REQUIRE(prepared_cpu.ok);
    BOOST_REQUIRE(direct_cpu.ok);
    BOOST_CHECK_EQUAL(prepared_cpu.digest, direct_cpu.digest);
}

BOOST_AUTO_TEST_CASE(cuda_backend_prepared_inputs_gpu_generation_auto_mode_records_profile_samples)
{
    ScopedGpuInputEnv gpu_env(nullptr);
    ScopedCudaDevicePreparedInputsEnv device_inputs_env(nullptr);
    constexpr uint32_t kTranscriptBlockSize = 8;
    constexpr uint32_t kNoiseRank = 4;

    const CBlockHeader header = MakeCandidateHeaderWithDim(256);
    const auto profile_before = qtc::cuda::ProbeMatMulInputGenerationProfile();
    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA);
    const auto profile_after = qtc::cuda::ProbeMatMulInputGenerationProfile();

    if (!profile_after.available) {
        BOOST_CHECK(!profile_after.reason.empty());
        return;
    }

    const bool expect_gpu_generation = matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::CUDA,
        header.matmul_dim,
        kTranscriptBlockSize,
        kNoiseRank);
    if (expect_gpu_generation) {
        BOOST_CHECK_GT(profile_after.samples, profile_before.samples);
    } else {
        BOOST_CHECK_EQUAL(profile_after.samples, profile_before.samples);
    }
    BOOST_CHECK(!profile_after.reason.empty());
    BOOST_CHECK(!prepared.compress_vec.empty());
}

BOOST_AUTO_TEST_CASE(cuda_backend_prepared_inputs_auto_mode_uses_device_inputs_for_product_digest_mainnet_shape)
{
    ScopedGpuInputEnv gpu_env(nullptr);
    ScopedCudaDevicePreparedInputsEnv device_inputs_env(nullptr);
    constexpr uint32_t kTranscriptBlockSize = 16;
    constexpr uint32_t kNoiseRank = 8;

    const CBlockHeader header = MakeCandidateHeaderWithDim(512);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);
    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    if (cuda_capability.available) {
        BOOST_CHECK(prepared.cuda_generated_inputs != nullptr);
        BOOST_CHECK(!prepared.noise.has_value());
        BOOST_CHECK(prepared.compress_vec.empty());
    } else {
        BOOST_CHECK(prepared.noise.has_value());
        BOOST_CHECK(!prepared.compress_vec.empty());
    }
}

BOOST_AUTO_TEST_CASE(cuda_variable_base_device_batch_matches_cpu_product_digest)
{
    ScopedGpuInputEnv gpu_env("1");
    ScopedCudaDevicePreparedInputsEnv device_inputs_env("1");
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    std::vector<CBlockHeader> headers;
    headers.push_back(MakeCandidateHeaderWithDim(32));
    headers.push_back(MakeCandidateHeaderWithDim(32));
    headers[1].nNonce64 = 43;
    headers[1].nNonce = static_cast<uint32_t>(headers[1].nNonce64);
    headers[1].hashMerkleRoot = ParseUint256("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    headers[1].seed_a = ParseUint256("1111111111111111111111111111111111111111111111111111111111111111");
    headers[1].seed_b = ParseUint256("2222222222222222222222222222222222222222222222222222222222222222");

    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_batch =
        matmul::accelerated::PrepareMatMulDigestInputsBatchForBackend(
            headers,
            kTranscriptBlockSize,
            kNoiseRank,
            matmul::backend::Kind::CUDA,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(prepared_batch.size(), headers.size());

    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);
    if (!cuda_capability.available) {
        return;
    }
    for (const auto& prepared : prepared_batch) {
        BOOST_REQUIRE(prepared.cuda_generated_inputs != nullptr);
    }

    const auto batch_results = matmul::accelerated::ComputeMatMulDigestPreparedVariableBaseBatchForMining(
        headers,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared_batch,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_REQUIRE_EQUAL(batch_results.size(), headers.size());
    for (size_t i = 0; i < headers.size(); ++i) {
        BOOST_REQUIRE_MESSAGE(batch_results[i].ok, batch_results[i].error);
        BOOST_CHECK_EQUAL(batch_results[i].backend, matmul::backend::Kind::CUDA);
        const auto A = matmul::SharedFromSeed(headers[i].seed_a, headers[i].matmul_dim);
        const auto B = matmul::SharedFromSeed(headers[i].seed_b, headers[i].matmul_dim);
        const uint256 cpu_digest = matmul::accelerated::ComputeDigestCpuFromPreparedInputs(
            *A,
            *B,
            prepared_batch[i],
            kTranscriptBlockSize,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
        BOOST_CHECK_EQUAL(batch_results[i].digest, cpu_digest);
    }
}

BOOST_AUTO_TEST_CASE(cuda_nonce_seed_v2_mainnet_boundary_variable_base_product_digest_matches_cpu)
{
    ScopedGpuInputEnv gpu_env("1");
    ScopedCudaDevicePreparedInputsEnv device_inputs_env("1");
    constexpr uint32_t kN = 512;
    constexpr uint32_t kTranscriptBlockSize = 16;
    constexpr uint32_t kNoiseRank = 8;
    constexpr uint32_t kActivationHeight = 125'000;
    constexpr uint32_t kHeight125000NBits = 0x1d0b8746U;
    constexpr uint32_t kBatchSize = 2;

    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);
    if (!cuda_capability.available) {
        return;
    }

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_batch;
    headers.reserve(kBatchSize);
    prepared_batch.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = MakeCandidateHeaderWithDim(kN);
        header.nVersion = 4;
        header.nBits = kHeight125000NBits;
        header.nTime = 1'773'277'390U + i;
        header.nNonce64 = 125'000 + i;
        header.nNonce = static_cast<uint32_t>(header.nNonce64);
        header.seed_a = DeterministicMatMulSeedV2(header, kActivationHeight, 0);
        header.seed_b = DeterministicMatMulSeedV2(header, kActivationHeight, 1);

        headers.push_back(header);
        prepared_batch.push_back(matmul::accelerated::PrepareMatMulDigestInputsForBackend(
            header,
            kTranscriptBlockSize,
            kNoiseRank,
            matmul::backend::Kind::CUDA,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED));
        BOOST_REQUIRE(prepared_batch.back().cuda_generated_inputs != nullptr);
    }

    const auto batch_results = matmul::accelerated::ComputeMatMulDigestPreparedVariableBaseBatchForMining(
        headers,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared_batch,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_REQUIRE_EQUAL(batch_results.size(), headers.size());
    for (size_t i = 0; i < headers.size(); ++i) {
        BOOST_REQUIRE_MESSAGE(batch_results[i].ok, batch_results[i].error);
        BOOST_CHECK_EQUAL(batch_results[i].backend, matmul::backend::Kind::CUDA);
        BOOST_CHECK(batch_results[i].accelerated);

        const matmul::Matrix matrix_a = matmul::FromSeed(headers[i].seed_a, kN);
        const matmul::Matrix matrix_b = matmul::FromSeed(headers[i].seed_b, kN);
        BOOST_CHECK_EQUAL(
            batch_results[i].digest,
            ComputeReferenceProductDigest(headers[i], matrix_a, matrix_b, kTranscriptBlockSize, kNoiseRank));
    }
}

BOOST_AUTO_TEST_CASE(cuda_backend_prepared_inputs_auto_mode_keeps_transcript_digest_on_host_inputs)
{
    ScopedGpuInputEnv gpu_env(nullptr);
    ScopedCudaDevicePreparedInputsEnv device_inputs_env(nullptr);
    constexpr uint32_t kTranscriptBlockSize = 16;
    constexpr uint32_t kNoiseRank = 8;

    const CBlockHeader header = MakeCandidateHeaderWithDim(512);
    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::TRANSCRIPT);

    BOOST_CHECK(prepared.noise.has_value());
    BOOST_CHECK(!prepared.compress_vec.empty());
    BOOST_CHECK(prepared.cuda_generated_inputs == nullptr);
}

BOOST_AUTO_TEST_CASE(backend_runtime_stats_track_cpu_digest_requests)
{
    matmul::accelerated::ResetMatMulBackendRuntimeStats();

    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CPU);
    BOOST_REQUIRE(digest_result.ok);

    const auto stats = matmul::accelerated::ProbeMatMulBackendRuntimeStats();
    BOOST_CHECK_EQUAL(stats.digest_requests, 1U);
    BOOST_CHECK_EQUAL(stats.requested_cpu, 1U);
    BOOST_CHECK_EQUAL(stats.requested_metal, 0U);
    BOOST_CHECK_EQUAL(stats.requested_cuda, 0U);
    BOOST_CHECK_EQUAL(stats.metal_successes, 0U);
    BOOST_CHECK_EQUAL(stats.metal_fallbacks_to_cpu, 0U);
    BOOST_CHECK_EQUAL(stats.cuda_successes, 0U);
    BOOST_CHECK_EQUAL(stats.cuda_fallbacks_to_cpu, 0U);
}

BOOST_AUTO_TEST_CASE(gpu_input_auto_mode_disables_after_hard_failure)
{
    ScopedGpuInputEnv gpu_env(nullptr);
    matmul::accelerated::ResetMatMulBackendRuntimeStats();

    const CBlockHeader header = MakeCandidateHeader();
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kInvalidNoiseRank = 16;

    (void)matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kInvalidNoiseRank,
        matmul::backend::Kind::METAL);
    const auto after_failure = matmul::accelerated::ProbeMatMulBackendRuntimeStats();

#if defined(__APPLE__)
    constexpr uint32_t kValidNoiseRank = 2;
    BOOST_CHECK_GE(after_failure.gpu_input_generation_attempts, 0U);
    BOOST_CHECK_GE(after_failure.gpu_input_generation_failures, 0U);
    const uint64_t attempts_after_failure = after_failure.gpu_input_generation_attempts;

    (void)matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kValidNoiseRank,
        matmul::backend::Kind::METAL);
    const auto after_second = matmul::accelerated::ProbeMatMulBackendRuntimeStats();
    if (after_failure.gpu_input_auto_disabled) {
        BOOST_CHECK(after_second.gpu_input_auto_disabled);
        BOOST_CHECK_EQUAL(after_second.gpu_input_generation_attempts, attempts_after_failure);
        BOOST_CHECK_GE(after_second.gpu_input_auto_disabled_skips, 1U);
    } else {
        BOOST_CHECK_GE(after_second.gpu_input_generation_attempts, attempts_after_failure);
    }
#else
    BOOST_CHECK_EQUAL(after_failure.gpu_input_generation_attempts, 0U);
    BOOST_CHECK_EQUAL(after_failure.gpu_input_generation_failures, 0U);
    BOOST_CHECK(!after_failure.gpu_input_auto_disabled);
#endif
}

BOOST_AUTO_TEST_CASE(gpu_input_forced_mode_keeps_attempting_after_failures)
{
    ScopedGpuInputEnv gpu_env("1");
    matmul::accelerated::ResetMatMulBackendRuntimeStats();

    const CBlockHeader header = MakeCandidateHeader();
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kInvalidNoiseRank = 16;

    (void)matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kInvalidNoiseRank,
        matmul::backend::Kind::METAL);
    (void)matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kInvalidNoiseRank,
        matmul::backend::Kind::METAL);

    const auto stats = matmul::accelerated::ProbeMatMulBackendRuntimeStats();
    BOOST_CHECK_EQUAL(stats.gpu_input_generation_attempts, 2U);
    BOOST_CHECK_EQUAL(stats.gpu_input_generation_failures, 2U);
    BOOST_CHECK(!stats.gpu_input_auto_disabled);
    BOOST_CHECK_EQUAL(stats.gpu_input_auto_disabled_skips, 0U);
}

BOOST_AUTO_TEST_CASE(gpu_input_auto_disable_policy_distinguishes_hard_and_transient_failures)
{
    BOOST_CHECK(matmul::accelerated::ShouldDisableGpuInputAutoModeForError("invalid dimensions for GPU input generation"));
    BOOST_CHECK(matmul::accelerated::ShouldDisableGpuInputAutoModeForError("noise rank exceeds matrix dimension"));
    BOOST_CHECK(matmul::accelerated::ShouldDisableGpuInputAutoModeForError("matrix dimension must be divisible by transcript block size"));
    BOOST_CHECK(matmul::accelerated::ShouldDisableGpuInputAutoModeForError("input generation dimensions exceed supported bounds"));
    BOOST_CHECK(matmul::accelerated::ShouldDisableGpuInputAutoModeForError("Metal context initialization failed"));

    BOOST_CHECK(!matmul::accelerated::ShouldDisableGpuInputAutoModeForError("Failed to create Metal command buffer"));
    BOOST_CHECK(!matmul::accelerated::ShouldDisableGpuInputAutoModeForError("unknown Metal command failure"));
}

BOOST_AUTO_TEST_CASE(gpu_input_auto_policy_is_backend_specific)
{
    ScopedGpuInputEnv gpu_env(nullptr);

#if defined(__APPLE__)
    BOOST_CHECK(matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::METAL,
        /*n=*/512,
        /*b=*/16,
        /*r=*/8));
#else
    BOOST_CHECK(!matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::METAL,
        /*n=*/512,
        /*b=*/16,
        /*r=*/8));
#endif
    BOOST_CHECK(!matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::METAL,
        /*n=*/256,
        /*b=*/8,
        /*r=*/4));
    BOOST_CHECK(!matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::METAL,
        /*n=*/64,
        /*b=*/8,
        /*r=*/4));
    BOOST_CHECK(matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::CUDA,
        /*n=*/512,
        /*b=*/16,
        /*r=*/8));
    BOOST_CHECK(matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::CUDA,
        /*n=*/256,
        /*b=*/8,
        /*r=*/4));
    BOOST_CHECK(!matmul::accelerated::ShouldUseGpuGeneratedInputsForShape(
        matmul::backend::Kind::CUDA,
        /*n=*/64,
        /*b=*/8,
        /*r=*/4));
}

BOOST_AUTO_TEST_CASE(metal_retry_policy_only_retries_uploaded_base_stale_errors)
{
    BOOST_CHECK(matmul::accelerated::ShouldRetryMetalDigestWithoutUploadedBase(
        "uploaded base matrices are unavailable or stale for requested dimension"));

    BOOST_CHECK(!matmul::accelerated::ShouldRetryMetalDigestWithoutUploadedBase(
        "Failed to create Metal command buffer"));
    BOOST_CHECK(!matmul::accelerated::ShouldRetryMetalDigestWithoutUploadedBase(
        "invalid MatMul request dimensions"));
}

BOOST_AUTO_TEST_CASE(cuda_digest_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA);

    BOOST_CHECK(digest_result.ok);
    BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);

    // The legacy TRANSCRIPT scheme is CPU-only on every GPU backend (QTC O5):
    // the request must report a clean fallback with a reason, never a CUDA hit.
    (void)cuda_capability;
    BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
    BOOST_CHECK(!digest_result.accelerated);
    BOOST_CHECK(!digest_result.error.empty());
}

BOOST_AUTO_TEST_CASE(cuda_regtest_shape_digest_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 8;
    constexpr uint32_t kNoiseRank = 4;

    const CBlockHeader header = MakeCandidateHeaderWithDim(64);
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA);

    BOOST_CHECK(digest_result.ok);
    BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);

    // The legacy TRANSCRIPT scheme is CPU-only on every GPU backend (QTC O5):
    // the request must report a clean fallback with a reason, never a CUDA hit.
    (void)cuda_capability;
    BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
    BOOST_CHECK(!digest_result.accelerated);
    BOOST_CHECK(!digest_result.error.empty());
}

BOOST_AUTO_TEST_CASE(cuda_product_digest_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 4;
    constexpr uint32_t kNoiseRank = 2;

    const CBlockHeader header = MakeCandidateHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_CHECK(digest_result.ok);
    BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);

    if (cuda_capability.available) {
        BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CUDA);
        BOOST_CHECK(digest_result.accelerated);
        BOOST_CHECK(digest_result.error.empty());
    } else {
        BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
        BOOST_CHECK(!digest_result.accelerated);
        BOOST_CHECK(!digest_result.error.empty());
    }
}

BOOST_AUTO_TEST_CASE(cuda_regtest_shape_product_digest_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 8;
    constexpr uint32_t kNoiseRank = 4;

    const CBlockHeader header = MakeCandidateHeaderWithDim(64);
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_CHECK(digest_result.ok);
    BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);

    if (cuda_capability.available) {
        BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CUDA);
        BOOST_CHECK(digest_result.accelerated);
        BOOST_CHECK(digest_result.error.empty());
    } else {
        BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
        BOOST_CHECK(!digest_result.accelerated);
        BOOST_CHECK(!digest_result.error.empty());
    }
}

// QTC O5: expected value re-pinned to the oracle-v2 / product-digest-v4 CPU output for the same inputs.
BOOST_AUTO_TEST_CASE(strict_regtest_warning_repro_cpu_digest_matches_logged_vector)
{
    constexpr uint32_t kTranscriptBlockSize = 8;
    constexpr uint32_t kNoiseRank = 4;

    const CBlockHeader header = MakeStrictRegtestWarningReproHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);

    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_CHECK_EQUAL(
        cpu_digest,
        ParseUint256("36a23eb97086ecdb7ca1405647c5362820ef7bdb5d6ae3974548aef67a54a1b6"));
}

BOOST_AUTO_TEST_CASE(cuda_strict_regtest_warning_repro_direct_and_batch_match_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 8;
    constexpr uint32_t kNoiseRank = 4;

    const CBlockHeader header = MakeStrictRegtestWarningReproHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);
    const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
        header,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA);
    const uint256 cpu_digest = matmul::accelerated::ComputeDigestCpuFromPreparedInputs(
        A,
        B,
        prepared,
        kTranscriptBlockSize,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    const auto single = matmul::accelerated::ComputeMatMulDigestPrepared(
        header,
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
        {header},
        A,
        B,
        kTranscriptBlockSize,
        kNoiseRank,
        {prepared},
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

    BOOST_REQUIRE(single.ok);
    BOOST_REQUIRE_EQUAL(batch.size(), 1U);
    BOOST_REQUIRE(batch[0].ok);

    BOOST_CHECK_EQUAL(single.digest, cpu_digest);
    BOOST_CHECK_EQUAL(batch[0].digest, cpu_digest);

    if (cuda_capability.available) {
        BOOST_CHECK_EQUAL(single.backend, matmul::backend::Kind::CUDA);
        BOOST_CHECK(single.accelerated);
        BOOST_CHECK_EQUAL(batch[0].backend, matmul::backend::Kind::CUDA);
        BOOST_CHECK(batch[0].accelerated);
    }
}

BOOST_AUTO_TEST_CASE(cuda_strict_regtest_warning_repro_nonce_scan_matches_cpu_or_cleanly_falls_back)
{
    constexpr uint32_t kTranscriptBlockSize = 8;
    constexpr uint32_t kNoiseRank = 4;
    constexpr uint64_t kNonceCount = 16;

    CBlockHeader header = MakeStrictRegtestWarningReproHeader();
    const matmul::Matrix A = matmul::FromSeed(header.seed_a, header.matmul_dim);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, header.matmul_dim);
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    for (uint64_t nonce = 0; nonce < kNonceCount; ++nonce) {
        header.nNonce64 = nonce;
        header.nNonce = static_cast<uint32_t>(nonce);
        header.matmul_digest.SetNull();

        const auto prepared = matmul::accelerated::PrepareMatMulDigestInputsForBackend(
            header,
            kTranscriptBlockSize,
            kNoiseRank,
            matmul::backend::Kind::CUDA);
        const uint256 cpu_digest = matmul::accelerated::ComputeDigestCpuFromPreparedInputs(
            A,
            B,
            prepared,
            kTranscriptBlockSize,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
        const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
            {header},
            A,
            B,
            kTranscriptBlockSize,
            kNoiseRank,
            {prepared},
            matmul::backend::Kind::CUDA,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

        BOOST_REQUIRE_EQUAL(batch.size(), 1U);
        BOOST_REQUIRE(batch[0].ok);
        BOOST_CHECK_EQUAL(batch[0].digest, cpu_digest);
        if (cuda_capability.available) {
            BOOST_CHECK_EQUAL(batch[0].backend, matmul::backend::Kind::CUDA);
            BOOST_CHECK(batch[0].accelerated);
        }
    }
}

// ---- QTC O5: oracle v2 + product digest v4 CUDA parity ----

BOOST_AUTO_TEST_CASE(cuda_oracle_v2_fill_matches_cpu_reference_and_vectors)
{
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);
    if (!cuda_capability.available) {
        const auto unavailable = qtc::cuda::FillFromOracleGPU(uint256{}, 0, 8);
        BOOST_CHECK(!unavailable.success);
        BOOST_CHECK(!unavailable.error.empty());
        return;
    }

    // from_oracle_extra vectors (test/reference/test_vectors.json, oracle v2).
    struct OracleVector {
        const char* seed_hex;
        uint32_t index;
        uint32_t expected;
    };
    const OracleVector vectors[] = {
        {"0000000000000000000000000000000000000000000000000000000000000000", 100U, 2060225844U},
        {"0000000000000000000000000000000000000000000000000000000000000000", 255U, 126251429U},
        {"0000000000000000000000000000000000000000000000000000000000000000", 1000U, 655637585U},
        {"0000000000000000000000000000000000000000000000000000000000000000", 65535U, 178895147U},
        {"0000000000000000000000000000000000000000000000000000000000000000", 4294967295U, 202950684U},
        {"4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150", 0U, 360032607U},
        {"4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150", 1U, 369286479U},
        {"4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150", 100U, 1349016275U},
        {"4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150", 999U, 1873833507U},
        {"c6a811f7f75fe4e64be106a50351aed9c04403a74bfe7b4bbe59f7311722b735", 12345U, 995759357U},
    };
    for (const auto& vector : vectors) {
        const uint256 seed = ParseUint256(vector.seed_hex);
        const auto filled = qtc::cuda::FillFromOracleGPU(seed, vector.index, 1);
        BOOST_REQUIRE_MESSAGE(filled.success, filled.error);
        BOOST_REQUIRE_EQUAL(filled.values.size(), 1U);
        BOOST_CHECK_EQUAL(filled.values[0], vector.expected);
        BOOST_CHECK_EQUAL(filled.values[0], matmul::field::from_oracle(seed, vector.index));
    }

    // from_seed_4x4 first row, verbatim from the vectors file.
    const auto zero4 = qtc::cuda::FillFromOracleGPU(uint256{}, 0, 4);
    BOOST_REQUIRE_MESSAGE(zero4.success, zero4.error);
    const uint32_t expected_row0[4] = {1432335981U, 1985401759U, 1463849330U, 1808620315U};
    BOOST_CHECK_EQUAL_COLLECTIONS(
        zero4.values.begin(), zero4.values.end(), std::begin(expected_row0), std::end(expected_row0));

    // Whole base matrices: CUDA oracle fill == matmul::FromSeed (which the field
    // tests pin to from_seed_4x4 / from_seed_8x8).
    for (const uint32_t n : {4U, 8U, 64U, 512U}) {
        const uint256 seed = n <= 8
            ? uint256{}
            : ParseUint256("4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150");
        const matmul::Matrix cpu = matmul::FromSeed(seed, n);
        const auto filled = qtc::cuda::FillFromOracleGPU(seed, 0, n * n);
        BOOST_REQUIRE_MESSAGE(filled.success, filled.error);
        BOOST_REQUIRE_EQUAL(filled.values.size(), static_cast<size_t>(n) * n);
        BOOST_CHECK_EQUAL_COLLECTIONS(
            filled.values.begin(), filled.values.end(), cpu.data(), cpu.data() + static_cast<size_t>(n) * n);
    }

    // Unaligned index range crossing 8-lane block boundaries.
    const uint256 seed = ParseUint256("c6a811f7f75fe4e64be106a50351aed9c04403a74bfe7b4bbe59f7311722b735");
    const auto range = qtc::cuda::FillFromOracleGPU(seed, 12341, 37);
    BOOST_REQUIRE_MESSAGE(range.success, range.error);
    std::vector<matmul::field::Element> cpu_range(37);
    matmul::field::fill_from_oracle(seed, 12341, 37, cpu_range.data());
    BOOST_CHECK_EQUAL_COLLECTIONS(range.values.begin(), range.values.end(), cpu_range.begin(), cpu_range.end());
    BOOST_CHECK_EQUAL(range.values[4], 995759357U);
}

BOOST_AUTO_TEST_CASE(cuda_product_tile_hashes_match_cpu_for_perturbed_matrices)
{
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);
    struct Shape {
        uint32_t n;
        uint32_t b;
    };
    const Shape shapes[] = {{8U, 4U}, {64U, 8U}, {512U, 16U}};
    const uint256 sigma = ParseUint256("5555555555555555555555555555555555555555555555555555555555555555");

    for (const Shape shape : shapes) {
        // Seeded matrices double as "perturbed" inputs: every element is a
        // canonical field element, which is all the GEMM/tile-hash stage sees.
        const matmul::Matrix A0 = matmul::FromSeed(
            ParseUint256("1111111111111111111111111111111111111111111111111111111111111111"), shape.n);
        const matmul::Matrix B0 = matmul::FromSeed(
            ParseUint256("2222222222222222222222222222222222222222222222222222222222222222"), shape.n);
        const matmul::Matrix A1 = matmul::FromSeed(
            ParseUint256("3333333333333333333333333333333333333333333333333333333333333333"), shape.n);
        const matmul::Matrix B1 = matmul::FromSeed(
            ParseUint256("4444444444444444444444444444444444444444444444444444444444444444"), shape.n);
        const matmul::field::Element* a_ptrs[] = {A0.data(), A1.data()};
        const matmul::field::Element* b_ptrs[] = {B0.data(), B1.data()};
        const uint256 sigma1 = ParseUint256("6666666666666666666666666666666666666666666666666666666666666666");
        const uint256 sigmas[] = {sigma, sigma1};

        const auto cuda = qtc::cuda::ComputeProductTileHashesBatch({
            .n = shape.n,
            .b = shape.b,
            .batch_size = 2,
            .matrix_a_perturbed = a_ptrs,
            .matrix_b_perturbed = b_ptrs,
            .sigmas = sigmas,
            .return_tile_hashes = true,
        });
        BOOST_CHECK_EQUAL(cuda.available, cuda_capability.available);
        if (!cuda_capability.available) {
            BOOST_CHECK(!cuda.success);
            BOOST_CHECK(!cuda.error.empty());
            continue;
        }
        BOOST_REQUIRE_MESSAGE(cuda.success, cuda.error);

        const uint32_t blocks_per_axis = shape.n / shape.b;
        const uint32_t tiles = blocks_per_axis * blocks_per_axis;
        BOOST_REQUIRE_EQUAL(cuda.tiles_per_request, tiles);
        BOOST_REQUIRE_EQUAL(cuda.tile_hashes.size(), static_cast<size_t>(2) * tiles);

        const matmul::Matrix C0 = A0 * B0;
        const matmul::Matrix C1 = A1 * B1;
        const auto cpu0 = matmul::transcript::ComputeProductTileHashes(C0, shape.b);
        const auto cpu1 = matmul::transcript::ComputeProductTileHashes(C1, shape.b);
        BOOST_REQUIRE_EQUAL(cpu0.size(), tiles);
        for (uint32_t i = 0; i < tiles; ++i) {
            BOOST_CHECK_EQUAL(cuda.tile_hashes[i], cpu0[i]);
            BOOST_CHECK_EQUAL(cuda.tile_hashes[tiles + i], cpu1[i]);
        }

        BOOST_CHECK_EQUAL(
            matmul::transcript::ComputeProductCommittedDigestFromTileHashes(
                Span<const uint256>{cuda.tile_hashes.data(), tiles}, sigma, shape.n, shape.b),
            matmul::transcript::ComputeProductCommittedDigest(C0, shape.b, sigma));
        BOOST_CHECK_EQUAL(
            matmul::transcript::ComputeProductCommittedDigestFromTileHashes(
                Span<const uint256>{cuda.tile_hashes.data() + tiles, tiles}, sigma, shape.n, shape.b),
            matmul::transcript::ComputeProductCommittedDigest(C1, shape.b, sigma));

        // Device-side finish (root + outer SHA256d) must equal the host finish
        // from the same tile hashes and the CPU reference digest.
        BOOST_REQUIRE_EQUAL(cuda.digests.size(), 2U);
        BOOST_CHECK_EQUAL(cuda.digests[0], matmul::transcript::ComputeProductCommittedDigest(C0, shape.b, sigma));
        BOOST_CHECK_EQUAL(cuda.digests[1], matmul::transcript::ComputeProductCommittedDigest(C1, shape.b, sigma1));
        BOOST_CHECK_EQUAL(
            cuda.digests[1],
            matmul::transcript::ComputeProductCommittedDigestFromTileHashes(
                Span<const uint256>{cuda.tile_hashes.data() + tiles, tiles}, sigma1, shape.n, shape.b));
    }
}

BOOST_AUTO_TEST_CASE(cuda_mainnet_shape_product_digest_matches_cpu)
{
    constexpr uint32_t kN = 512;
    constexpr uint32_t kTranscriptBlockSize = 16;
    constexpr uint32_t kNoiseRank = 8;

    CBlockHeader header = MakeCandidateHeaderWithDim(kN);
    header.nVersion = 0x20000000;
    header.nTime = 1'790'000'123U;
    header.nBits = 0x1e063c74U;
    header.nNonce64 = 0x1d3f9a7c5b2e4801ULL;
    header.nNonce = static_cast<uint32_t>(header.nNonce64);
    header.hashPrevBlock = ParseUint256("7a1c2f3e4d5b6a798897a6b5c4d3e2f10f1e2d3c4b5a69788796a5b4c3d2e1f0");
    header.hashMerkleRoot = ParseUint256("0f1e2d3c4b5a69788796a5b4c3d2e1f07a1c2f3e4d5b6a798897a6b5c4d3e2f1");
    header.seed_a = ParseUint256("9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08");
    header.seed_b = ParseUint256("2c26b46b68ffc68ff99b453c1d30413413422d706483bfa0f98a5e886266e7ae");

    const matmul::Matrix A = matmul::FromSeed(header.seed_a, kN);
    const matmul::Matrix B = matmul::FromSeed(header.seed_b, kN);
    const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
        header, A, B, kTranscriptBlockSize, kNoiseRank, matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_CHECK_EQUAL(
        cpu_digest,
        ComputeReferenceProductDigest(header, A, B, kTranscriptBlockSize, kNoiseRank));
    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);

    const auto check = [&](const matmul::accelerated::DigestResult& digest_result) {
        BOOST_CHECK(digest_result.ok);
        BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);
        if (cuda_capability.available) {
            BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CUDA);
            BOOST_CHECK(digest_result.accelerated);
            BOOST_CHECK_MESSAGE(digest_result.error.empty(), digest_result.error);
        } else {
            BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
            BOOST_CHECK(!digest_result.accelerated);
            BOOST_CHECK(!digest_result.error.empty());
        }
    };

    {
        // AUTO policy: device-generated noise for the mainnet shape.
        ScopedGpuInputEnv gpu_env(nullptr);
        ScopedCudaDevicePreparedInputsEnv device_inputs_env(nullptr);
        check(matmul::accelerated::ComputeMatMulDigest(
            header, A, B, kTranscriptBlockSize, kNoiseRank,
            matmul::backend::Kind::CUDA, matmul::accelerated::DigestScheme::PRODUCT_COMMITTED));
    }
    {
        // Host-prepared noise through the same CUDA path.
        ScopedGpuInputEnv gpu_env("0");
        check(matmul::accelerated::ComputeMatMulDigest(
            header, A, B, kTranscriptBlockSize, kNoiseRank,
            matmul::backend::Kind::CUDA, matmul::accelerated::DigestScheme::PRODUCT_COMMITTED));
    }
}

BOOST_AUTO_TEST_CASE(cuda_variable_base_host_noise_regtest_shape_batch_matches_cpu_product_digest)
{
    // The nonce-seeded regtest mining path: per-header seeds, host-prepared noise
    // (the AUTO GPU-input policy stays off at n=64), CUDA regenerates A'/B' from
    // the seeds with oracle v2 and must agree with the CPU reference.
    ScopedGpuInputEnv gpu_env("0");
    constexpr uint32_t kN = 64;
    constexpr uint32_t kTranscriptBlockSize = 8;
    constexpr uint32_t kNoiseRank = 4;
    constexpr uint32_t kBatchSize = 5;

    std::vector<CBlockHeader> headers;
    headers.reserve(kBatchSize);
    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = MakeStrictRegtestWarningReproHeader();
        header.nNonce64 = 1000 + i;
        header.nNonce = static_cast<uint32_t>(header.nNonce64);
        const std::string suffix = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
        header.seed_a = ParseUint256("4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd1" + suffix);
        header.seed_b = ParseUint256("c6a811f7f75fe4e64be106a50351aed9c04403a74bfe7b4bbe59f7311722b7" + suffix);
        headers.push_back(header);
    }

    const auto prepared_batch = matmul::accelerated::PrepareMatMulDigestInputsBatchForBackend(
        headers,
        kTranscriptBlockSize,
        kNoiseRank,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(prepared_batch.size(), headers.size());
    for (const auto& prepared : prepared_batch) {
        BOOST_REQUIRE(prepared.noise.has_value());
        BOOST_REQUIRE(prepared.cuda_generated_inputs == nullptr);
    }

    const auto cuda_capability = matmul::backend::CapabilityFor(matmul::backend::Kind::CUDA);
    matmul::accelerated::ResetMatMulBackendRuntimeStats();
    const auto batch_results = matmul::accelerated::ComputeMatMulDigestPreparedVariableBaseBatchForMining(
        headers,
        kTranscriptBlockSize,
        kNoiseRank,
        prepared_batch,
        matmul::backend::Kind::CUDA,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(batch_results.size(), headers.size());

    const auto stats = matmul::accelerated::ProbeMatMulBackendRuntimeStats();
    BOOST_CHECK_EQUAL(stats.requested_cuda, kBatchSize);
    if (cuda_capability.available) {
        BOOST_CHECK_EQUAL(stats.cuda_successes, kBatchSize);
        BOOST_CHECK_EQUAL(stats.cuda_fallbacks_to_cpu, 0U);
    } else {
        BOOST_CHECK_EQUAL(stats.cuda_successes, 0U);
        BOOST_CHECK_EQUAL(stats.cuda_fallbacks_to_cpu, kBatchSize);
    }

    for (size_t i = 0; i < headers.size(); ++i) {
        BOOST_REQUIRE_MESSAGE(batch_results[i].ok, batch_results[i].error);
        const matmul::Matrix A = matmul::FromSeed(headers[i].seed_a, kN);
        const matmul::Matrix B = matmul::FromSeed(headers[i].seed_b, kN);
        const uint256 cpu_digest = matmul::accelerated::ComputeDigestCpuFromPreparedInputs(
            A,
            B,
            prepared_batch[i],
            kTranscriptBlockSize,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
        BOOST_CHECK_EQUAL(batch_results[i].digest, cpu_digest);
        BOOST_CHECK_EQUAL(
            cpu_digest,
            ComputeReferenceProductDigest(headers[i], A, B, kTranscriptBlockSize, kNoiseRank));
        if (cuda_capability.available) {
            BOOST_CHECK_EQUAL(batch_results[i].backend, matmul::backend::Kind::CUDA);
            BOOST_CHECK(batch_results[i].accelerated);
            BOOST_CHECK_MESSAGE(batch_results[i].error.empty(), batch_results[i].error);
        } else {
            BOOST_CHECK_EQUAL(batch_results[i].backend, matmul::backend::Kind::CPU);
            BOOST_CHECK(!batch_results[i].accelerated);
            BOOST_CHECK(!batch_results[i].error.empty());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
