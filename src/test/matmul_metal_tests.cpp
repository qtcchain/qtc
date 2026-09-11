// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <matmul/accelerated_solver.h>
#include <matmul/backend_capabilities.h>
#include <matmul/matmul_pow.h>
#include <matmul/noise.h>
#include <matmul/transcript.h>
#include <metal/matmul_accel.h>
#include <metal/matmul_accel_env.h>
#include <metal/nonce_accel.h>
#include <metal/oracle_accel.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Metal MatMul backend tests (oracle v2 + product digest v4).
//
// Every parity test below compares REAL device output against the CPU
// consensus reference (matmul::field::from_oracle, matmul::FromSeed,
// matmul::noise::Generate, matmul::transcript::ComputeProductCommittedDigest*).
// Tests that go through matmul::accelerated require the solver to route
// PRODUCT_COMMITTED requests to Metal; they assert backend == METAL so a silent
// CPU fallback is a failure, not a pass.

namespace {

uint256 ParseUint256(std::string_view hex)
{
    const auto parsed = uint256::FromHex(hex);
    BOOST_REQUIRE(parsed.has_value());
    return *parsed;
}

CBlockHeader BuildHeader(uint32_t n, uint64_t nonce64)
{
    CBlockHeader header{};
    header.nVersion = 1;
    header.hashPrevBlock = ParseUint256("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    header.hashMerkleRoot = ParseUint256("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100");
    header.nTime = 1'700'000'000U;
    header.nBits = 0x207fffffU;
    header.nNonce64 = nonce64;
    header.nNonce = static_cast<uint32_t>(nonce64);
    header.matmul_dim = static_cast<uint16_t>(n);
    header.seed_a = ParseUint256("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    header.seed_b = ParseUint256("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");
    return header;
}

uint256 RandomUint256(std::mt19937_64& rng)
{
    uint256 out;
    for (size_t i = 0; i < uint256::size(); ++i) {
        out.data()[i] = static_cast<unsigned char>(rng());
    }
    return out;
}

CBlockHeader RandomHeader(std::mt19937_64& rng, uint32_t n)
{
    CBlockHeader header = BuildHeader(n, rng());
    header.nVersion = 4;
    header.hashPrevBlock = RandomUint256(rng);
    header.hashMerkleRoot = RandomUint256(rng);
    header.nTime = 1'770'000'000U + static_cast<uint32_t>(rng() % 100'000);
    header.seed_a = RandomUint256(rng);
    header.seed_b = RandomUint256(rng);
    return header;
}

uint256 ComputeReferenceProductDigest(const CBlockHeader& header,
                                      const matmul::Matrix& matrix_a,
                                      const matmul::Matrix& matrix_b,
                                      uint32_t transcript_block_size,
                                      uint32_t noise_rank)
{
    const uint256 sigma = matmul::DeriveSigma(header);
    const auto noise = matmul::noise::Generate(sigma, header.matmul_dim, noise_rank);
    const auto a_prime = matrix_a + (noise.E_L * noise.E_R);
    const auto b_prime = matrix_b + (noise.F_L * noise.F_R);
    return matmul::transcript::ComputeProductCommittedDigestFromPerturbed(
        a_prime,
        b_prime,
        transcript_block_size,
        sigma);
}

void CheckWordsEqual(std::string_view label,
                     const std::vector<matmul::field::Element>& actual,
                     const matmul::field::Element* expected,
                     size_t expected_size)
{
    BOOST_REQUIRE_EQUAL(actual.size(), expected_size);
    const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected);
    BOOST_REQUIRE_MESSAGE(
        mismatch.first == actual.end(),
        label << " mismatch at index " << std::distance(actual.begin(), mismatch.first)
              << ": metal=" << *mismatch.first
              << " cpu=" << *mismatch.second);
}

class ScopedEnvVar
{
public:
    ScopedEnvVar(const char* name, const char* value) : m_name(name)
    {
        const char* current = std::getenv(name);
        if (current != nullptr) {
            m_had_original = true;
            m_original = current;
        }
#if defined(WIN32)
        _putenv_s(name, value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
#endif
    }

    ~ScopedEnvVar()
    {
#if defined(WIN32)
        _putenv_s(m_name, m_had_original ? m_original.c_str() : "");
#else
        if (m_had_original) {
            setenv(m_name, m_original.c_str(), 1);
        } else {
            unsetenv(m_name);
        }
#endif
    }

private:
    const char* m_name;
    bool m_had_original{false};
    std::string m_original;
};

class ThreadGate
{
public:
    explicit ThreadGate(size_t participants) : m_participants(participants)
    {
    }

    void ArriveAndWait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const size_t generation = m_generation;
        ++m_arrived;
        if (m_arrived == m_participants) {
            m_arrived = 0;
            ++m_generation;
            lock.unlock();
            m_cv.notify_all();
            return;
        }
        m_cv.wait(lock, [&] {
            return generation != m_generation;
        });
    }

private:
    const size_t m_participants;
    size_t m_arrived{0};
    size_t m_generation{0};
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

// Pinned product_digest_v4_n8_b4 vector (test/reference/test_vectors.json):
// A' and B' from canonical_matmul_n8_b4, sigma and the final digest from
// product_digest_v4_n8_b4 (digest_uint256 is the uint256::GetHex form).
constexpr std::string_view kPinnedV4SigmaHex{"ffc381ccd5e78ab52348ec8ba82f51d5feb0e857d7969ab0df9a5891c68cdf15"};
constexpr std::string_view kPinnedV4DigestHex{"0bd68fd1c252d7bfabec102e7cfe7adab5755ad0034459b072df1daea8707759"};
constexpr std::array<matmul::field::Element, 64> kPinnedV4APrime{{
    1250808675u, 1031609836u, 641254537u, 2014880093u, 337813477u, 537282586u, 29705999u, 470293751u,
    367070551u, 498089549u, 715163425u, 2121941823u, 543407349u, 906478229u, 1119109779u, 305170783u,
    729182874u, 1069604427u, 1097644910u, 971889561u, 2121179529u, 701625932u, 661036716u, 433249082u,
    317716062u, 878556432u, 654505513u, 2031074302u, 1570442429u, 453197794u, 1467690225u, 398970056u,
    1215558895u, 1344524680u, 1437268790u, 1143927535u, 1224381050u, 1619240365u, 193748136u, 758777870u,
    1881883323u, 1421142552u, 2138246248u, 1792270272u, 33782970u, 911514986u, 1551205634u, 1649133114u,
    785518483u, 861629953u, 1107455648u, 1816327622u, 205964647u, 138796389u, 995066876u, 944444022u,
    628651013u, 1733663456u, 1081905098u, 194789510u, 316614720u, 108094701u, 321930492u, 348060415u,
}};
constexpr std::array<matmul::field::Element, 64> kPinnedV4BPrime{{
    999636393u, 1353963538u, 334285782u, 441572004u, 1050328767u, 367802926u, 2017085005u, 1321252983u,
    910157771u, 150598436u, 506603745u, 2145307643u, 1149676891u, 1622349977u, 973591419u, 1230417989u,
    1203648617u, 1166150172u, 1805569825u, 1386684665u, 1865781955u, 884561512u, 432682081u, 1068993909u,
    182873767u, 1504079551u, 1753651153u, 1445182502u, 1260828012u, 1321151210u, 15008083u, 1026679938u,
    697118610u, 1860212148u, 1462030703u, 805601461u, 878805641u, 1568055006u, 334313094u, 1092141246u,
    1641187193u, 970334845u, 1580485365u, 1346699774u, 204357708u, 490013452u, 945661111u, 993685224u,
    1029406262u, 319309879u, 791157179u, 1201759412u, 347764021u, 2102187296u, 1956364931u, 636932083u,
    1622148234u, 1911858535u, 1426072986u, 1751834465u, 1315485698u, 1569981718u, 1133075784u, 1886922686u,
}};

} // namespace

BOOST_FIXTURE_TEST_SUITE(matmul_metal_tests, BasicTestingSetup)

// --- oracle v2 -------------------------------------------------------------

BOOST_AUTO_TEST_CASE(metal_oracle_v2_vectors_match_cpu)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        BOOST_TEST_MESSAGE("Skipping Metal oracle v2 vector test: " << probe.reason);
        return;
    }

    struct Vector {
        const char* seed_hex;
        uint32_t index;
        uint32_t result;
    };
    // from_oracle_extra / pinned_tv* entries of test/reference/test_vectors.json
    constexpr std::array<Vector, 11> kVectors{{
        {"0000000000000000000000000000000000000000000000000000000000000000", 0, 1432335981u},
        {"0000000000000000000000000000000000000000000000000000000000000000", 1, 1985401759u},
        {"0000000000000000000000000000000000000000000000000000000000000000", 7, 243940604u},
        {"0000000000000000000000000000000000000000000000000000000000000000", 100, 2060225844u},
        {"0000000000000000000000000000000000000000000000000000000000000000", 255, 126251429u},
        {"0000000000000000000000000000000000000000000000000000000000000000", 1000, 655637585u},
        {"0000000000000000000000000000000000000000000000000000000000000000", 65535, 178895147u},
        {"4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150", 42, 1637792496u},
        {"4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150", 100, 1349016275u},
        {"4504d44d861b69197db1d95e473442346c4f2bc1f5869996bdccd63cfbdbd150", 999, 1873833507u},
        {"c6a811f7f75fe4e64be106a50351aed9c04403a74bfe7b4bbe59f7311722b735", 12345, 995759357u},
    }};

    for (const auto& vector : kVectors) {
        const uint256 seed = ParseUint256(vector.seed_hex);
        const uint32_t count = vector.index + 1;
        const auto gpu = qtc::metal::GenerateOracleVectorGPUForTesting(seed, count);
        BOOST_REQUIRE_MESSAGE(gpu.success, gpu.error);
        BOOST_REQUIRE_EQUAL(gpu.values.size(), count);
        BOOST_CHECK_EQUAL(gpu.values[vector.index], vector.result);
        BOOST_CHECK_EQUAL(gpu.values[vector.index], matmul::field::from_oracle(seed, vector.index));

        std::vector<matmul::field::Element> cpu(count);
        matmul::field::fill_from_oracle(seed, 0, count, cpu.data());
        CheckWordsEqual("oracle_vector", gpu.values, cpu.data(), cpu.size());
    }

    // Partial trailing block (count not a multiple of 8).
    const uint256 seed = ParseUint256("c6a811f7f75fe4e64be106a50351aed9c04403a74bfe7b4bbe59f7311722b735");
    constexpr uint32_t kCount = 10'003;
    const auto gpu = qtc::metal::GenerateOracleVectorGPUForTesting(seed, kCount);
    BOOST_REQUIRE_MESSAGE(gpu.success, gpu.error);
    std::vector<matmul::field::Element> cpu(kCount);
    matmul::field::fill_from_oracle(seed, 0, kCount, cpu.data());
    CheckWordsEqual("oracle_vector_partial_block", gpu.values, cpu.data(), cpu.size());
}

