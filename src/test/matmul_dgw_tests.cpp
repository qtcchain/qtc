// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

arith_uint256 DecodeTarget(uint32_t nbits)
{
    bool negative{false};
    bool overflow{false};
    arith_uint256 target{};
    target.SetCompact(nbits, &negative, &overflow);
    BOOST_REQUIRE(!negative);
    BOOST_REQUIRE(!overflow);
    BOOST_REQUIRE(target > 0);
    return target;
}

// DESIGN INVARIANT: MatMul networks use ASERT exclusively for difficulty
// adjustment. DGW is NOT used for MatMul mining. nMatMulAsertHeight must
// equal nFastMineHeight. Do not reintroduce DGW routing.
Consensus::Params MatMulRetargetParams()
{
    auto params = CreateChainParams(ArgsManager{}, ChainType::REGTEST)->GetConsensus();
    params.fMatMulPOW = true;
    params.fPowNoRetargeting = false;
    params.fPowAllowMinDifficultyBlocks = false;
    params.nPowTargetSpacingFastMs = 90'000;
    params.nPowTargetSpacingNormal = 90;
    params.nFastMineDifficultyScale = 1;
    params.nFastMineHeight = 0;
    // ASERT activates at nFastMineHeight (no DGW for MatMul).
    params.nMatMulAsertHeight = 0;
    params.nMatMulAsertHalfLife = 14'400;
    params.nMatMulAsertBootstrapFactor = 1;
    return params;
}

void SeedFixedDifficultyChain(
    std::vector<CBlockIndex>& blocks,
    uint32_t nbits,
    int64_t start_time,
    int64_t spacing)
{
    for (size_t i = 0; i < blocks.size(); ++i) {
        blocks[i].nHeight = static_cast<int>(i);
        blocks[i].nBits = nbits;
        blocks[i].nTime = start_time + static_cast<int64_t>(i) * spacing;
        blocks[i].pprev = (i == 0) ? nullptr : &blocks[i - 1];
    }
}

