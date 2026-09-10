// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <matmul/noise.h>
#include <matmul/solver_runtime.h>

#include <random.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

namespace {

uint256 ParseUint256(std::string_view hex)
{
    const auto parsed = uint256::FromHex(hex);
    BOOST_REQUIRE(parsed.has_value());
    return *parsed;
}

bool MatrixElementsInField(const matmul::Matrix& m)
{
    for (uint32_t r = 0; r < m.rows(); ++r) {
        for (uint32_t c = 0; c < m.cols(); ++c) {
            if (m.at(r, c) >= matmul::field::MODULUS) {
                return false;
            }
        }
    }
    return true;
}

uint32_t MatrixRank(const matmul::Matrix& m)
{
    const uint32_t rows = m.rows();
    const uint32_t cols = m.cols();

    std::vector<std::vector<matmul::field::Element>> a(rows, std::vector<matmul::field::Element>(cols));
    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            a[r][c] = m.at(r, c);
        }
    }

    uint32_t rank = 0;
    for (uint32_t col = 0; col < cols && rank < rows; ++col) {
        uint32_t pivot = rank;
        while (pivot < rows && a[pivot][col] == 0) {
            ++pivot;
        }
        if (pivot == rows) {
            continue;
        }

        if (pivot != rank) {
            std::swap(a[pivot], a[rank]);
        }

        const auto inv_pivot = matmul::field::inv(a[rank][col]);
        for (uint32_t c = col; c < cols; ++c) {
            a[rank][c] = matmul::field::mul(a[rank][c], inv_pivot);
        }

        for (uint32_t r = 0; r < rows; ++r) {
            if (r == rank || a[r][col] == 0) continue;
            const auto factor = a[r][col];
            for (uint32_t c = col; c < cols; ++c) {
                const auto term = matmul::field::mul(factor, a[rank][c]);
                a[r][c] = matmul::field::sub(a[r][c], term);
            }
        }

        ++rank;
    }

    return rank;
}

bool MatrixEqualsExpected(const matmul::Matrix& m, const std::vector<std::vector<uint32_t>>& expected)
{
    if (m.rows() != expected.size()) return false;
    if (m.rows() == 0) return true;
    if (m.cols() != expected[0].size()) return false;

    for (uint32_t r = 0; r < m.rows(); ++r) {
        for (uint32_t c = 0; c < m.cols(); ++c) {
            if (m.at(r, c) != expected[r][c]) {
                return false;
            }
        }
    }
    return true;
}