BOOST_AUTO_TEST_CASE(metal_base_matrix_from_seed_matches_cpu_oracle_v2)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        BOOST_TEST_MESSAGE("Skipping Metal FromSeed test: " << probe.reason);
        return;
    }

    // Pinned from_seed_4x4 / from_seed_8x8 (zero seed).
    const uint256 zero_seed;
    const auto metal8 = qtc::metal::GenerateBaseMatrixFromSeedForTesting(8, zero_seed);
    BOOST_REQUIRE_MESSAGE(metal8.success, metal8.error);
    BOOST_REQUIRE_EQUAL(metal8.matrix.size(), 64U);
    BOOST_CHECK_EQUAL(metal8.matrix[0], 1432335981u);
    BOOST_CHECK_EQUAL(metal8.matrix[1], 1985401759u);
    BOOST_CHECK_EQUAL(metal8.matrix[7], 243940604u);
    BOOST_CHECK_EQUAL(metal8.matrix[8], 1134348657u);
    BOOST_CHECK_EQUAL(metal8.matrix[63], 1111172580u);
    const matmul::Matrix cpu8 = matmul::FromSeed(zero_seed, 8);
    CheckWordsEqual("from_seed_8x8", metal8.matrix, cpu8.data(), 64);

    const auto metal4 = qtc::metal::GenerateBaseMatrixFromSeedForTesting(4, zero_seed);
    BOOST_REQUIRE_MESSAGE(metal4.success, metal4.error);
    const matmul::Matrix cpu4 = matmul::FromSeed(zero_seed, 4);
    CheckWordsEqual("from_seed_4x4", metal4.matrix, cpu4.data(), 16);
    BOOST_CHECK_EQUAL(metal4.matrix[4], 305445043u);

    std::mt19937_64 rng(0x5eed0001ULL);
    for (const uint32_t n : {2u, 16u, 64u, 256u, 512u}) {
        const uint256 seed = RandomUint256(rng);
        const auto metal = qtc::metal::GenerateBaseMatrixFromSeedForTesting(n, seed);
        BOOST_REQUIRE_MESSAGE(metal.success, metal.error);
        const matmul::Matrix cpu = matmul::FromSeed(seed, n);
        CheckWordsEqual("from_seed", metal.matrix, cpu.data(), static_cast<size_t>(n) * n);
    }
}

BOOST_AUTO_TEST_CASE(metal_generated_noise_matches_cpu_oracle_v2)
{
    const auto profile = qtc::metal::ProbeMatMulInputGenerationProfile();
    if (!profile.available) {
        BOOST_TEST_MESSAGE("Skipping Metal noise generation test: " << profile.reason);
        return;
    }

    struct Shape {
        uint32_t n, b, r;
    };
    constexpr std::array<Shape, 4> kShapes{{{512, 16, 8}, {64, 8, 4}, {8, 4, 2}, {4, 2, 2}}};
    std::mt19937_64 rng(0x5eed0002ULL);
    for (const auto& shape : kShapes) {
        const uint256 sigma = RandomUint256(rng);
        const auto generated = qtc::metal::GenerateMatMulInputsGPU({
            .n = shape.n,
            .b = shape.b,
            .r = shape.r,
            .sigma = sigma,
        });
        BOOST_REQUIRE_MESSAGE(generated.success, generated.error);
        const auto cpu = matmul::noise::Generate(sigma, shape.n, shape.r);
        const size_t words = static_cast<size_t>(shape.n) * shape.r;
        CheckWordsEqual("E_L", generated.noise_e_l, cpu.E_L.data(), words);
        CheckWordsEqual("E_R", generated.noise_e_r, cpu.E_R.data(), words);
        CheckWordsEqual("F_L", generated.noise_f_l, cpu.F_L.data(), words);
        CheckWordsEqual("F_R", generated.noise_f_r, cpu.F_R.data(), words);
        BOOST_CHECK_EQUAL(generated.compress_vec.size(), static_cast<size_t>(shape.b) * shape.b);
    }
}

// --- product digest v4 ------------------------------------------------------

BOOST_AUTO_TEST_CASE(metal_pinned_product_digest_v4_vector)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        BOOST_TEST_MESSAGE("Skipping pinned v4 vector test: " << probe.reason);
        return;
    }

    // Feed the pinned A'/B' as base matrices with zero noise (r=1) so the device
    // hashes exactly the pinned C' = A'*B'.
    constexpr uint32_t kN = 8;
    constexpr uint32_t kB = 4;
    constexpr uint32_t kR = 1;
    const std::vector<matmul::field::Element> zero_noise(static_cast<size_t>(kN) * kR, 0);
    const uint256 sigma = ParseUint256(kPinnedV4SigmaHex);
    const auto result = qtc::metal::ComputeCanonicalTranscriptDigest({
        .n = kN,
        .b = kB,
        .r = kR,
        .sigma = sigma,
        .matrix_a = kPinnedV4APrime.data(),
        .matrix_b = kPinnedV4BPrime.data(),
        .noise_e_l = zero_noise.data(),
        .noise_e_r = zero_noise.data(),
        .noise_f_l = zero_noise.data(),
        .noise_f_r = zero_noise.data(),
    });
    BOOST_REQUIRE_MESSAGE(result.success, result.error);
    BOOST_CHECK_EQUAL(result.digest, ParseUint256(kPinnedV4DigestHex));

    matmul::Matrix a_prime(kN, kN);
    matmul::Matrix b_prime(kN, kN);
    std::copy(kPinnedV4APrime.begin(), kPinnedV4APrime.end(), a_prime.data());
    std::copy(kPinnedV4BPrime.begin(), kPinnedV4BPrime.end(), b_prime.data());
    BOOST_CHECK_EQUAL(
        result.digest,
        matmul::transcript::ComputeProductCommittedDigestFromPerturbed(a_prime, b_prime, kB, sigma));
}