void AppendSimulatedBlock(
    const Consensus::Params& params,
    std::vector<CBlockIndex>& blocks,
    int height,
    int64_t spacing)
{
    CBlockIndex& prev = blocks[height - 1];
    CBlockHeader next{};
    next.nTime = prev.GetBlockTime() + spacing;

    CBlockIndex& current = blocks[height];
    current.nHeight = height;
    current.nBits = GetNextWorkRequired(&prev, &next, params);
    current.nTime = next.nTime;
    current.pprev = &prev;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(matmul_dgw_tests, BasicTestingSetup)

// --- Legacy ExpectedDgwTimespan tests (function still exists for KAWPOW) ---

BOOST_AUTO_TEST_CASE(dgw_fast_phase_targets_250ms)
{
    // QTC Option B: mainnet has no fast phase, so height 500 already uses the
    // normal timespan; the legacy 250 ms schedule is exercised on a copy.
    auto params = CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus();
    BOOST_REQUIRE_EQUAL(params.nFastMineHeight, 0);
    BOOST_CHECK_EQUAL(ExpectedDgwTimespan(500, params), 108'000 /* 180 intervals x 600 s */);
    params.nFastMineHeight = 50'000;
    BOOST_CHECK_EQUAL(ExpectedDgwTimespan(500, params), 45);
}

BOOST_AUTO_TEST_CASE(dgw_post_phase_targets_90s)
{
    const auto params = CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus();
    // With nDgwWindowAlignmentHeight disabled (max), interval_count = 180.
    BOOST_CHECK_EQUAL(ExpectedDgwTimespan(60'000, params), 108'000 /* 180 intervals x 600 s */);
}

BOOST_AUTO_TEST_CASE(dgw_boundary_targets_normal_timespan)
{
    const auto params = CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus();
    BOOST_CHECK_EQUAL(ExpectedDgwTimespan(50'090, params), 108'000 /* 180 intervals x 600 s */);
}

BOOST_AUTO_TEST_CASE(dgw_boundary_transition_start)
{
    const auto params = CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus();
    BOOST_CHECK_EQUAL(ExpectedDgwTimespan(50'000, params), 108'000 /* 180 intervals x 600 s */);
}

BOOST_AUTO_TEST_CASE(dgw_boundary_transition_end)
{
    const auto params = CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus();
    BOOST_CHECK_EQUAL(ExpectedDgwTimespan(50'180, params), 108'000 /* 180 intervals x 600 s */);
}

// --- Genesis nBits sanity checks ---

BOOST_AUTO_TEST_CASE(mainnet_genesis_nbits_equals_powlimit_compact)
{
    // QTC Option B: powLimit is the hard floor and the genesis anchor. Genesis
    // nBits MUST equal compact(powLimit): consensus would clamp a looser anchor,
    // but rpc GetTarget (CHECK_NONFATAL DeriveTarget) aborts on block 0.
    const auto chain_params = CreateChainParams(ArgsManager{}, ChainType::MAIN);
    const auto& params = chain_params->GetConsensus();
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    const uint32_t mainnet_genesis_nbits = chain_params->GenesisBlock().nBits;

    BOOST_CHECK_EQUAL(pow_limit.GetCompact(), mainnet_genesis_nbits);
    BOOST_CHECK_EQUAL(mainnet_genesis_nbits, 0x1e033333U);

    arith_uint256 genesis_target{};
    genesis_target.SetCompact(mainnet_genesis_nbits);
    BOOST_CHECK(genesis_target == pow_limit);
    BOOST_CHECK(DeriveTarget(mainnet_genesis_nbits, params.powLimit).has_value());
}

BOOST_AUTO_TEST_CASE(testnet_genesis_uses_powlimit_compact)
{
    const auto params = CreateChainParams(ArgsManager{}, ChainType::TESTNET)->GetConsensus();
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    const uint32_t testnet_genesis_nbits = 0x20027525U;

    BOOST_CHECK_EQUAL(testnet_genesis_nbits, pow_limit.GetCompact());

    arith_uint256 genesis_target{};
    genesis_target.SetCompact(testnet_genesis_nbits);
    BOOST_CHECK(genesis_target <= pow_limit);
    BOOST_CHECK(genesis_target > 0);
}

// --- Fast mining phase returns bootstrap difficulty (ASERT not yet active) ---

BOOST_AUTO_TEST_CASE(asert_fast_phase_holds_bootstrap_difficulty)
{
    auto params = MatMulRetargetParams();
    params.nPowTargetSpacingFastMs = 1'000;
    params.nPowTargetSpacingNormal = 90;
    params.nFastMineHeight = 50'000;
    params.nMatMulAsertHeight = 50'000;

    std::vector<CBlockIndex> blocks(400);
    SeedFixedDifficultyChain(blocks, 0x1f0fffffU, 1'700'000'000, 1);
    blocks[0].nBits = 0x1d00ffffU;

    CBlockHeader next{};
    next.nTime = blocks.back().GetBlockTime() + 1;

    // During fast phase, difficulty is fixed at genesis-derived bootstrap.
    BOOST_CHECK_EQUAL(blocks.back().nHeight, 399);
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), blocks[0].nBits);
}

// --- ASERT activates with bootstrap factor at nFastMineHeight ---

BOOST_AUTO_TEST_CASE(asert_activation_bootstrap_factor_eases_target)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 541;
    params.nMatMulAsertHeight = 541;
    params.nMatMulAsertHalfLife = 14'400;
    params.nMatMulAsertBootstrapFactor = 40;

    std::vector<CBlockIndex> blocks(541);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader next{};
    next.nTime = blocks.back().GetBlockTime() + 1;
    const arith_uint256 parent_target = DecodeTarget(blocks.back().nBits);
    const arith_uint256 asert_target = DecodeTarget(GetNextWorkRequired(&blocks.back(), &next, params));
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);

    arith_uint256 expected = parent_target;
    expected *= params.nMatMulAsertBootstrapFactor;
    if (expected > pow_limit) {
        expected = pow_limit;
    }

    BOOST_CHECK(asert_target > parent_target);
    BOOST_CHECK_EQUAL(asert_target, expected);
}

// --- ASERT anchors on activation block ---

BOOST_AUTO_TEST_CASE(asert_post_activation_anchors_on_activation_block)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 1'000'000'000;
    params.nMatMulAsertBootstrapFactor = 40;

    std::vector<CBlockIndex> blocks(363);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[360].GetBlockTime() + 1;
    blocks[361].nHeight = 361;
    blocks[361].nBits = GetNextWorkRequired(&blocks[360], &activation, params);
    blocks[361].nTime = activation.nTime;
    blocks[361].pprev = &blocks[360];

    CBlockHeader next{};
    next.nTime = blocks[361].GetBlockTime() + 90;
    const arith_uint256 target_362 = DecodeTarget(GetNextWorkRequired(&blocks[361], &next, params));
    const arith_uint256 activation_target = DecodeTarget(blocks[361].nBits);
    const arith_uint256 parent_target = DecodeTarget(blocks[360].nBits);

    // Block 362 should track activation target with only a tiny ASERT step.
    BOOST_CHECK(target_362 <= activation_target);
    BOOST_CHECK(target_362 > (activation_target >> 1));
    BOOST_CHECK(target_362 > parent_target);

    // Mutating pre-activation parent difficulty must not change post-activation
    // retargeting because anchor is height 361 itself.
    blocks[360].nBits = 0x1b0404cbU;
    const arith_uint256 target_362_after_parent_mutation =
        DecodeTarget(GetNextWorkRequired(&blocks[361], &next, params));
    BOOST_CHECK_EQUAL(target_362_after_parent_mutation, target_362);
}

// --- ASERT convergence tests ---

BOOST_AUTO_TEST_CASE(asert_step_up_converges_under_faster_blocks)
{
    auto params = MatMulRetargetParams();
    params.nMatMulAsertHeight = 0;
    std::vector<CBlockIndex> blocks(361 + 240);
    SeedFixedDifficultyChain(blocks, 0x1f040d7fU, 1'700'000'000, 90);

    for (int height = 361; height < static_cast<int>(blocks.size()); ++height) {
        AppendSimulatedBlock(params, blocks, height, 30);
    }

    const arith_uint256 start_target = DecodeTarget(blocks[361].nBits);
    const arith_uint256 final_target = DecodeTarget(blocks.back().nBits);
    BOOST_CHECK(final_target < start_target);
}

BOOST_AUTO_TEST_CASE(asert_step_down_converges_under_slower_blocks)
{
    auto params = MatMulRetargetParams();
    params.nMatMulAsertHeight = 0;
    std::vector<CBlockIndex> blocks(361 + 240);
    SeedFixedDifficultyChain(blocks, 0x1f040d7fU, 1'700'000'000, 90);

    for (int height = 361; height < static_cast<int>(blocks.size()); ++height) {
        AppendSimulatedBlock(params, blocks, height, 600);
    }

    const arith_uint256 start_target = DecodeTarget(blocks[361].nBits);
    const arith_uint256 final_target = DecodeTarget(blocks.back().nBits);
    BOOST_CHECK(final_target > start_target);
    BOOST_CHECK(final_target <= UintToArith256(params.powLimit));
}

BOOST_AUTO_TEST_CASE(asert_oscillation_resilience)
{
    auto params = MatMulRetargetParams();
    params.nMatMulAsertHeight = 0;
    std::vector<CBlockIndex> blocks(361 + 400);
    SeedFixedDifficultyChain(blocks, 0x1f040d7fU, 1'700'000'000, 90);

    for (int height = 361; height < static_cast<int>(blocks.size()); ++height) {
        const int64_t spacing = ((height & 1) == 0) ? 30 : 600;
        AppendSimulatedBlock(params, blocks, height, spacing);

        const arith_uint256 target = DecodeTarget(blocks[height].nBits);
        BOOST_CHECK(target > 0);
        BOOST_CHECK(target <= UintToArith256(params.powLimit));
    }

    const arith_uint256 start_target = DecodeTarget(blocks[361].nBits);
    const arith_uint256 final_target = DecodeTarget(blocks.back().nBits);
    arith_uint256 upper = start_target;
    upper *= 1'000;
    arith_uint256 lower = start_target;
    lower /= 1'000;
    BOOST_CHECK(final_target < upper);
    BOOST_CHECK(final_target > lower);
}

// --- ASERT retune tests ---

BOOST_AUTO_TEST_CASE(asert_retune_hardening_factor_applies_at_height)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 14'400;
    params.nMatMulAsertBootstrapFactor = 40;
    params.nMatMulAsertRetuneHeight = 366;
    params.nMatMulAsertRetuneHardeningFactor = 2;

    std::vector<CBlockIndex> blocks(367);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[360].GetBlockTime() + 1;
    blocks[361].nHeight = 361;
    blocks[361].nBits = GetNextWorkRequired(&blocks[360], &activation, params);
    blocks[361].nTime = activation.nTime;
    blocks[361].pprev = &blocks[360];
    for (int h = 362; h <= 365; ++h) {
        AppendSimulatedBlock(params, blocks, h, 90);
    }

    CBlockHeader retune_next{};
    retune_next.nTime = blocks[365].GetBlockTime() + 90;
    const arith_uint256 retune_target =
        DecodeTarget(GetNextWorkRequired(&blocks[365], &retune_next, params));
    const arith_uint256 parent_target = DecodeTarget(blocks[365].nBits);

    arith_uint256 expected = parent_target;
    expected /= params.nMatMulAsertRetuneHardeningFactor;
    if (expected == 0) {
        expected = arith_uint256{1};
    }
    BOOST_CHECK(retune_target < parent_target);
    BOOST_CHECK_EQUAL(retune_target, expected);
}

BOOST_AUTO_TEST_CASE(asert_post_retune_anchors_on_retune_block)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 1'000'000'000;
    params.nMatMulAsertBootstrapFactor = 40;
    params.nMatMulAsertRetuneHeight = 366;
    params.nMatMulAsertRetuneHardeningFactor = 2;

    std::vector<CBlockIndex> blocks(368);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[360].GetBlockTime() + 1;
    blocks[361].nHeight = 361;
    blocks[361].nBits = GetNextWorkRequired(&blocks[360], &activation, params);
    blocks[361].nTime = activation.nTime;
    blocks[361].pprev = &blocks[360];
    for (int h = 362; h <= 366; ++h) {
        AppendSimulatedBlock(params, blocks, h, 90);
    }

    CBlockHeader after_retune{};
    after_retune.nTime = blocks[366].GetBlockTime() + 90;
    const arith_uint256 target_367 =
        DecodeTarget(GetNextWorkRequired(&blocks[366], &after_retune, params));
    const arith_uint256 retune_target = DecodeTarget(blocks[366].nBits);

    BOOST_CHECK(target_367 <= retune_target);
    BOOST_CHECK(target_367 > (retune_target >> 1));

    // Mutating pre-retune parent must not change post-retune target because
    // anchor has moved to retune height.
    blocks[365].nBits = 0x1b0404cbU;
    const arith_uint256 target_367_after_parent_mutation =
        DecodeTarget(GetNextWorkRequired(&blocks[366], &after_retune, params));
    BOOST_CHECK_EQUAL(target_367_after_parent_mutation, target_367);
}

BOOST_AUTO_TEST_CASE(asert_retune2_multiplier_applies_at_height)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 14'400;
    params.nMatMulAsertBootstrapFactor = 40;
    params.nMatMulAsertRetuneHeight = 366;
    params.nMatMulAsertRetuneHardeningFactor = 2;
    params.nMatMulAsertRetune2Height = 370;
    params.nMatMulAsertRetune2TargetNum = 4;
    params.nMatMulAsertRetune2TargetDen = 3;

    std::vector<CBlockIndex> blocks(371);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[360].GetBlockTime() + 1;
    blocks[361].nHeight = 361;
    blocks[361].nBits = GetNextWorkRequired(&blocks[360], &activation, params);
    blocks[361].nTime = activation.nTime;
    blocks[361].pprev = &blocks[360];
    for (int h = 362; h <= 369; ++h) {
        AppendSimulatedBlock(params, blocks, h, 90);
    }

    CBlockHeader retune2_next{};
    retune2_next.nTime = blocks[369].GetBlockTime() + 90;
    const arith_uint256 retune2_target =
        DecodeTarget(GetNextWorkRequired(&blocks[369], &retune2_next, params));
    const arith_uint256 parent_target = DecodeTarget(blocks[369].nBits);

    arith_uint256 expected = parent_target;
    expected *= params.nMatMulAsertRetune2TargetNum;
    expected /= params.nMatMulAsertRetune2TargetDen;
    if (expected == 0) {
        expected = arith_uint256{1};
    }
    BOOST_CHECK(retune2_target > parent_target);
    BOOST_CHECK_EQUAL(retune2_target, expected);
}

BOOST_AUTO_TEST_CASE(asert_post_retune2_anchors_on_retune2_block)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 1'000'000'000;
    params.nMatMulAsertBootstrapFactor = 40;
    params.nMatMulAsertRetuneHeight = 366;
    params.nMatMulAsertRetuneHardeningFactor = 2;
    params.nMatMulAsertRetune2Height = 370;
    params.nMatMulAsertRetune2TargetNum = 4;
    params.nMatMulAsertRetune2TargetDen = 3;

    std::vector<CBlockIndex> blocks(372);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[360].GetBlockTime() + 1;
    blocks[361].nHeight = 361;
    blocks[361].nBits = GetNextWorkRequired(&blocks[360], &activation, params);
    blocks[361].nTime = activation.nTime;
    blocks[361].pprev = &blocks[360];
    for (int h = 362; h <= 370; ++h) {
        AppendSimulatedBlock(params, blocks, h, 90);
    }

    CBlockHeader after_retune2{};
    after_retune2.nTime = blocks[370].GetBlockTime() + 90;
    const arith_uint256 target_371 =
        DecodeTarget(GetNextWorkRequired(&blocks[370], &after_retune2, params));
    const arith_uint256 retune2_target = DecodeTarget(blocks[370].nBits);

    BOOST_CHECK(target_371 <= retune2_target);
    BOOST_CHECK(target_371 > (retune2_target >> 1));

    // Mutating pre-retune2 parent must not change post-retune2 target because
    // anchor has moved to retune2 height.
    blocks[369].nBits = 0x1b0404cbU;
    const arith_uint256 target_371_after_parent_mutation =
        DecodeTarget(GetNextWorkRequired(&blocks[370], &after_retune2, params));
    BOOST_CHECK_EQUAL(target_371_after_parent_mutation, target_371);
}

BOOST_AUTO_TEST_CASE(asert_half_life_upgrade_keeps_activation_target_continuous)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 14'400;
    params.nMatMulAsertHalfLifeUpgradeHeight = 366;
    params.nMatMulAsertHalfLifeUpgrade = 3'600;

    std::vector<CBlockIndex> blocks(367);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[360].GetBlockTime() + 1;
    blocks[361].nHeight = 361;
    blocks[361].nBits = GetNextWorkRequired(&blocks[360], &activation, params);
    blocks[361].nTime = activation.nTime;
    blocks[361].pprev = &blocks[360];
    for (int h = 362; h <= 365; ++h) {
        AppendSimulatedBlock(params, blocks, h, 90);
    }

    CBlockHeader upgrade_next{};
    upgrade_next.nTime = blocks[365].GetBlockTime() + 90;
    const arith_uint256 upgrade_target =
        DecodeTarget(GetNextWorkRequired(&blocks[365], &upgrade_next, params));
    const arith_uint256 parent_target = DecodeTarget(blocks[365].nBits);

    BOOST_CHECK_EQUAL(upgrade_target, parent_target);
}