class ScopedEnvVar
{
public:
    ScopedEnvVar(const char* name, const char* value)
        : m_name{name}
    {
        const char* current = std::getenv(name);
        if (current != nullptr) {
            m_previous = current;
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
        _putenv_s(m_name, m_previous.has_value() ? m_previous->c_str() : "");
#else
        if (m_previous.has_value()) {
            setenv(m_name, m_previous->c_str(), 1);
        } else {
            unsetenv(m_name);
        }
#endif
    }

private:
    const char* m_name;
    std::optional<std::string> m_previous;
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(matmul_noise_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(noise_deterministic)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const auto a = matmul::noise::Generate(sigma, 8, 2);
    const auto b = matmul::noise::Generate(sigma, 8, 2);

    BOOST_CHECK(a.E_L == b.E_L);
    BOOST_CHECK(a.E_R == b.E_R);
    BOOST_CHECK(a.F_L == b.F_L);
    BOOST_CHECK(a.F_R == b.F_R);
}

BOOST_AUTO_TEST_CASE(noise_rank_bounded)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    constexpr uint32_t n = 8;
    constexpr uint32_t r = 2;

    const auto pair = matmul::noise::Generate(sigma, n, r);
    const matmul::Matrix product = pair.E_L * pair.E_R;

    BOOST_CHECK_LE(MatrixRank(product), r);
}

BOOST_AUTO_TEST_CASE(noise_elements_in_field)
{
    FastRandomContext rng{true};
    const auto pair = matmul::noise::Generate(rng.rand256(), 16, 4);

    BOOST_CHECK(MatrixElementsInField(pair.E_L));
    BOOST_CHECK(MatrixElementsInField(pair.E_R));
    BOOST_CHECK(MatrixElementsInField(pair.F_L));
    BOOST_CHECK(MatrixElementsInField(pair.F_R));
}

BOOST_AUTO_TEST_CASE(noise_different_sigma_differ)
{
    const uint256 sigma_a = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const uint256 sigma_b = ParseUint256("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    const auto a = matmul::noise::Generate(sigma_a, 8, 2);
    const auto b = matmul::noise::Generate(sigma_b, 8, 2);

    BOOST_CHECK(!(a.E_L == b.E_L));
    BOOST_CHECK(!(a.E_R == b.E_R));
}

BOOST_AUTO_TEST_CASE(noise_generation_profile_matches_platform_capability)
{
    const auto profile = matmul::noise::ProbeNoiseGenerationProfile(512, 8);
    BOOST_CHECK_GT(profile.worker_count, 0U);
    BOOST_CHECK(!profile.reason.empty());
#if defined(__APPLE__)
    BOOST_CHECK(profile.parallel_supported);
#else
    BOOST_CHECK(!profile.parallel_supported);
    BOOST_CHECK_EQUAL(profile.worker_count, 1U);
#endif
}

BOOST_AUTO_TEST_CASE(noise_generation_profile_serial_for_small_footprint_on_apple)
{
#if defined(__APPLE__)
    const auto profile = matmul::noise::ProbeNoiseGenerationProfile(512, 8);
    BOOST_CHECK_EQUAL(profile.worker_count, 1U);
#endif
}

BOOST_AUTO_TEST_CASE(noise_generation_profile_respects_solve_runtime_worker_cap)
{
#if defined(__APPLE__)
    const ScopedEnvVar force_parallel{"QTC_MATMUL_NOISE_PARALLEL", "1"};
    const matmul::ScopedSolveRuntime runtime{
        {.time_budget_ms = 0, .max_worker_threads = 2}};
    const auto profile = matmul::noise::ProbeNoiseGenerationProfile(1024, 1024);
    BOOST_CHECK(profile.parallel_supported);
    BOOST_CHECK_EQUAL(profile.reason, "parallel_forced_on");
    BOOST_CHECK_GT(profile.worker_count, 0U);
    BOOST_CHECK_LE(profile.worker_count, 2U);
#endif
}

BOOST_AUTO_TEST_CASE(noise_r_independent_of_b)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const auto pair = matmul::noise::Generate(sigma, 8, 8);

    BOOST_CHECK_EQUAL(pair.E_L.rows(), 8U);
    BOOST_CHECK_EQUAL(pair.E_L.cols(), 8U);
    BOOST_CHECK_EQUAL(pair.E_R.rows(), 8U);
    BOOST_CHECK_EQUAL(pair.E_R.cols(), 8U);
}

BOOST_AUTO_TEST_CASE(noise_derived_seed_pinned_EL)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const uint256 seed = matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_EL, sigma);
    BOOST_CHECK_EQUAL(seed.GetHex(), "993a427eeb3dc053000d570842d2e7f0f093393c00e8e729155c48719118b386");
}

BOOST_AUTO_TEST_CASE(noise_derived_seed_pinned_ER)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const uint256 seed = matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_ER, sigma);
    BOOST_CHECK_EQUAL(seed.GetHex(), "0b3b1aa329a9ee863b3aa0080346e4ced9842b39db47d70418af99120b6530a2");
}

BOOST_AUTO_TEST_CASE(noise_derived_seed_pinned_FL)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const uint256 seed = matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FL, sigma);
    BOOST_CHECK_EQUAL(seed.GetHex(), "73ff6f6817e0c7e7ce9219076b14f1d932be70c641393bfc4c53a230bf65ddd8");
}

BOOST_AUTO_TEST_CASE(noise_derived_seed_pinned_FR)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const uint256 seed = matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FR, sigma);
    BOOST_CHECK_EQUAL(seed.GetHex(), "91d399ff912ea452af750501448661096d5251cd17921403ab70d0c4561b45a3");
}

BOOST_AUTO_TEST_CASE(noise_domain_separation_all_seeds_distinct)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");

    BOOST_CHECK_EQUAL(matmul::noise::TAG_EL.size(), 18U);
    BOOST_CHECK_EQUAL(matmul::noise::TAG_ER.size(), 18U);
    BOOST_CHECK_EQUAL(matmul::noise::TAG_FL.size(), 18U);
    BOOST_CHECK_EQUAL(matmul::noise::TAG_FR.size(), 18U);

    std::set<uint256> seeds{
        matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_EL, sigma),
        matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_ER, sigma),
        matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FL, sigma),
        matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_FR, sigma),
    };

    BOOST_CHECK_EQUAL(seeds.size(), 4U);
}

BOOST_AUTO_TEST_CASE(noise_EL_pinned_elements)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const auto pair = matmul::noise::Generate(sigma, 4, 2);

    const std::vector<std::vector<uint32_t>> expected{
        {1931902215U, 1595657157U},
        {196402182U, 2100142100U},
        {545335226U, 850560975U},
        {1393477226U, 445857985U},
    };

    BOOST_CHECK(MatrixEqualsExpected(pair.E_L, expected));
}