BOOST_AUTO_TEST_CASE(metal_transcript_digest_mode_is_rejected_cleanly_by_direct_api)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    constexpr uint32_t kN = 8;
    constexpr uint32_t kB = 4;
    constexpr uint32_t kR = 2;
    const matmul::Matrix matrix_a(kN, kN);
    const matmul::Matrix matrix_b(kN, kN);
    const auto noise = matmul::noise::Generate(uint256{}, kN, kR);

    const auto submission = qtc::metal::SubmitCanonicalTranscriptDigest({
        .n = kN,
        .b = kB,
        .r = kR,
        .digest_mode = qtc::metal::MatMulDigestMode::TRANSCRIPT,
        .matrix_a = matrix_a.data(),
        .matrix_b = matrix_b.data(),
        .noise_e_l = noise.E_L.data(),
        .noise_e_r = noise.E_R.data(),
        .noise_f_l = noise.F_L.data(),
        .noise_f_r = noise.F_R.data(),
    });
    BOOST_CHECK_EQUAL(submission.available, probe.available);
    BOOST_CHECK(!submission.submitted);
    BOOST_CHECK(!submission.error.empty());
    if (probe.available) {
        BOOST_CHECK(submission.error.find("transcript") != std::string::npos);
    }

    const auto batch = qtc::metal::ComputeCanonicalTranscriptDigestBatch({
        .n = kN,
        .b = kB,
        .r = kR,
        .batch_size = 1,
        .digest_mode = qtc::metal::MatMulDigestMode::TRANSCRIPT,
    });
    BOOST_CHECK(!batch.success);
    BOOST_CHECK(!batch.error.empty());
}

BOOST_AUTO_TEST_CASE(metal_transcript_scheme_requests_fall_back_to_cpu_cleanly)
{
    // The legacy TRANSCRIPT scheme (pre-activation regtest heights) is never
    // served by Metal: the solver must report a clean CPU fallback with the
    // correct digest.
    constexpr std::array<uint32_t, 2> kDims{8, 16};
    for (const uint32_t n : kDims) {
        const uint32_t b = n / 2;
        const uint32_t r = n / 4;
        const CBlockHeader header = BuildHeader(n, 1000 + n);
        const matmul::Matrix matrix_a = matmul::FromSeed(header.seed_a, n);
        const matmul::Matrix matrix_b = matmul::FromSeed(header.seed_b, n);

        const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
            header, matrix_a, matrix_b, b, r, matmul::accelerated::DigestScheme::TRANSCRIPT);

        const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
            header, matrix_a, matrix_b, b, r,
            matmul::backend::Kind::METAL,
            matmul::accelerated::DigestScheme::TRANSCRIPT);

        BOOST_REQUIRE(digest_result.ok);
        BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);
        BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
        BOOST_CHECK(!digest_result.accelerated);
        BOOST_CHECK(digest_result.error.find("fallback_to_cpu") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(metal_product_digest_direct_api_matches_cpu_for_random_headers)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        BOOST_TEST_MESSAGE("Skipping Metal direct-API product digest parity test: " << probe.reason);
        return;
    }

    struct Shape {
        uint32_t n, b, r;
        const char* label;
    };
    constexpr std::array<Shape, 4> kShapes{{
        {512, 16, 8, "mainnet"},
        {64, 8, 4, "regtest"},
        {256, 8, 4, "testnet"},
        {8, 4, 2, "minimal"},
    }};
    std::mt19937_64 rng(0x5eed0003ULL);
    for (const auto& shape : kShapes) {
        for (int trial = 0; trial < 2; ++trial) {
            const CBlockHeader header = RandomHeader(rng, shape.n);
            const uint256 sigma = matmul::DeriveSigma(header);
            const matmul::Matrix matrix_a = matmul::FromSeed(header.seed_a, shape.n);
            const matmul::Matrix matrix_b = matmul::FromSeed(header.seed_b, shape.n);
            const auto noise = matmul::noise::Generate(sigma, shape.n, shape.r);
            const uint256 expected = ComputeReferenceProductDigest(header, matrix_a, matrix_b, shape.b, shape.r);

            const auto explicit_result = qtc::metal::ComputeCanonicalTranscriptDigest({
                .n = shape.n,
                .b = shape.b,
                .r = shape.r,
                .sigma = sigma,
                .matrix_a = matrix_a.data(),
                .matrix_b = matrix_b.data(),
                .noise_e_l = noise.E_L.data(),
                .noise_e_r = noise.E_R.data(),
                .noise_f_l = noise.F_L.data(),
                .noise_f_r = noise.F_R.data(),
            });
            BOOST_REQUIRE_MESSAGE(explicit_result.success, shape.label << ": " << explicit_result.error);
            BOOST_CHECK_EQUAL(explicit_result.digest, expected);

            const auto uploaded = qtc::metal::UploadBaseMatrices({
                .n = shape.n,
                .matrix_a = matrix_a.data(),
                .matrix_b = matrix_b.data(),
            });
            BOOST_REQUIRE_MESSAGE(uploaded.success, uploaded.error);
            const auto uploaded_result = qtc::metal::ComputeCanonicalTranscriptDigest({
                .n = shape.n,
                .b = shape.b,
                .r = shape.r,
                .sigma = sigma,
                .use_uploaded_base_matrices = true,
                .noise_e_l = noise.E_L.data(),
                .noise_e_r = noise.E_R.data(),
                .noise_f_l = noise.F_L.data(),
                .noise_f_r = noise.F_R.data(),
            });
            BOOST_REQUIRE_MESSAGE(uploaded_result.success, shape.label << ": " << uploaded_result.error);
            BOOST_CHECK_EQUAL(uploaded_result.digest, expected);

            const uint256 seeds_a[] = {header.seed_a};
            const uint256 seeds_b[] = {header.seed_b};
            const uint256 sigmas[] = {sigma};
            const matmul::field::Element* e_l[] = {noise.E_L.data()};
            const matmul::field::Element* e_r[] = {noise.E_R.data()};
            const matmul::field::Element* f_l[] = {noise.F_L.data()};
            const matmul::field::Element* f_r[] = {noise.F_R.data()};
            const auto variable_base = qtc::metal::ComputeCanonicalTranscriptDigestVariableBaseBatch({
                .n = shape.n,
                .b = shape.b,
                .r = shape.r,
                .batch_size = 1,
                .sigmas = sigmas,
                .matrix_a_seeds = seeds_a,
                .matrix_b_seeds = seeds_b,
                .noise_e_l = e_l,
                .noise_e_r = e_r,
                .noise_f_l = f_l,
                .noise_f_r = f_r,
            });
            BOOST_REQUIRE_MESSAGE(variable_base.success, shape.label << ": " << variable_base.error);
            BOOST_REQUIRE_EQUAL(variable_base.digests.size(), 1U);
            BOOST_CHECK_EQUAL(variable_base.digests[0], expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(metal_product_digest_via_solver_matches_cpu_across_supported_dimensions)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    constexpr std::array<uint32_t, 3> kDims{8, 16, 64};

    for (const uint32_t n : kDims) {
        const uint32_t b = n / 2 > 16 ? 8 : n / 2;
        const uint32_t r = n / 4 > 8 ? 4 : n / 4;
        const CBlockHeader header = BuildHeader(n, 10'000 + n);
        const matmul::Matrix matrix_a = matmul::FromSeed(header.seed_a, n);
        const matmul::Matrix matrix_b = matmul::FromSeed(header.seed_b, n);

        const uint256 cpu_digest = matmul::accelerated::ComputeMatMulDigestCPU(
            header, matrix_a, matrix_b, b, r, matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

        const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
            header, matrix_a, matrix_b, b, r,
            matmul::backend::Kind::METAL,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);

        BOOST_REQUIRE(digest_result.ok);
        BOOST_CHECK_EQUAL(digest_result.digest, ComputeReferenceProductDigest(header, matrix_a, matrix_b, b, r));
        BOOST_CHECK_EQUAL(digest_result.digest, cpu_digest);
        if (capability.available) {
            // A CPU fallback here would mean the solver is not routing v4
            // product digests to the (ported) Metal backend.
            BOOST_CHECK_MESSAGE(digest_result.backend == matmul::backend::Kind::METAL, digest_result.error);
            BOOST_CHECK(digest_result.accelerated);
            BOOST_CHECK(digest_result.error.empty());
        } else {
            BOOST_CHECK_EQUAL(digest_result.backend, matmul::backend::Kind::CPU);
        }
    }
}

BOOST_AUTO_TEST_CASE(metal_product_digest_via_solver_matches_cpu_for_random_mainnet_and_regtest_headers)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.available) {
        BOOST_TEST_MESSAGE("Skipping solver-routed Metal product digest parity test: " << capability.reason);
        return;
    }

    ScopedEnvVar specialization_env("QTC_MATMUL_METAL_FUNCTION_CONSTANTS", "auto");
    struct Shape {
        uint32_t n, b, r;
    };
    constexpr std::array<Shape, 2> kShapes{{{512, 16, 8}, {64, 8, 4}}};
    std::mt19937_64 rng(0x5eed0004ULL);
    for (const auto& shape : kShapes) {
        for (int trial = 0; trial < 3; ++trial) {
            const CBlockHeader header = RandomHeader(rng, shape.n);
            const matmul::Matrix matrix_a = matmul::FromSeed(header.seed_a, shape.n);
            const matmul::Matrix matrix_b = matmul::FromSeed(header.seed_b, shape.n);
            const uint256 expected = ComputeReferenceProductDigest(header, matrix_a, matrix_b, shape.b, shape.r);

            const auto digest_result = matmul::accelerated::ComputeMatMulDigest(
                header, matrix_a, matrix_b, shape.b, shape.r,
                matmul::backend::Kind::METAL,
                matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
            BOOST_REQUIRE_MESSAGE(digest_result.ok, digest_result.error);
            BOOST_CHECK_EQUAL(digest_result.digest, expected);
            BOOST_CHECK_MESSAGE(digest_result.backend == matmul::backend::Kind::METAL, digest_result.error);
            BOOST_CHECK(digest_result.accelerated);
        }
    }
}

BOOST_AUTO_TEST_CASE(metal_buffer_pool_reuses_for_repeated_dimension_digest_requests)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        const auto pool = qtc::metal::ProbeMatMulBufferPool();
        BOOST_CHECK(!pool.available);
        return;
    }

    constexpr uint32_t kN = 16;
    constexpr uint32_t kB = 8;
    constexpr uint32_t kR = 4;
    const matmul::Matrix matrix_a(kN, kN);
    const matmul::Matrix matrix_b(kN, kN);
    const uint256 sigma = ParseUint256("0f0e0d0c0b0a09080706050403020100000102030405060708090a0b0c0d0e0f");
    const auto noise = matmul::noise::Generate(sigma, kN, kR);

    const auto before = qtc::metal::ProbeMatMulBufferPool();
    const uint32_t requests = std::max<uint32_t>(2U, before.slot_count + 1U);
    for (uint32_t i = 0; i < requests; ++i) {
        const auto digest = qtc::metal::ComputeCanonicalTranscriptDigest({
            .n = kN,
            .b = kB,
            .r = kR,
            .sigma = sigma,
            .matrix_a = matrix_a.data(),
            .matrix_b = matrix_b.data(),
            .noise_e_l = noise.E_L.data(),
            .noise_e_r = noise.E_R.data(),
            .noise_f_l = noise.F_L.data(),
            .noise_f_r = noise.F_R.data(),
        });
        BOOST_REQUIRE(digest.success);
    }

    const auto after = qtc::metal::ProbeMatMulBufferPool();
    BOOST_CHECK(after.initialized);
    BOOST_CHECK_GE(after.allocation_events + after.reuse_events,
                   before.allocation_events + before.reuse_events + requests);
    BOOST_CHECK_LE(after.allocation_events, before.allocation_events + after.slot_count);
    BOOST_CHECK_GT(after.reuse_events, before.reuse_events);
}