BOOST_AUTO_TEST_CASE(asert_post_half_life_upgrade_anchors_on_upgrade_block_and_uses_new_half_life)
{
    auto upgraded_params = MatMulRetargetParams();
    upgraded_params.nFastMineHeight = 361;
    upgraded_params.nMatMulAsertHeight = 361;
    upgraded_params.nMatMulAsertHalfLife = 14'400;
    upgraded_params.nMatMulAsertHalfLifeUpgradeHeight = 366;
    upgraded_params.nMatMulAsertHalfLifeUpgrade = 3'600;

    auto control_params = upgraded_params;
    control_params.nMatMulAsertHalfLifeUpgrade = 14'400;

    std::vector<CBlockIndex> blocks(368);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[360].GetBlockTime() + 1;
    blocks[361].nHeight = 361;
    blocks[361].nBits = GetNextWorkRequired(&blocks[360], &activation, upgraded_params);
    blocks[361].nTime = activation.nTime;
    blocks[361].pprev = &blocks[360];
    for (int h = 362; h <= 366; ++h) {
        AppendSimulatedBlock(upgraded_params, blocks, h, 90);
    }

    CBlockHeader after_upgrade{};
    after_upgrade.nTime = blocks[366].GetBlockTime() + 90;
    const arith_uint256 upgraded_target =
        DecodeTarget(GetNextWorkRequired(&blocks[366], &after_upgrade, upgraded_params));
    const arith_uint256 control_target =
        DecodeTarget(GetNextWorkRequired(&blocks[366], &after_upgrade, control_params));

    BOOST_CHECK(upgraded_target < control_target);
    BOOST_CHECK(upgraded_target > 0);

    blocks[365].nBits = 0x1b0404cbU;
    const arith_uint256 upgraded_target_after_parent_mutation =
        DecodeTarget(GetNextWorkRequired(&blocks[366], &after_upgrade, upgraded_params));
    BOOST_CHECK_EQUAL(upgraded_target_after_parent_mutation, upgraded_target);
}