BOOST_AUTO_TEST_CASE(noise_ER_pinned_elements)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const auto pair = matmul::noise::Generate(sigma, 4, 2);

    const std::vector<std::vector<uint32_t>> expected{
        {962405871U, 1446915036U, 488189162U, 647563697U},
        {1646275947U, 1520400424U, 1495996857U, 530077483U},
    };

    BOOST_CHECK(MatrixEqualsExpected(pair.E_R, expected));
}

BOOST_AUTO_TEST_CASE(noise_first_element_domain_separation)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const auto pair = matmul::noise::Generate(sigma, 4, 2);

    BOOST_CHECK_EQUAL(pair.E_L.at(0, 0), 1931902215U);
    BOOST_CHECK_EQUAL(pair.E_R.at(0, 0), 962405871U);
    BOOST_CHECK_EQUAL(pair.F_L.at(0, 0), 1766706109U);
    BOOST_CHECK_EQUAL(pair.F_R.at(0, 0), 1500561682U);

    std::set<uint32_t> first_elements{
        pair.E_L.at(0, 0),
        pair.E_R.at(0, 0),
        pair.F_L.at(0, 0),
        pair.F_R.at(0, 0),
    };
    BOOST_CHECK_EQUAL(first_elements.size(), 4U);
}

BOOST_AUTO_TEST_CASE(noise_index_uses_factor_column_count)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const auto pair = matmul::noise::Generate(sigma, 8, 2);

    const uint256 tag_el = matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_EL, sigma);
    const uint256 tag_er = matmul::noise::DeriveNoiseSeed(matmul::noise::TAG_ER, sigma);

    BOOST_CHECK_EQUAL(pair.E_L.at(3, 1), matmul::field::from_oracle(tag_el, 3 * 2 + 1));
    BOOST_CHECK_EQUAL(pair.E_R.at(1, 3), matmul::field::from_oracle(tag_er, 1 * 8 + 3));
}

BOOST_AUTO_TEST_CASE(noise_all_elements_in_field)
{
    FastRandomContext rng{true};
    const auto pair = matmul::noise::Generate(rng.rand256(), 64, 4);

    BOOST_CHECK(MatrixElementsInField(pair.E_L));
    BOOST_CHECK(MatrixElementsInField(pair.E_R));
    BOOST_CHECK(MatrixElementsInField(pair.F_L));
    BOOST_CHECK(MatrixElementsInField(pair.F_R));
}

// TEST: cross_platform_cuda_vs_cpu
// TEST: cross_platform_cuda_vs_metal
// TEST: cross_platform_metal_vs_cpu
BOOST_AUTO_TEST_CASE(noise_cross_platform_consistency)
{
    const uint256 sigma = ParseUint256("0000000000000000000000000000000000000000000000000000000000000000");
    const auto pair = matmul::noise::Generate(sigma, 8, 2);

    const std::vector<std::vector<uint32_t>> expected_el{
        {1931902215U, 1595657157U},
        {196402182U, 2100142100U},
        {545335226U, 850560975U},
        {1393477226U, 445857985U},
        {129748845U, 1050096989U},
        {1525789677U, 803773653U},
        {1059107676U, 879417578U},
        {1048684237U, 1593149007U},
    };
    const std::vector<std::vector<uint32_t>> expected_er{
        {962405871U, 1446915036U, 488189162U, 647563697U, 1646275947U, 1520400424U, 1495996857U, 530077483U},
        {1142251768U, 828648144U, 663895780U, 1033321960U, 767768131U, 271550251U, 174992716U, 944385125U},
    };
    const std::vector<std::vector<uint32_t>> expected_fl{
        {1766706109U, 378279764U},
        {1766056347U, 1039169922U},
        {971949704U, 458374902U},
        {1170543340U, 1037688433U},
        {900565569U, 215411276U},
        {1944198153U, 164101153U},
        {1253053965U, 1019691754U},
        {1980460U, 1595350917U},
    };
    const std::vector<std::vector<uint32_t>> expected_fr{
        {1500561682U, 599859230U, 408303329U, 1473672044U, 745479985U, 1539019890U, 1702083855U, 80765561U},
        {1998156436U, 282284786U, 827777684U, 1285974698U, 1750974632U, 1377467251U, 1193428297U, 1240276054U},
    };

    BOOST_CHECK(MatrixEqualsExpected(pair.E_L, expected_el));
    BOOST_CHECK(MatrixEqualsExpected(pair.E_R, expected_er));
    BOOST_CHECK(MatrixEqualsExpected(pair.F_L, expected_fl));
    BOOST_CHECK(MatrixEqualsExpected(pair.F_R, expected_fr));
}

BOOST_AUTO_TEST_SUITE_END()