BOOST_AUTO_TEST_CASE(metal_kernel_profile_and_profiling_report_runtime_values)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    const auto kernel = qtc::metal::ProbeMatMulKernelProfile();
    const auto profiling = qtc::metal::ProbeMatMulProfilingStats();

    BOOST_CHECK_EQUAL(kernel.available, probe.available);
    BOOST_CHECK_EQUAL(profiling.available, probe.available);
    if (!probe.available) {
        BOOST_CHECK(!kernel.reason.empty());
        BOOST_CHECK(!profiling.reason.empty());
        return;
    }

    BOOST_CHECK(kernel.tiled_build_prefix);
    BOOST_CHECK(kernel.fused_prefix_compress);
    BOOST_CHECK(kernel.gpu_transcript_hash);
    BOOST_CHECK(kernel.function_constant_specialization);
    BOOST_CHECK_GT(kernel.specialized_shape_count, 0U);
    BOOST_CHECK_GT(kernel.fused_prefix_threadgroup_threads, 0U);
    BOOST_CHECK(!kernel.specialization_reason.empty());
    BOOST_CHECK(kernel.cooperative_tensor_prepared);
    BOOST_CHECK(!kernel.cooperative_tensor_active);
    BOOST_CHECK(!kernel.uses_prefix_buffer);
    BOOST_CHECK(kernel.cooperative_tensor_reason.find("simdgroup_uint32_reduce") != std::string::npos);
    BOOST_CHECK(kernel.reason.find("product_digest_v4") != std::string::npos);
    BOOST_CHECK(!kernel.library_source.empty());
    BOOST_CHECK(!profiling.reason.empty());
}

BOOST_AUTO_TEST_CASE(metal_function_constant_policy_specializes_production_and_regtest_shapes_in_auto_mode)
{
    ScopedEnvVar specialization_env("QTC_MATMUL_METAL_FUNCTION_CONSTANTS", "auto");

    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.compiled) {
        BOOST_CHECK(!qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(/*n=*/512));
        BOOST_CHECK(!qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(/*n=*/256));
        return;
    }

    BOOST_CHECK(qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(/*n=*/512));
    BOOST_CHECK(qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(/*n=*/256));
    BOOST_CHECK(qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(/*n=*/64));
    BOOST_CHECK(!qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(/*n=*/128));
    // The legacy second argument is ignored since the v4 port.
    BOOST_CHECK_EQUAL(qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(512, true),
                      qtc::metal::ShouldUseFunctionConstantSpecializationPolicy(512, false));
}

BOOST_AUTO_TEST_CASE(metal_product_kernel_variants_agree_with_cpu)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        return;
    }

    constexpr uint32_t kN = 512;
    constexpr uint32_t kB = 16;
    constexpr uint32_t kR = 8;
    const CBlockHeader header = BuildHeader(kN, 77'000);
    const uint256 sigma = matmul::DeriveSigma(header);
    const matmul::Matrix matrix_a = matmul::FromSeed(header.seed_a, kN);
    const matmul::Matrix matrix_b = matmul::FromSeed(header.seed_b, kN);
    const auto noise = matmul::noise::Generate(sigma, kN, kR);
    const uint256 expected = ComputeReferenceProductDigest(header, matrix_a, matrix_b, kB, kR);

    for (const char* kernel_mode : {"simple", "tiled"}) {
        for (const char* fc_mode : {"0", "1"}) {
            ScopedEnvVar kernel_env("QTC_MATMUL_METAL_PRODUCT_KERNEL", kernel_mode);
            ScopedEnvVar fc_env("QTC_MATMUL_METAL_FUNCTION_CONSTANTS", fc_mode);
            const auto result = qtc::metal::ComputeCanonicalTranscriptDigest({
                .n = kN,
                .b = kB,
                .r = kR,
                .sigma = sigma,
                .matrix_a = matrix_a.data(),
                .matrix_b = matrix_b.data(),
                .noise_e_l = noise.E_L.data(),
                .noise_e_r = noise.E_R.data(),
                .noise_f_l = noise.F_L.data(),
                .noise_f_r = noise.F_R.data(),
            });
            BOOST_REQUIRE_MESSAGE(result.success, kernel_mode << "/" << fc_mode << ": " << result.error);
            BOOST_CHECK_MESSAGE(result.digest == expected, "kernel=" << kernel_mode << " fc=" << fc_mode);
        }
    }
}