// --- ASERT stability and edge case tests ---

BOOST_AUTO_TEST_CASE(asert_steady_spacing_stays_bounded)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 14'400;

    std::vector<CBlockIndex> blocks(361 + 300);
    SeedFixedDifficultyChain(blocks, 0x1e0d7f00U, 1'700'000'000, 90);

    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    for (int height = 361; height < static_cast<int>(blocks.size()); ++height) {
        AppendSimulatedBlock(params, blocks, height, 90);
        const arith_uint256 target = DecodeTarget(blocks[height].nBits);
        BOOST_CHECK(target > 0);
        BOOST_CHECK(target <= pow_limit);
    }

    const arith_uint256 start_target = DecodeTarget(blocks[361].nBits);
    const arith_uint256 final_target = DecodeTarget(blocks.back().nBits);
    const double start = start_target.getdouble();
    const double end = final_target.getdouble();
    BOOST_REQUIRE(start > 0.0);
    const double ratio = end / start;
    BOOST_CHECK_GE(ratio, 0.98);
    BOOST_CHECK_LE(ratio, 1.02);
}

BOOST_AUTO_TEST_CASE(asert_half_life_controls_reactivity)
{
    auto params_fast = MatMulRetargetParams();
    params_fast.nFastMineHeight = 500;
    params_fast.nMatMulAsertHeight = 500;
    params_fast.nMatMulAsertHalfLife = 3'600;

    auto params_slow = params_fast;
    params_slow.nMatMulAsertHalfLife = 86'400;

    std::vector<CBlockIndex> blocks(541);
    SeedFixedDifficultyChain(blocks, 0x1e0d7f00U, 1'700'000'000, 300);

    CBlockHeader next{};
    next.nTime = blocks.back().GetBlockTime() + 1;

    const arith_uint256 parent_target = DecodeTarget(blocks.back().nBits);
    const arith_uint256 fast_target = DecodeTarget(GetNextWorkRequired(&blocks.back(), &next, params_fast));
    const arith_uint256 slow_target = DecodeTarget(GetNextWorkRequired(&blocks.back(), &next, params_slow));

    BOOST_CHECK(fast_target > slow_target);
    BOOST_CHECK(slow_target > parent_target);
}

BOOST_AUTO_TEST_CASE(asert_path_independent_for_same_anchor_height_time)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 14'400;

    constexpr int first_varied_height = 362;
    constexpr int parent_height = 380;
    constexpr int total_blocks = parent_height + 1;
    std::vector<CBlockIndex> chain_a(total_blocks);
    std::vector<CBlockIndex> chain_b(total_blocks);
    SeedFixedDifficultyChain(chain_a, 0x1e0d7f00U, 1'700'000'000, 90);
    SeedFixedDifficultyChain(chain_b, 0x1e0d7f00U, 1'700'000'000, 90);

    // Same multiset of spacings for heights 362..380, different ordering.
    // Height 361 (ASERT anchor) is identical across chains; parent timestamp
    // at 380 is also identical, exercising path-independence.
    std::vector<int64_t> spacings_a{};
    std::vector<int64_t> spacings_b{};
    for (int i = 0; i < 9; ++i) {
        spacings_a.push_back(30);
        spacings_a.push_back(150);
        spacings_b.push_back(150);
        spacings_b.push_back(30);
    }
    spacings_a.push_back(30);
    spacings_b.push_back(30);

    for (int i = 0; i < 19; ++i) {
        const int height = first_varied_height + i;
        AppendSimulatedBlock(params, chain_a, height, spacings_a[i]);
        AppendSimulatedBlock(params, chain_b, height, spacings_b[i]);
    }

    const CBlockIndex* parent_a = &chain_a[parent_height];
    const CBlockIndex* parent_b = &chain_b[parent_height];
    BOOST_REQUIRE(parent_a != nullptr);
    BOOST_REQUIRE(parent_b != nullptr);
    BOOST_CHECK_EQUAL(parent_a->nHeight, parent_b->nHeight);
    BOOST_CHECK_EQUAL(parent_a->GetBlockTime(), parent_b->GetBlockTime());

    const int32_t anchor_height = params.nMatMulAsertHeight;
    const CBlockIndex* anchor_a = parent_a->GetAncestor(anchor_height);
    const CBlockIndex* anchor_b = parent_b->GetAncestor(anchor_height);
    BOOST_REQUIRE(anchor_a != nullptr);
    BOOST_REQUIRE(anchor_b != nullptr);

    const int64_t delta_a = parent_a->GetBlockTime() - anchor_a->GetBlockTime();
    const int64_t delta_b = parent_b->GetBlockTime() - anchor_b->GetBlockTime();
    BOOST_CHECK_EQUAL(delta_a, delta_b);

    CBlockHeader next_a{};
    next_a.nTime = parent_a->GetBlockTime() + 90;
    CBlockHeader next_b{};
    next_b.nTime = parent_b->GetBlockTime() + 90;
    const uint32_t bits_a = GetNextWorkRequired(parent_a, &next_a, params);
    const uint32_t bits_b = GetNextWorkRequired(parent_b, &next_b, params);

    // ASERT is stateless/path-independent: same anchor + same elapsed time +
    // same height must produce the same next target, regardless of prior path.
    BOOST_CHECK_EQUAL(bits_a, bits_b);
}

BOOST_AUTO_TEST_CASE(asert_extreme_inputs_respect_powlimit_and_nonzero_target)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 361;
    params.nMatMulAsertHeight = 361;
    params.nMatMulAsertHalfLife = 14'400;

    std::vector<CBlockIndex> slow_blocks(400);
    SeedFixedDifficultyChain(slow_blocks, 0x1e0d7f00U, 1'700'000'000, 90);
    // Drive elapsed time massively above schedule to force powLimit clamp.
    slow_blocks.back().nTime = slow_blocks[360].GetBlockTime() + 2'000'000'000;
    CBlockHeader next_slow{};
    next_slow.nTime = slow_blocks.back().GetBlockTime() + 1;
    const arith_uint256 slow_target = DecodeTarget(GetNextWorkRequired(&slow_blocks.back(), &next_slow, params));
    arith_uint256 compact_pow_limit{};
    compact_pow_limit.SetCompact(UintToArith256(params.powLimit).GetCompact());
    BOOST_CHECK_EQUAL(slow_target, compact_pow_limit);

    auto hard_params = params;
    hard_params.nMatMulAsertHalfLife = 1;
    std::vector<CBlockIndex> hard_blocks(500);
    SeedFixedDifficultyChain(hard_blocks, 0x1e0d7f00U, 1'700'000'000, 1);
    CBlockHeader next_hard{};
    next_hard.nTime = hard_blocks.back().GetBlockTime() + 1;
    const arith_uint256 hard_target = DecodeTarget(GetNextWorkRequired(&hard_blocks.back(), &next_hard, hard_params));
    BOOST_CHECK_EQUAL(hard_target, arith_uint256{1});
}

BOOST_AUTO_TEST_CASE(asert_zero_and_negative_time_diff_are_bounded)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 10;
    params.nMatMulAsertHeight = 10;
    params.nMatMulAsertHalfLife = 14'400;

    std::vector<CBlockIndex> blocks(11);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);

    CBlockHeader activation{};
    activation.nTime = blocks[9].GetBlockTime() + 1;
    blocks[10].nHeight = 10;
    blocks[10].nBits = GetNextWorkRequired(&blocks[9], &activation, params);
    blocks[10].nTime = activation.nTime;
    blocks[10].pprev = &blocks[9];

    CBlockIndex parent_zero{};
    parent_zero.nHeight = 11;
    parent_zero.nBits = blocks[10].nBits;
    parent_zero.nTime = blocks[10].GetBlockTime();
    parent_zero.pprev = &blocks[10];
    const int64_t anchor_time = blocks[9].GetBlockTime();

    CBlockHeader next_zero{};
    next_zero.nTime = anchor_time; // force time_diff == 0 at ASERT anchor
    const arith_uint256 target_zero =
        DecodeTarget(GetNextWorkRequired(&parent_zero, &next_zero, params));

    CBlockIndex parent_negative{};
    parent_negative.nHeight = parent_zero.nHeight;
    parent_negative.nBits = parent_zero.nBits;
    parent_negative.pprev = parent_zero.pprev;
    parent_negative.nTime = blocks[10].GetBlockTime() - 30;
    CBlockHeader next_negative{};
    next_negative.nTime = anchor_time - 1; // force time_diff < 0
    const arith_uint256 target_negative =
        DecodeTarget(GetNextWorkRequired(&parent_negative, &next_negative, params));

    const arith_uint256 pow_limit = UintToArith256(params.powLimit);
    BOOST_CHECK(target_zero > 0);
    BOOST_CHECK(target_zero <= pow_limit);
    BOOST_CHECK(target_negative > 0);
    BOOST_CHECK(target_negative <= pow_limit);
}

BOOST_AUTO_TEST_CASE(asert_fail_closed_bits_is_hardest_valid_target)
{
    // AUDIT D1: the fail-closed result is the hardest representable target
    // (arith_uint256{1}), encoded as compact 0x01010000. It must be a valid,
    // non-zero, non-negative, non-overflowing compact that decodes back to 1 and
    // is strictly harder than every network's powLimit.
    const uint32_t fail_closed_bits = MatMulAsertFailClosedBits();
    BOOST_CHECK_EQUAL(fail_closed_bits, 0x01010000U);
    bool negative{false};
    bool overflow{false};
    arith_uint256 decoded{};
    decoded.SetCompact(fail_closed_bits, &negative, &overflow);
    BOOST_CHECK(!negative);
    BOOST_CHECK(!overflow);
    BOOST_CHECK(decoded == arith_uint256{1});
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::TESTNET4, ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params = CreateChainParams(ArgsManager{}, chain)->GetConsensus();
        BOOST_CHECK(decoded < UintToArith256(params.powLimit));
        BOOST_CHECK(fail_closed_bits != UintToArith256(params.powLimit).GetCompact());
    }
}

