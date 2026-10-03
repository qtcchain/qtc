// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// QTC v2 treasury allocation: at nTreasuryPremineHeight the coinbase must pay the premine amount to the
// treasury script exactly once, and the premine is added to the subsidy at that height only.

#include <chainparams.h>
#include <consensus/merkle.h>
#include <node/miner.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

using node::BlockAssembler;
using node::CBlockTemplate;

namespace {
const std::string kTreasuryProgramHex{"58dd0cb7668399503edc13a9d67dcf21a6be63a23958bdb2dfe3614e563154d6"};
const std::string kTreasuryScriptArg{"-regtesttreasurypreminescript=5220" + kTreasuryProgramHex};
constexpr CAmount kPremine{2'000'000 * COIN};

struct TreasuryPremineSetup : public TestingSetup {
    static TestOpts BuildOpts()
    {
        TestOpts opts;
        opts.extra_args = {"-test=matmulstrict", "-regtesttreasurypremineheight=1", "-regtesttreasurypremineamount=200000000000000",
                           kTreasuryScriptArg.c_str()};
        return opts;
    }
    TreasuryPremineSetup() : TestingSetup{ChainType::REGTEST, BuildOpts()} {}

    CScript TreasuryScript() const { return CScript() << OP_2 << ParseHex(kTreasuryProgramHex); }
    CScript MinerScript() const { return CScript() << OP_2 << std::vector<unsigned char>(32, 0x42); }

    std::shared_ptr<CBlockTemplate> Template()
    {
        BlockAssembler::Options options;
        options.coinbase_output_script = MinerScript();
        options.test_block_validity = true; // the assembler itself must accept what it built
        return BlockAssembler{m_node.chainman->ActiveChainstate(), m_node.mempool.get(), options, m_node}.CreateNewBlock();
    }

    bool Valid(const CBlock& block, std::string* reason = nullptr)
    {
        LOCK(cs_main);
        BlockValidationState state;
        CBlockIndex* prev = m_node.chainman->ActiveChain().Tip();
        const bool ok = TestBlockValidity(state, Params(), m_node.chainman->ActiveChainstate(), block, prev,
                                          /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false);
        if (reason) *reason = state.GetRejectReason();
        return ok && state.IsValid();
    }

    static CBlock WithCoinbase(const CBlock& block, const CMutableTransaction& cb)
    {
        CBlock copy{block};
        copy.vtx[0] = MakeTransactionRef(cb);
        copy.hashMerkleRoot = BlockMerkleRoot(copy);
        return copy;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(treasury_premine_tests, TreasuryPremineSetup)

BOOST_AUTO_TEST_CASE(regtest_override_sets_consensus_params)
{
    const auto& consensus = Params().GetConsensus();
    BOOST_CHECK_EQUAL(consensus.nTreasuryPremineHeight, 1);
    BOOST_CHECK_EQUAL(consensus.nTreasuryPremineAmount, kPremine);
    BOOST_CHECK(consensus.treasuryPremineScript == TreasuryScript());
    BOOST_CHECK(consensus.TreasuryPremineActiveAt(1));
    BOOST_CHECK(!consensus.TreasuryPremineActiveAt(0));
    BOOST_CHECK(!consensus.TreasuryPremineActiveAt(2));
}

BOOST_AUTO_TEST_CASE(assembler_pays_the_treasury_in_block_one)
{
    const auto tmpl = Template();
    BOOST_REQUIRE(tmpl);
    const CTransaction& cb = *tmpl->block.vtx[0];
    // miner output, treasury output, witness-commitment OP_RETURN
    BOOST_REQUIRE_EQUAL(cb.vout.size(), 3U);
    const CAmount subsidy = GetBlockSubsidy(1, Params().GetConsensus());
    BOOST_CHECK(cb.vout[0].scriptPubKey == MinerScript());
    BOOST_CHECK_EQUAL(cb.vout[0].nValue, subsidy);
    BOOST_CHECK(cb.vout[1].scriptPubKey == TreasuryScript());
    BOOST_CHECK_EQUAL(cb.vout[1].nValue, kPremine);
    BOOST_CHECK(cb.vout[2].scriptPubKey.IsUnspendable());
    BOOST_CHECK_EQUAL(cb.vout[2].nValue, 0);
    BOOST_CHECK_EQUAL(cb.GetValueOut(), subsidy + kPremine);
    BOOST_CHECK(Valid(tmpl->block));
}

BOOST_AUTO_TEST_CASE(block_one_without_the_treasury_output_is_invalid)
{
    const auto tmpl = Template();
    CMutableTransaction cb{*tmpl->block.vtx[0]};
    BOOST_REQUIRE(cb.vout.size() == 3 && cb.vout[1].scriptPubKey == TreasuryScript());
    std::string reason;

    // Miner keeps everything: total still within the limit, so bad-cb-amount does not fire — the premine rule must.
    CMutableTransaction keep{cb};
    keep.vout[0].nValue += keep.vout[1].nValue;
    keep.vout.erase(keep.vout.begin() + 1);
    BOOST_CHECK(!Valid(WithCoinbase(tmpl->block, keep), &reason));
    BOOST_CHECK_EQUAL(reason, "bad-cb-treasury-premine");

    // Output simply dropped (block pays less than allowed): still invalid.
    CMutableTransaction dropped{cb};
    dropped.vout.erase(dropped.vout.begin() + 1);
    BOOST_CHECK(!Valid(WithCoinbase(tmpl->block, dropped), &reason));
    BOOST_CHECK_EQUAL(reason, "bad-cb-treasury-premine");

    // Wrong amount to the treasury.
    CMutableTransaction short_pay{cb};
    short_pay.vout[1].nValue -= 1;
    BOOST_CHECK(!Valid(WithCoinbase(tmpl->block, short_pay), &reason));
    BOOST_CHECK_EQUAL(reason, "bad-cb-treasury-premine");

    // Right amount, wrong script.
    CMutableTransaction wrong_script{cb};
    wrong_script.vout[1].scriptPubKey = CScript() << OP_2 << std::vector<unsigned char>(32, 0x99);
    BOOST_CHECK(!Valid(WithCoinbase(tmpl->block, wrong_script), &reason));
    BOOST_CHECK_EQUAL(reason, "bad-cb-treasury-premine");

    // Treasury paid twice: exceeds the reward.
    CMutableTransaction twice{cb};
    twice.vout.insert(twice.vout.begin() + 2, twice.vout[1]);
    BOOST_CHECK(!Valid(WithCoinbase(tmpl->block, twice), &reason));
    BOOST_CHECK_EQUAL(reason, "bad-cb-amount");

    // The genuine template still validates after all that.
    BOOST_CHECK(Valid(tmpl->block));
}

BOOST_AUTO_TEST_CASE(premine_counts_only_at_its_height)
{
    const auto& consensus = Params().GetConsensus();
    CBlock dummy; // empty-block penalty is inactive on regtest, so only the height matters here
    BOOST_CHECK_EQUAL(GetBlockSubsidyForBlock(1, dummy, nullptr, consensus), GetBlockSubsidy(1, consensus) + kPremine);
    BOOST_CHECK_EQUAL(GetBlockSubsidyForBlock(2, dummy, nullptr, consensus), GetBlockSubsidy(2, consensus));
    BOOST_CHECK_EQUAL(GetBlockSubsidyForBlock(0, dummy, nullptr, consensus), GetBlockSubsidy(0, consensus));
}

BOOST_AUTO_TEST_SUITE_END()