BOOST_AUTO_TEST_CASE(metal_batch_digest_matches_single_digest_sequence_and_cpu)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        const auto batch = qtc::metal::ComputeCanonicalTranscriptDigestBatch({});
        BOOST_CHECK(!batch.available);
        BOOST_CHECK(!batch.success);
        return;
    }

    constexpr uint32_t kN = 16;
    constexpr uint32_t kB = 8;
    constexpr uint32_t kR = 4;
    constexpr uint32_t kBatchSize = 3;

    const matmul::Matrix matrix_a = matmul::FromSeed(
        ParseUint256("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        kN);
    const matmul::Matrix matrix_b = matmul::FromSeed(
        ParseUint256("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        kN);

    const auto uploaded = qtc::metal::UploadBaseMatrices({
        .n = kN,
        .matrix_a = matrix_a.data(),
        .matrix_b = matrix_b.data(),
    });
    BOOST_REQUIRE(uploaded.success);

    std::vector<matmul::noise::NoisePair> noises;
    std::vector<uint256> sigmas;
    std::vector<const matmul::field::Element*> noise_e_l_ptrs;
    std::vector<const matmul::field::Element*> noise_e_r_ptrs;
    std::vector<const matmul::field::Element*> noise_f_l_ptrs;
    std::vector<const matmul::field::Element*> noise_f_r_ptrs;
    std::vector<uint256> single_hashes;
    std::vector<uint256> cpu_hashes;

    noises.reserve(kBatchSize);
    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = BuildHeader(kN, 9'000 + i);
        const uint256 sigma = matmul::DeriveSigma(header);
        sigmas.push_back(sigma);
        noises.push_back(matmul::noise::Generate(sigma, kN, kR));
        const auto& noise = noises.back();
        noise_e_l_ptrs.push_back(noise.E_L.data());
        noise_e_r_ptrs.push_back(noise.E_R.data());
        noise_f_l_ptrs.push_back(noise.F_L.data());
        noise_f_r_ptrs.push_back(noise.F_R.data());
        cpu_hashes.push_back(matmul::transcript::ComputeProductCommittedDigestFromPerturbed(
            matrix_a + (noise.E_L * noise.E_R),
            matrix_b + (noise.F_L * noise.F_R),
            kB,
            sigma));

        const auto single = qtc::metal::ComputeCanonicalTranscriptDigest({
            .n = kN,
            .b = kB,
            .r = kR,
            .sigma = sigma,
            .use_uploaded_base_matrices = true,
            .noise_e_l = noise.E_L.data(),
            .noise_e_r = noise.E_R.data(),
            .noise_f_l = noise.F_L.data(),
            .noise_f_r = noise.F_R.data(),
        });
        BOOST_REQUIRE(single.success);
        single_hashes.push_back(single.digest);
    }

    const auto batch = qtc::metal::ComputeCanonicalTranscriptDigestBatch({
        .n = kN,
        .b = kB,
        .r = kR,
        .batch_size = kBatchSize,
        .sigmas = sigmas.data(),
        .use_uploaded_base_matrices = true,
        .noise_e_l = noise_e_l_ptrs.data(),
        .noise_e_r = noise_e_r_ptrs.data(),
        .noise_f_l = noise_f_l_ptrs.data(),
        .noise_f_r = noise_f_r_ptrs.data(),
    });
    BOOST_REQUIRE(batch.success);
    BOOST_REQUIRE_EQUAL(batch.digests.size(), kBatchSize);
    for (uint32_t i = 0; i < kBatchSize; ++i) {
        BOOST_CHECK(batch.digests[i] == single_hashes[i]);
        BOOST_CHECK_EQUAL(batch.digests[i], cpu_hashes[i]);
    }
}

BOOST_AUTO_TEST_CASE(metal_product_digest_batch_matches_single_digest_sequence)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.available) {
        BOOST_TEST_MESSAGE("Skipping prepared-batch Metal product digest test: Metal backend unavailable ("
            << capability.reason << ")");
        return;
    }

    constexpr uint32_t kN = 16;
    constexpr uint32_t kB = 8;
    constexpr uint32_t kR = 4;
    constexpr uint32_t kBatchSize = 3;

    const matmul::Matrix matrix_a = matmul::FromSeed(
        ParseUint256("abababababababababababababababababababababababababababababababab"),
        kN);
    const matmul::Matrix matrix_b = matmul::FromSeed(
        ParseUint256("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd"),
        kN);

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    std::vector<uint256> single_digests;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);
    single_digests.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        const CBlockHeader header = BuildHeader(kN, 20'000 + i);
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputs(
            header,
            kB,
            kR));
        single_digests.push_back(ComputeReferenceProductDigest(header, matrix_a, matrix_b, kB, kR));
    }

    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
        headers,
        matrix_a,
        matrix_b,
        kB,
        kR,
        prepared_inputs,
        matmul::backend::Kind::METAL,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        BOOST_REQUIRE(batch[i].ok);
        BOOST_CHECK_EQUAL(batch[i].digest, single_digests[i]);
        BOOST_CHECK_MESSAGE(batch[i].backend == matmul::backend::Kind::METAL, batch[i].error);
    }
}

BOOST_AUTO_TEST_CASE(metal_variable_base_product_digest_batch_matches_cpu)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.available) {
        BOOST_TEST_MESSAGE("Skipping variable-base Metal product digest batch test: Metal backend unavailable ("
            << capability.reason << ")");
        return;
    }

    constexpr uint32_t kN = 16;
    constexpr uint32_t kB = 8;
    constexpr uint32_t kR = 4;
    constexpr uint32_t kBatchSize = 3;
    const std::array<uint256, kBatchSize> seed_a{{
        ParseUint256("0101010101010101010101010101010101010101010101010101010101010101"),
        ParseUint256("0202020202020202020202020202020202020202020202020202020202020202"),
        ParseUint256("0303030303030303030303030303030303030303030303030303030303030303"),
    }};
    const std::array<uint256, kBatchSize> seed_b{{
        ParseUint256("1111111111111111111111111111111111111111111111111111111111111111"),
        ParseUint256("1212121212121212121212121212121212121212121212121212121212121212"),
        ParseUint256("1313131313131313131313131313131313131313131313131313131313131313"),
    }};

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    std::vector<uint256> cpu_digests;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);
    cpu_digests.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = BuildHeader(kN, 30'000 + i);
        header.seed_a = seed_a[i];
        header.seed_b = seed_b[i];
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputs(
            header,
            kB,
            kR));

        const matmul::Matrix matrix_a = matmul::FromSeed(header.seed_a, kN);
        const matmul::Matrix matrix_b = matmul::FromSeed(header.seed_b, kN);
        cpu_digests.push_back(ComputeReferenceProductDigest(header, matrix_a, matrix_b, kB, kR));
    }

    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedVariableBaseBatchForMining(
        headers,
        kB,
        kR,
        prepared_inputs,
        matmul::backend::Kind::METAL,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        BOOST_REQUIRE(batch[i].ok);
        BOOST_CHECK_EQUAL(batch[i].digest, cpu_digests[i]);
        BOOST_CHECK_MESSAGE(batch[i].backend == matmul::backend::Kind::METAL, batch[i].error);
    }
}

BOOST_AUTO_TEST_CASE(metal_nonce_seed_v2_mainnet_boundary_variable_base_product_digest_matches_cpu)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.available) {
        BOOST_TEST_MESSAGE("Skipping nonce-seed v2 Metal product digest batch test: Metal backend unavailable ("
            << capability.reason << ")");
        return;
    }

    ScopedEnvVar gpu_inputs_env("QTC_MATMUL_GPU_INPUTS", "1");
    ScopedEnvVar specialization_env("QTC_MATMUL_METAL_FUNCTION_CONSTANTS", "auto");

    constexpr uint32_t kN = 512;
    constexpr uint32_t kB = 16;
    constexpr uint32_t kR = 8;
    constexpr uint32_t kActivationHeight = 125'000;
    constexpr uint32_t kHeight125000NBits = 0x1d0b8746U;
    constexpr uint32_t kBatchSize = 2;

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    std::vector<uint256> cpu_digests;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);
    cpu_digests.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        CBlockHeader header = BuildHeader(kN, 125'000 + i);
        header.nVersion = 4;
        header.nBits = kHeight125000NBits;
        header.nTime = 1'773'277'390U + i;
        header.seed_a = DeterministicMatMulSeedV2(header, kActivationHeight, 0);
        header.seed_b = DeterministicMatMulSeedV2(header, kActivationHeight, 1);
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputsForBackend(
            header,
            kB,
            kR,
            matmul::backend::Kind::METAL,
            matmul::accelerated::DigestScheme::PRODUCT_COMMITTED));

        const matmul::Matrix matrix_a = matmul::FromSeed(header.seed_a, kN);
        const matmul::Matrix matrix_b = matmul::FromSeed(header.seed_b, kN);
        cpu_digests.push_back(ComputeReferenceProductDigest(header, matrix_a, matrix_b, kB, kR));
    }

    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedVariableBaseBatchForMining(
        headers,
        kB,
        kR,
        prepared_inputs,
        matmul::backend::Kind::METAL,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        BOOST_REQUIRE_MESSAGE(batch[i].ok, batch[i].error);
        BOOST_CHECK_EQUAL(batch[i].digest, cpu_digests[i]);
        BOOST_CHECK_MESSAGE(batch[i].backend == matmul::backend::Kind::METAL, batch[i].error);
        BOOST_CHECK(batch[i].accelerated);
    }
}