BOOST_AUTO_TEST_CASE(asert_invalid_half_life_fails_closed_to_hardest_target)
{
    // AUDIT D1: an invalid half-life must fail CLOSED to the hardest target,
    // never OPEN to powLimit (the easiest target).
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 10;
    params.nMatMulAsertHeight = 10;

    std::vector<CBlockIndex> blocks(12);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);
    CBlockHeader next{};
    next.nTime = blocks.back().GetBlockTime() + 90;
    const uint32_t powlimit_bits = UintToArith256(params.powLimit).GetCompact();
    const uint32_t hardest_bits = MatMulAsertFailClosedBits();
    BOOST_REQUIRE(hardest_bits != powlimit_bits);

    // Sanity: the valid schedule does NOT fail closed.
    BOOST_CHECK(GetNextWorkRequired(&blocks.back(), &next, params) != hardest_bits);

    params.nMatMulAsertHalfLife = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);
    params.nMatMulAsertHalfLife = -1;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    // The upgrade half-life is validated the same way once an upgrade height is set.
    params.nMatMulAsertHalfLife = 14'400;
    params.nMatMulAsertHalfLifeUpgradeHeight = 16;
    params.nMatMulAsertHalfLifeUpgrade = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);
    params.nMatMulAsertHalfLifeUpgrade = -3'600;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    // The validator itself reports the breach.
    params.nMatMulAsertHalfLifeUpgradeHeight = std::numeric_limits<int32_t>::max();
    params.nMatMulAsertHalfLife = 0;
    BOOST_CHECK(!ValidateMatMulAsertParams(params, 11));
    params.nMatMulAsertHalfLife = 14'400;
    BOOST_CHECK(ValidateMatMulAsertParams(params, 11));
}

