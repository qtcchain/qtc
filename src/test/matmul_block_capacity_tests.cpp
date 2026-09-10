// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <common/args.h>
#include <consensus/consensus.h>
#include <node/miner.h>
#include <node/types.h>
#include <policy/policy.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

namespace {

ArgsManager EmptyArgs()
{
    return ArgsManager{};
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(matmul_block_capacity_tests, BasicTestingSetup)

// TEST: block_capacity_params_sane_sizes
BOOST_AUTO_TEST_CASE(block_capacity_mainnet_defaults)
{
    auto params = CreateChainParams(EmptyArgs(), ChainType::MAIN);
    const auto& c = params->GetConsensus();

    BOOST_CHECK_EQUAL(c.nMaxBlockWeight, 24'000'000U);
    BOOST_CHECK_EQUAL(c.nMaxBlockSerializedSize, 24'000'000U);
    BOOST_CHECK_EQUAL(c.nMaxBlockSigOpsCost, 480'000U);
    BOOST_CHECK_EQUAL(c.nDefaultBlockMaxWeight, 24'000'000U);
    BOOST_CHECK(c.nDefaultBlockMaxWeight <= c.nMaxBlockWeight);
}

BOOST_AUTO_TEST_CASE(block_capacity_regtest_defaults)
{
    auto params = CreateChainParams(EmptyArgs(), ChainType::REGTEST);
    const auto& c = params->GetConsensus();

    BOOST_CHECK_EQUAL(c.nMaxBlockWeight, 24'000'000U);
    BOOST_CHECK_EQUAL(c.nDefaultBlockMaxWeight, 24'000'000U);
    BOOST_CHECK_EQUAL(c.nMaxBlockSerializedSize, 24'000'000U);
    BOOST_CHECK_EQUAL(c.nMaxBlockSigOpsCost, 480'000U);
}

// TEST: block_capacity_weight_validation
// TEST: block_capacity_sigops_validation
BOOST_AUTO_TEST_CASE(block_capacity_consensus_constants)
{
    BOOST_CHECK_EQUAL(MAX_BLOCK_WEIGHT, 24'000'000U);
    BOOST_CHECK_EQUAL(MAX_BLOCK_SERIALIZED_SIZE, 24'000'000U);
    BOOST_CHECK_EQUAL(MAX_BLOCK_SIGOPS_COST, 480'000);
}

BOOST_AUTO_TEST_CASE(block_capacity_witness_scale_factor)
{
    BOOST_CHECK_EQUAL(WITNESS_SCALE_FACTOR, 1);
}

BOOST_AUTO_TEST_CASE(block_capacity_mining_policy_defaults)
{
    BOOST_CHECK_EQUAL(DEFAULT_BLOCK_MAX_WEIGHT, 24'000'000U);
    BOOST_CHECK_EQUAL(DEFAULT_BLOCK_MAX_SIZE, 24'000'000U);

    const node::BlockCreateOptions opts;
    BOOST_CHECK_EQUAL(opts.nBlockMaxWeight, 24'000'000U);
    BOOST_CHECK_EQUAL(opts.nBlockMaxSize, 24'000'000U);
}

// M-7: block templates are not capped at a fixed transaction count by default;
// -blockmaxtemplatetxs=<n> still bounds selection when set explicitly.
BOOST_AUTO_TEST_CASE(block_capacity_template_tx_cap_default_unlimited)
{
    BOOST_CHECK_EQUAL(DEFAULT_BLOCK_MAX_TEMPLATE_TXS, 0U);
    BOOST_CHECK_EQUAL(node::BlockCreateOptions{}.nBlockMaxTemplateTxs, 0U);
    BOOST_CHECK_EQUAL(node::BlockCreateOptions{}.Clamped().nBlockMaxTemplateTxs, 0U);

    ArgsManager args;
    for (const char* arg : {"-blockmaxweight=<n>", "-blockmaxsize=<n>", "-blockmaxtemplatetxs=<n>",
                            "-blockmintxfee=<amt>", "-printpriority", "-blockreservedweight=<n>"}) {
        args.AddArg(arg, "", ArgsManager::ALLOW_ANY, OptionsCategory::BLOCK_CREATION);
    }
    node::BlockAssembler::Options options;
    node::ApplyArgsManOptions(args, options);
    BOOST_CHECK_EQUAL(options.nBlockMaxTemplateTxs, 0U);

    args.ForceSetArg("-blockmaxtemplatetxs", "7");
    node::ApplyArgsManOptions(args, options);
    BOOST_CHECK_EQUAL(options.nBlockMaxTemplateTxs, 7U);
}

// N-7: the Freivalds product payload size the template must reserve.
BOOST_AUTO_TEST_CASE(block_capacity_product_payload_reservation)
{
    auto params = CreateChainParams(EmptyArgs(), ChainType::MAIN);
    const auto& c = params->GetConsensus();
    const uint64_t words{static_cast<uint64_t>(c.nMatMulDimension) * c.nMatMulDimension};
    const uint64_t expected{words * sizeof(uint32_t) + GetSizeOfCompactSize(words)};
    BOOST_CHECK_EQUAL(c.nMatMulDimension, 512U);
    BOOST_CHECK_EQUAL(expected, 512ULL * 512ULL * 4ULL + 5ULL);
    // Mainnet requires the payload from nMatMulProductDigestHeight onwards.
    BOOST_CHECK_EQUAL(node::GetMatMulProductPayloadSerializedSize(c, c.nMatMulProductDigestHeight), expected);
    if (!c.fMatMulRequireProductPayload && c.nMatMulProductDigestHeight > 0) {
        BOOST_CHECK_EQUAL(node::GetMatMulProductPayloadSerializedSize(c, c.nMatMulProductDigestHeight - 1), 0U);
    }
    // The payload always fits alongside the coinbase reservation.
    BOOST_CHECK_LT(expected + DEFAULT_BLOCK_RESERVED_WEIGHT, MAX_BLOCK_WEIGHT);
}

BOOST_AUTO_TEST_CASE(block_capacity_block_create_clamping)
{
    node::BlockCreateOptions opts;
    opts.nBlockMaxWeight = MAX_BLOCK_WEIGHT + 1;
    opts.nBlockMaxSize = MAX_BLOCK_SERIALIZED_SIZE + 1;

    auto clamped = opts.Clamped();
    BOOST_CHECK_EQUAL(clamped.nBlockMaxWeight, MAX_BLOCK_WEIGHT);
    BOOST_CHECK_EQUAL(clamped.nBlockMaxSize, MAX_BLOCK_SERIALIZED_SIZE);

    opts.nBlockMaxWeight = opts.block_reserved_weight - 1;
    clamped = opts.Clamped();
    BOOST_CHECK_EQUAL(clamped.nBlockMaxWeight, opts.block_reserved_weight);
}

BOOST_AUTO_TEST_SUITE_END()