BOOST_AUTO_TEST_CASE(metal_base_matrix_from_nonce_seed_v2_matches_cpu_for_mainnet_shape)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.available) {
        BOOST_TEST_MESSAGE("Skipping nonce-seed v2 Metal base matrix generation test: Metal backend unavailable ("
            << capability.reason << ")");
        return;
    }

    constexpr uint32_t kN = 512;
    constexpr uint32_t kActivationHeight = 125'000;
    constexpr uint32_t kHeight125000NBits = 0x1d0b8746U;

    CBlockHeader header = BuildHeader(kN, 125'000);
    header.nVersion = 4;
    header.nBits = kHeight125000NBits;
    header.nTime = 1'773'277'390U;
    header.seed_a = DeterministicMatMulSeedV2(header, kActivationHeight, 0);
    header.seed_b = DeterministicMatMulSeedV2(header, kActivationHeight, 1);

    const matmul::Matrix cpu_matrix_a = matmul::FromSeed(header.seed_a, kN);
    const auto metal_matrix_a = qtc::metal::GenerateBaseMatrixFromSeedForTesting(kN, header.seed_a);
    BOOST_CHECK(metal_matrix_a.available);
    BOOST_REQUIRE_MESSAGE(metal_matrix_a.success, metal_matrix_a.error);
    CheckWordsEqual("matrix_a", metal_matrix_a.matrix, cpu_matrix_a.data(), static_cast<size_t>(kN) * kN);

    const matmul::Matrix cpu_matrix_b = matmul::FromSeed(header.seed_b, kN);
    const auto metal_matrix_b = qtc::metal::GenerateBaseMatrixFromSeedForTesting(kN, header.seed_b);
    BOOST_CHECK(metal_matrix_b.available);
    BOOST_REQUIRE_MESSAGE(metal_matrix_b.success, metal_matrix_b.error);
    CheckWordsEqual("matrix_b", metal_matrix_b.matrix, cpu_matrix_b.data(), static_cast<size_t>(kN) * kN);
}

BOOST_AUTO_TEST_CASE(metal_variable_base_intermediates_and_tile_hashes_match_cpu)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.available) {
        BOOST_TEST_MESSAGE("Skipping Metal intermediate diagnostic: Metal backend unavailable ("
            << capability.reason << ")");
        return;
    }

    ScopedEnvVar specialization_env("QTC_MATMUL_METAL_FUNCTION_CONSTANTS", "auto");

    constexpr uint32_t kN = 512;
    constexpr uint32_t kB = 16;
    constexpr uint32_t kR = 8;
    constexpr uint32_t kActivationHeight = 125'000;
    constexpr uint32_t kHeight125000NBits = 0x1d0b8746U;

    CBlockHeader header = BuildHeader(kN, 125'000);
    header.nVersion = 4;
    header.nBits = kHeight125000NBits;
    header.nTime = 1'773'277'390U;
    header.seed_a = DeterministicMatMulSeedV2(header, kActivationHeight, 0);
    header.seed_b = DeterministicMatMulSeedV2(header, kActivationHeight, 1);

    const uint256 sigma = matmul::DeriveSigma(header);
    const auto noise = matmul::noise::Generate(sigma, kN, kR);

    const matmul::Matrix cpu_matrix_a = matmul::FromSeed(header.seed_a, kN);
    const matmul::Matrix cpu_matrix_b = matmul::FromSeed(header.seed_b, kN);
    const matmul::Matrix cpu_a_prime = cpu_matrix_a + (noise.E_L * noise.E_R);
    const matmul::Matrix cpu_b_prime = cpu_matrix_b + (noise.F_L * noise.F_R);
    const matmul::Matrix cpu_c_prime = cpu_a_prime * cpu_b_prime;
    const auto cpu_tile_hashes = matmul::transcript::ComputeProductTileHashes(cpu_c_prime, kB);

    const auto metal = qtc::metal::GenerateVariableBaseProductForTesting({
        .n = kN,
        .b = kB,
        .r = kR,
        .matrix_a_seed = header.seed_a,
        .matrix_b_seed = header.seed_b,
        .noise_e_l = noise.E_L.data(),
        .noise_e_r = noise.E_R.data(),
        .noise_f_l = noise.F_L.data(),
        .noise_f_r = noise.F_R.data(),
    });
    BOOST_CHECK(metal.available);
    BOOST_REQUIRE_MESSAGE(metal.success, metal.error);

    const size_t words = static_cast<size_t>(kN) * kN;
    CheckWordsEqual("matrix_a", metal.matrix_a, cpu_matrix_a.data(), words);
    CheckWordsEqual("matrix_b", metal.matrix_b, cpu_matrix_b.data(), words);
    CheckWordsEqual("a_prime", metal.a_prime, cpu_a_prime.data(), words);
    CheckWordsEqual("b_prime", metal.b_prime, cpu_b_prime.data(), words);
    CheckWordsEqual("c_prime", metal.c_prime, cpu_c_prime.data(), words);
    BOOST_REQUIRE_EQUAL(metal.tile_hashes.size(), cpu_tile_hashes.size());
    BOOST_CHECK(metal.tile_hashes == cpu_tile_hashes);
    BOOST_CHECK_EQUAL(
        matmul::transcript::ComputeProductCommittedDigestFromTileHashes(
            Span<const uint256>{metal.tile_hashes.data(), metal.tile_hashes.size()}, sigma, kN, kB),
        matmul::transcript::ComputeProductCommittedDigest(cpu_c_prime, kB, sigma));
}

BOOST_AUTO_TEST_CASE(metal_mainnet_shape_product_digest_batch_matches_cpu_under_auto_policy)
{
    const auto capability = matmul::backend::CapabilityFor(matmul::backend::Kind::METAL);
    if (!capability.available) {
        BOOST_TEST_MESSAGE("Skipping mainnet-shape Metal product digest batch test: Metal backend unavailable ("
            << capability.reason << ")");
        return;
    }

    ScopedEnvVar specialization_env("QTC_MATMUL_METAL_FUNCTION_CONSTANTS", "auto");

    constexpr uint32_t kN = 512;
    constexpr uint32_t kB = 16;
    constexpr uint32_t kR = 8;
    constexpr uint32_t kBatchSize = 4;

    const matmul::Matrix matrix_a = matmul::FromSeed(
        ParseUint256("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"),
        kN);
    const matmul::Matrix matrix_b = matmul::FromSeed(
        ParseUint256("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"),
        kN);

    std::vector<CBlockHeader> headers;
    std::vector<matmul::accelerated::PreparedDigestInputs> prepared_inputs;
    std::vector<uint256> cpu_digests;
    headers.reserve(kBatchSize);
    prepared_inputs.reserve(kBatchSize);
    cpu_digests.reserve(kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        const CBlockHeader header = BuildHeader(kN, 40'000 + i);
        headers.push_back(header);
        prepared_inputs.push_back(matmul::accelerated::PrepareMatMulDigestInputs(
            header,
            kB,
            kR));
        cpu_digests.push_back(ComputeReferenceProductDigest(header, matrix_a, matrix_b, kB, kR));
    }

    const auto batch = matmul::accelerated::ComputeMatMulDigestPreparedBatch(
        headers,
        matrix_a,
        matrix_b,
        kB,
        kR,
        prepared_inputs,
        matmul::backend::Kind::METAL,
        matmul::accelerated::DigestScheme::PRODUCT_COMMITTED);
    BOOST_REQUIRE_EQUAL(batch.size(), kBatchSize);

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        BOOST_REQUIRE(batch[i].ok);
        BOOST_CHECK_EQUAL(batch[i].digest, cpu_digests[i]);
        BOOST_CHECK_MESSAGE(batch[i].backend == matmul::backend::Kind::METAL, batch[i].error);
    }
}