BOOST_AUTO_TEST_CASE(asert_invalid_params_fail_closed_to_hardest_target)
{
    // AUDIT D1: every invalid-schedule path fails CLOSED to the hardest target.
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 10;
    params.nMatMulAsertHeight = 10;
    params.nMatMulAsertHalfLife = 0;

    std::vector<CBlockIndex> blocks(12);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);
    CBlockHeader next{};
    next.nTime = blocks.back().GetBlockTime() + 90;
    const uint32_t hardest_bits = MatMulAsertFailClosedBits();
    BOOST_REQUIRE(hardest_bits != UintToArith256(params.powLimit).GetCompact());
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertHalfLife = 14'400;
    params.nMatMulAsertRetuneHeight = 9; // below activation => invalid schedule
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertRetuneHeight = 16;
    params.nMatMulAsertRetune2Height = 14; // below retune => invalid schedule
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertRetune2Height = std::numeric_limits<int32_t>::max();
    params.nMatMulAsertHalfLifeUpgradeHeight = 16;
    params.nMatMulAsertHalfLifeUpgrade = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertHalfLifeUpgrade = 3'600;
    params.nMatMulAsertRetune2Height = 16;
    params.nMatMulAsertHalfLifeUpgradeHeight = 16; // upgrade must be strictly after latest retune anchor
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertHalfLifeUpgradeHeight = 17;
    params.nMatMulAsertBootstrapFactor = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertBootstrapFactor = 1;
    params.nMatMulAsertRetuneHardeningFactor = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertRetuneHardeningFactor = 1;
    params.nMatMulAsertRetune2TargetDen = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);

    params.nMatMulAsertRetune2TargetDen = 1;
    params.nPowTargetSpacing = 0;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&blocks.back(), &next, params), hardest_bits);
}

// QTC Option B launch vector: no fast phase; ASERT is live from block 1 with the
// genesis block as anchor, so the target already responds to parent timing.
BOOST_AUTO_TEST_CASE(asert_mainnet_option_b_retargets_from_block_one)
{
    const auto params = CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus();
    // DESIGN INVARIANT: nMatMulAsertHeight must equal nFastMineHeight.
    BOOST_REQUIRE_EQUAL(params.nMatMulAsertHeight, params.nFastMineHeight);
    BOOST_REQUIRE_EQUAL(params.nMatMulAsertHeight, 0);
    const arith_uint256 pow_limit = UintToArith256(params.powLimit);

    CBlockIndex genesis{};
    genesis.nHeight = 0;
    genesis.nBits = pow_limit.GetCompact();
    genesis.nTime = 1'770'000'000;
    genesis.pprev = nullptr;

    CBlockHeader next1{};
    next1.nTime = genesis.GetBlockTime() + 90;
    const uint32_t bits_1 = GetNextWorkRequired(&genesis, &next1, params);
    const arith_uint256 target_1 = DecodeTarget(bits_1);
    BOOST_CHECK(target_1 > 0);
    BOOST_CHECK(target_1 <= pow_limit);

    // Same block 1, two possible timestamps: an early parent tightens block 2,
    // a late parent eases it -- but never above the floor.
    CBlockIndex early_parent{};
    early_parent.nHeight = 1;
    early_parent.nBits = bits_1;
    early_parent.nTime = genesis.nTime + 1;
    early_parent.pprev = &genesis;

    CBlockIndex late_parent{};
    late_parent.nHeight = 1;
    late_parent.nBits = bits_1;
    late_parent.nTime = genesis.nTime + 3'000;
    late_parent.pprev = &genesis;

    CBlockHeader next2{};
    next2.nTime = genesis.nTime + 3'090;
    const arith_uint256 target_after_early = DecodeTarget(GetNextWorkRequired(&early_parent, &next2, params));
    const arith_uint256 target_after_late = DecodeTarget(GetNextWorkRequired(&late_parent, &next2, params));
    BOOST_CHECK(target_after_early < target_1);
    BOOST_CHECK(target_after_late > target_1);
    BOOST_CHECK(target_after_late <= pow_limit);
}