BOOST_AUTO_TEST_CASE(metal_batch_digest_reuses_staging_pool_for_repeated_requests)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        return;
    }

    constexpr uint32_t kN = 16;
    constexpr uint32_t kB = 8;
    constexpr uint32_t kR = 4;
    constexpr uint32_t kBatchSize = 4;

    const matmul::Matrix matrix_a = matmul::FromSeed(
        ParseUint256("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"),
        kN);
    const matmul::Matrix matrix_b = matmul::FromSeed(
        ParseUint256("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"),
        kN);

    const auto uploaded = qtc::metal::UploadBaseMatrices({
        .n = kN,
        .matrix_a = matrix_a.data(),
        .matrix_b = matrix_b.data(),
    });
    BOOST_REQUIRE(uploaded.success);

    std::vector<matmul::noise::NoisePair> noises;
    std::vector<uint256> sigmas;
    std::vector<const matmul::field::Element*> noise_e_l_ptrs;
    std::vector<const matmul::field::Element*> noise_e_r_ptrs;
    std::vector<const matmul::field::Element*> noise_f_l_ptrs;
    std::vector<const matmul::field::Element*> noise_f_r_ptrs;

    noises.reserve(kBatchSize);
    for (uint32_t i = 0; i < kBatchSize; ++i) {
        const CBlockHeader header = BuildHeader(kN, 12'000 + i);
        const uint256 sigma = matmul::DeriveSigma(header);
        sigmas.push_back(sigma);
        noises.push_back(matmul::noise::Generate(sigma, kN, kR));
        noise_e_l_ptrs.push_back(noises.back().E_L.data());
        noise_e_r_ptrs.push_back(noises.back().E_R.data());
        noise_f_l_ptrs.push_back(noises.back().F_L.data());
        noise_f_r_ptrs.push_back(noises.back().F_R.data());
    }

    const auto before = qtc::metal::ProbeMatMulBufferPool();
    const uint32_t requests = std::max<uint32_t>(2U, before.slot_count + 1U);
    for (uint32_t i = 0; i < requests; ++i) {
        const auto batch = qtc::metal::ComputeCanonicalTranscriptDigestBatch({
            .n = kN,
            .b = kB,
            .r = kR,
            .batch_size = kBatchSize,
            .sigmas = sigmas.data(),
            .use_uploaded_base_matrices = true,
            .noise_e_l = noise_e_l_ptrs.data(),
            .noise_e_r = noise_e_r_ptrs.data(),
            .noise_f_l = noise_f_l_ptrs.data(),
            .noise_f_r = noise_f_r_ptrs.data(),
        });
        BOOST_REQUIRE(batch.success);
    }

    const auto after = qtc::metal::ProbeMatMulBufferPool();
    BOOST_CHECK(after.initialized);
    BOOST_CHECK_GE(after.allocation_events + after.reuse_events,
                   before.allocation_events + before.reuse_events + requests);
    BOOST_CHECK_LE(after.allocation_events, before.allocation_events + after.slot_count);
    BOOST_CHECK_GT(after.reuse_events, before.reuse_events);
}