BOOST_AUTO_TEST_CASE(asert_mainnet_option_b_no_fast_phase_retargets_from_genesis)
{
    // QTC Option B: nFastMineHeight = nMatMulAsertHeight = 0, so there is no
    // bootstrap phase; block 1 is already an ASERT block anchored at genesis and
    // its target can never exceed powLimit (the floor).
    const auto params = CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus();
    BOOST_REQUIRE_EQUAL(params.nMatMulAsertHeight, params.nFastMineHeight);
    BOOST_REQUIRE_EQUAL(params.nFastMineHeight, 0);
    const arith_uint256 pow_limit{UintToArith256(params.powLimit)};
    const uint32_t powlimit_bits = pow_limit.GetCompact();

    CBlockIndex genesis{};
    genesis.nHeight = 0;
    genesis.nBits = powlimit_bits; // genesis nBits == compact(powLimit)
    genesis.nTime = 1'770'000'000;
    genesis.pprev = nullptr;

    CBlockHeader next{};
    next.nTime = genesis.GetBlockTime() + 1;
    const uint32_t observed = GetNextWorkRequired(&genesis, &next, params);
    arith_uint256 observed_target{};
    observed_target.SetCompact(observed);
    BOOST_CHECK(observed_target <= pow_limit);
    BOOST_CHECK(observed_target > pow_limit / 2); // one early block: ~0.4% tighter, not a cliff
}

BOOST_AUTO_TEST_CASE(asert_missing_anchor_fails_closed_to_hardest_target)
{
    auto params = MatMulRetargetParams();
    params.nFastMineHeight = 100;
    params.nMatMulAsertHeight = 100;
    params.nMatMulAsertHalfLife = 14'400;

    std::vector<CBlockIndex> blocks(121);
    SeedFixedDifficultyChain(blocks, 0x1f00ffffU, 1'700'000'000, 90);
    // Simulate a deep reorg/corruption below ASERT anchor height.
    blocks[106].pprev = nullptr;

    CBlockHeader next{};
    next.nTime = blocks.back().GetBlockTime() + 90;
    // A missing anchor means the difficulty schedule cannot be derived; the node must not hand
    // out the easiest target (that is how a corrupted index would weaken the chain) but the
    // hardest one, so no block can be built on the broken state until it is repaired.
    const uint32_t powlimit_bits = UintToArith256(params.powLimit).GetCompact();
    const uint32_t observed = GetNextWorkRequired(&blocks.back(), &next, params);
    BOOST_CHECK_EQUAL(observed, MatMulAsertFailClosedBits());
    BOOST_CHECK(observed != powlimit_bits);
}

BOOST_AUTO_TEST_SUITE_END()