BOOST_AUTO_TEST_CASE(metal_concurrent_digest_requests_match_cpu_and_report_pool_contention)
{
    const auto probe = qtc::metal::ProbeMatMulDigestAcceleration();
    if (!probe.available) {
        return;
    }

    constexpr uint32_t kN = 512;
    constexpr uint32_t kB = 16;
    constexpr uint32_t kR = 8;
    constexpr size_t kThreads = 2;

    const matmul::Matrix matrix_a = matmul::FromSeed(
        ParseUint256("1111111111111111111111111111111111111111111111111111111111111111"),
        kN);
    const matmul::Matrix matrix_b = matmul::FromSeed(
        ParseUint256("2222222222222222222222222222222222222222222222222222222222222222"),
        kN);

    const auto uploaded = qtc::metal::UploadBaseMatrices({
        .n = kN,
        .matrix_a = matrix_a.data(),
        .matrix_b = matrix_b.data(),
    });
    BOOST_REQUIRE(uploaded.success);

    struct WorkerResult {
        bool success{false};
        uint256 digest;
        uint256 expected;
        std::string error;
    };

    std::array<CBlockHeader, kThreads> headers{
        BuildHeader(kN, 52'000),
        BuildHeader(kN, 52'001),
    };
    std::array<WorkerResult, kThreads> results;
    for (size_t i = 0; i < kThreads; ++i) {
        results[i].expected = ComputeReferenceProductDigest(headers[i], matrix_a, matrix_b, kB, kR);
    }

    const auto before = qtc::metal::ProbeMatMulBufferPool();
    ThreadGate gate(kThreads);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (size_t i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i] {
            const uint256 sigma = matmul::DeriveSigma(headers[i]);
            const auto noise = matmul::noise::Generate(sigma, kN, kR);
            gate.ArriveAndWait();

            const auto digest = qtc::metal::ComputeCanonicalTranscriptDigest({
                .n = kN,
                .b = kB,
                .r = kR,
                .sigma = sigma,
                .use_uploaded_base_matrices = true,
                .noise_e_l = noise.E_L.data(),
                .noise_e_r = noise.E_R.data(),
                .noise_f_l = noise.F_L.data(),
                .noise_f_r = noise.F_R.data(),
            });

            results[i].success = digest.success;
            results[i].digest = digest.digest;
            results[i].error = digest.error;
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    const auto after = qtc::metal::ProbeMatMulBufferPool();
    for (const auto& result : results) {
        BOOST_REQUIRE_MESSAGE(result.success, result.error);
        BOOST_CHECK_EQUAL(result.digest, result.expected);
    }
    BOOST_CHECK(after.initialized);
    BOOST_CHECK_GE(after.slot_count, 1U);
    BOOST_CHECK_EQUAL(after.active_slots, 0U);
    BOOST_CHECK_GE(after.reuse_events + after.allocation_events, before.reuse_events + before.allocation_events + 2);
    BOOST_CHECK_GE(after.high_water_slots, 1U);
    if (after.slot_count >= kThreads) {
        BOOST_CHECK_GE(after.high_water_slots, static_cast<uint32_t>(kThreads));
        BOOST_CHECK_EQUAL(after.wait_events, before.wait_events);
    } else {
        BOOST_CHECK_GT(after.wait_events, before.wait_events);
    }
}

// --- nonce prefilter tuner ---------------------------------------------------

BOOST_AUTO_TEST_CASE(metal_nonce_threshold_tuner_holds_when_pass_rate_in_target_window)
{
    const auto tuned = qtc::metal::TuneNoncePrefilterThreshold({
        .current_threshold = 4'000'000'000'000'000'000ULL,
        .batch_size = 1024,
        .observed_candidates = 64,
        .target_min_candidates = 48,
        .target_max_candidates = 80,
    });

    BOOST_CHECK(!tuned.adjusted);
    BOOST_CHECK_EQUAL(tuned.threshold, 4'000'000'000'000'000'000ULL);
}

BOOST_AUTO_TEST_CASE(metal_nonce_threshold_tuner_increases_when_pass_rate_too_low)
{
    const auto tuned = qtc::metal::TuneNoncePrefilterThreshold({
        .current_threshold = 1'000'000'000'000'000'000ULL,
        .batch_size = 4096,
        .observed_candidates = 8,
        .target_min_candidates = 128,
        .target_max_candidates = 256,
    });

    BOOST_CHECK(tuned.adjusted);
    BOOST_CHECK_GT(tuned.threshold, 1'000'000'000'000'000'000ULL);
}

BOOST_AUTO_TEST_CASE(metal_nonce_threshold_tuner_decreases_when_pass_rate_too_high)
{
    const auto tuned = qtc::metal::TuneNoncePrefilterThreshold({
        .current_threshold = 8'000'000'000'000'000'000ULL,
        .batch_size = 4096,
        .observed_candidates = 2048,
        .target_min_candidates = 128,
        .target_max_candidates = 256,
    });

    BOOST_CHECK(tuned.adjusted);
    BOOST_CHECK_LT(tuned.threshold, 8'000'000'000'000'000'000ULL);
}

BOOST_AUTO_TEST_CASE(metal_nonce_threshold_tuner_handles_zero_candidates_without_overflow)
{
    const auto tuned = qtc::metal::TuneNoncePrefilterThreshold({
        .current_threshold = 0,
        .batch_size = 1024,
        .observed_candidates = 0,
        .target_min_candidates = 16,
        .target_max_candidates = 32,
    });

    BOOST_CHECK(tuned.adjusted);
    BOOST_CHECK_GT(tuned.threshold, 0U);
}

// -- Property tests for the QTC_MATMUL_METAL_* env-var parsers --------------
//
// These cover the parsing rules for QTC_MATMUL_METAL_PIPELINE,
// QTC_MATMUL_METAL_FUNCTION_CONSTANTS, QTC_MATMUL_METAL_POOL_SLOTS, and the
// generic truthy/falsy helper used by QTC_MATMUL_METAL_POOL_PREWARM. They run
// unconditionally (no Metal device required) because the parsers are pure
// functions in qtc::metal::detail; they do not touch process-global env
// state, so they are safe to interleave with other tests.

BOOST_AUTO_TEST_CASE(metal_env_parse_transcript_pipeline_known_tokens)
{
    using qtc::metal::detail::ParseTranscriptPipelineEnv;
    using qtc::metal::detail::TranscriptPipelineMode;

    BOOST_CHECK(ParseTranscriptPipelineEnv(nullptr) == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("auto") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("fused") == TranscriptPipelineMode::FUSED);
    BOOST_CHECK(ParseTranscriptPipelineEnv("legacy") == TranscriptPipelineMode::LEGACY);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_transcript_pipeline_unknown_tokens_fall_back_to_auto)
{
    using qtc::metal::detail::ParseTranscriptPipelineEnv;
    using qtc::metal::detail::TranscriptPipelineMode;

    // Token recognition is case-sensitive and exact. Anything else is AUTO.
    BOOST_CHECK(ParseTranscriptPipelineEnv("AUTO") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("Auto") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("FUSED") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("Legacy") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv(" auto") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("auto ") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("fused;legacy") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("garbage") == TranscriptPipelineMode::AUTO);
    BOOST_CHECK(ParseTranscriptPipelineEnv("1") == TranscriptPipelineMode::AUTO);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_function_constant_known_tokens)
{
    using qtc::metal::detail::FunctionConstantMode;
    using qtc::metal::detail::ParseFunctionConstantEnv;

    BOOST_CHECK(ParseFunctionConstantEnv(nullptr) == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("auto") == FunctionConstantMode::AUTO);

    // Truthy enable tokens.
    BOOST_CHECK(ParseFunctionConstantEnv("1") == FunctionConstantMode::ENABLED);
    BOOST_CHECK(ParseFunctionConstantEnv("on") == FunctionConstantMode::ENABLED);
    BOOST_CHECK(ParseFunctionConstantEnv("true") == FunctionConstantMode::ENABLED);

    // Falsy disable tokens.
    BOOST_CHECK(ParseFunctionConstantEnv("0") == FunctionConstantMode::DISABLED);
    BOOST_CHECK(ParseFunctionConstantEnv("off") == FunctionConstantMode::DISABLED);
    BOOST_CHECK(ParseFunctionConstantEnv("false") == FunctionConstantMode::DISABLED);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_function_constant_unknown_tokens_fall_back_to_auto)
{
    using qtc::metal::detail::FunctionConstantMode;
    using qtc::metal::detail::ParseFunctionConstantEnv;

    // Recognition is case-sensitive: uppercase variants are NOT recognised
    // (matches the original inline parser's behaviour); they fall back to AUTO.
    BOOST_CHECK(ParseFunctionConstantEnv("TRUE") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("FALSE") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("ON") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("OFF") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("yes") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("no") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("2") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv(" 1") == FunctionConstantMode::AUTO);
    BOOST_CHECK(ParseFunctionConstantEnv("1 ") == FunctionConstantMode::AUTO);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_truthy_recognises_falsy_tokens)
{
    using qtc::metal::detail::ParseTruthyEnv;

    // Null and empty fall back to default_value (both polarities verified).
    BOOST_CHECK(ParseTruthyEnv(nullptr, true) == true);
    BOOST_CHECK(ParseTruthyEnv(nullptr, false) == false);
    BOOST_CHECK(ParseTruthyEnv("", true) == true);
    BOOST_CHECK(ParseTruthyEnv("", false) == false);

    // Recognised falsy tokens override default_value.
    BOOST_CHECK(ParseTruthyEnv("0", true) == false);
    BOOST_CHECK(ParseTruthyEnv("false", true) == false);
    BOOST_CHECK(ParseTruthyEnv("FALSE", true) == false);
    BOOST_CHECK(ParseTruthyEnv("off", true) == false);
    BOOST_CHECK(ParseTruthyEnv("OFF", true) == false);

    // Anything non-empty and not a recognised falsy token is truthy.
    // This is intentional — see the helper's docstring — so set values
    // such as "1", "yes", "on", "True", "FaLSe" all enable the feature
    // even though they are not all idiomatic. Capturing this in tests
    // pins the behaviour against accidental tightening.
    BOOST_CHECK(ParseTruthyEnv("1", false) == true);
    BOOST_CHECK(ParseTruthyEnv("on", false) == true);
    BOOST_CHECK(ParseTruthyEnv("true", false) == true);
    BOOST_CHECK(ParseTruthyEnv("yes", false) == true);
    BOOST_CHECK(ParseTruthyEnv("True", false) == true);
    BOOST_CHECK(ParseTruthyEnv("FaLSe", false) == true);
    BOOST_CHECK(ParseTruthyEnv("anything-not-recognised", false) == true);
    BOOST_CHECK(ParseTruthyEnv(" 0", false) == true); // leading space → not '0' literal
    BOOST_CHECK(ParseTruthyEnv("0 ", false) == true); // trailing space → not '0' literal
}

BOOST_AUTO_TEST_CASE(metal_env_parse_pool_slots_unset_returns_nullopt)
{
    using qtc::metal::detail::ParsePoolSlotsEnv;

    // Null and empty mean "variable unset" — caller should auto-detect.
    BOOST_CHECK(!ParsePoolSlotsEnv(nullptr, /*max_slots=*/16, /*default_fallback=*/4).has_value());
    BOOST_CHECK(!ParsePoolSlotsEnv("", 16, 4).has_value());
}

BOOST_AUTO_TEST_CASE(metal_env_parse_pool_slots_valid_inputs_clamp_to_range)
{
    using qtc::metal::detail::ParsePoolSlotsEnv;
    constexpr uint32_t kMax = 16;
    constexpr uint32_t kDefault = 5;

    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("1", kMax, kDefault).value(), 1U);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("5", kMax, kDefault).value(), 5U);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("16", kMax, kDefault).value(), 16U);

    // Above max → clamped to max.
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("17", kMax, kDefault).value(), 16U);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("1000", kMax, kDefault).value(), 16U);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("2147483647", kMax, kDefault).value(), 16U);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_pool_slots_non_positive_falls_back_to_default)
{
    using qtc::metal::detail::ParsePoolSlotsEnv;
    constexpr uint32_t kMax = 16;
    constexpr uint32_t kDefault = 5;

    // Zero and negatives are treated as malformed — fall back to default.
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("0", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("-1", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("-1000000", kMax, kDefault).value(), kDefault);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_pool_slots_malformed_inputs_fall_back_to_default)
{
    using qtc::metal::detail::ParsePoolSlotsEnv;
    constexpr uint32_t kMax = 16;
    constexpr uint32_t kDefault = 5;

    // Pure non-digits.
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("abc", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("!@#", kMax, kDefault).value(), kDefault);

    // Trailing garbage after a digit run is rejected (matches std::strtol
    // strict-tail check in the original parser).
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("4x", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("4 ", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("4\n", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("4.0", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("4,000", kMax, kDefault).value(), kDefault);

    // Hex and octal-looking inputs: strtol with base 10 accepts a leading 0
    // (parsing as decimal 4) and rejects 0x... at the strict-tail check.
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("0x4", kMax, kDefault).value(), kDefault);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("04", kMax, kDefault).value(), 4U);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_pool_slots_overflow_clamps_to_max)
{
    using qtc::metal::detail::ParsePoolSlotsEnv;
    constexpr uint32_t kMax = 16;
    constexpr uint32_t kDefault = 5;

    // Beyond LONG_MAX, std::strtol saturates at LONG_MAX. The original inline
    // parser then clamped that oversized positive value down to max_slots.
    BOOST_CHECK_EQUAL(
        ParsePoolSlotsEnv("999999999999999999999999999999", kMax, kDefault).value(),
        kMax);
}

BOOST_AUTO_TEST_CASE(metal_env_parse_pool_slots_clamps_default_fallback_into_range)
{
    using qtc::metal::detail::ParsePoolSlotsEnv;

    // If the malformed-input path is taken, the default_fallback itself is
    // clamped to [1, max_slots] before it is returned. This protects against
    // an obviously-invalid default_fallback value coming from a future
    // refactor.
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("bad", /*max=*/8, /*default=*/0).value(), 1U);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("bad", /*max=*/8, /*default=*/100).value(), 8U);
    BOOST_CHECK_EQUAL(ParsePoolSlotsEnv("bad", /*max=*/8, /*default=*/4).value(), 4U);
}

BOOST_AUTO_TEST_SUITE_END()
