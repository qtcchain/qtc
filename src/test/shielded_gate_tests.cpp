// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// QTC security review S-1/S-2/C-3/C-4: on a chain whose shielded pool is
// disabled (Consensus::Params::fShieldedPoolDisabled, mainnet) every shielded
// bundle is rejected by the height gate before any value-balance, witness,
// proof or ML-DSA work, is non-standard, and is rejected context-free by
// CheckBlock. With the flag clear the gate keeps its previous behaviour.
//
// The suite runs on the TESTNET-based LegacyScheduleTestingSetup (flag false,
// pre-sunset heights available) and flips the flag on a params copy.

#include <chainparams.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <crypto/chacha20poly1305.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <shielded/bundle.h>
#include <shielded/lattice/params.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <memory>
#include <string>

namespace {

// Structurally minimal legacy bundle: enough to pass CheckTransaction(), never
// enough to verify. The gate must reject it before looking inside.
CShieldedBundle BuildMinimalShieldedBundle()
{
    CShieldedBundle bundle;

    CShieldedInput in;
    in.nullifier = GetRandHash();
    for (size_t i = 0; i < shielded::lattice::RING_SIZE; ++i) {
        in.ring_positions.push_back(i);
    }
    bundle.shielded_inputs.push_back(in);
    bundle.proof = {0x01};

    CShieldedOutput out;
    out.note_commitment = GetRandHash();
    out.merkle_anchor = GetRandHash();
    out.encrypted_note.aead_ciphertext.assign(AEADChaCha20Poly1305::EXPANSION, 0x00);
    bundle.shielded_outputs.push_back(out);

    bundle.value_balance = 0;
    return bundle;
}

CMutableTransaction BuildTransparentTx()
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(GetRandHash()), 0});
    // OP_RETURN outputs satisfy every chain's reduced-data / P2MR-only output rules.
    mtx.vout.emplace_back(CAmount{0}, CScript{} << OP_RETURN);
    return mtx;
}

CTransactionRef BuildShieldedTx()
{
    CMutableTransaction mtx{BuildTransparentTx()};
    mtx.shielded_bundle = BuildMinimalShieldedBundle();
    return MakeTransactionRef(std::move(mtx));
}

CBlock BuildBlockWith(const CTransactionRef& tx)
{
    CMutableTransaction coinbase;
    coinbase.version = 2;
    coinbase.vin.emplace_back(COutPoint{}, CScript{} << OP_0 << OP_0);
    coinbase.vout.emplace_back(CAmount{0}, CScript{} << OP_RETURN);

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    block.vtx.push_back(tx);
    return block;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(shielded_gate_tests, LegacyScheduleTestingSetup)

BOOST_AUTO_TEST_CASE(shielded_pool_disabled_flag_matches_chain)
{
    BOOST_CHECK(CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus().fShieldedPoolDisabled);
    BOOST_CHECK(!CreateChainParams(*m_node.args, ChainType::TESTNET)->GetConsensus().fShieldedPoolDisabled);
    BOOST_CHECK(!CreateChainParams(*m_node.args, ChainType::REGTEST)->GetConsensus().fShieldedPoolDisabled);
    // This suite's own chain (testnet) must keep the flag clear so the "previous behaviour" leg is real.
    BOOST_CHECK(!Params().GetConsensus().fShieldedPoolDisabled);
}

BOOST_AUTO_TEST_CASE(height_gate_rejects_every_bundle_when_pool_disabled)
{
    const CTransactionRef shielded_tx{BuildShieldedTx()};
    BOOST_REQUIRE(shielded_tx->HasShieldedBundle());

    Consensus::Params disabled{Params().GetConsensus()};
    disabled.fShieldedPoolDisabled = true;

    // Every height, including pre-activation and the sunset itself, is rejected with the same reason.
    for (const int32_t height : {int32_t{0}, int32_t{100}, disabled.nShieldedSunsetHeight,
                                 std::numeric_limits<int32_t>::max()}) {
        std::string reject;
        BOOST_CHECK_MESSAGE(!RejectShieldedHeightGateViolation(*shielded_tx, disabled, height, reject),
                            "height " << height);
        BOOST_CHECK_EQUAL(reject, "bad-shielded-disabled");
    }

    // The real mainnet parameters behave the same way.
    {
        const auto main_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
        std::string reject;
        BOOST_CHECK(!RejectShieldedHeightGateViolation(*shielded_tx, main_params->GetConsensus(), 1, reject));
        BOOST_CHECK_EQUAL(reject, "bad-shielded-disabled");
    }

    // A transaction without a bundle is untouched by the rule.
    {
        const CTransactionRef transparent{MakeTransactionRef(BuildTransparentTx())};
        BOOST_REQUIRE(!transparent->HasShieldedBundle());
        std::string reject;
        BOOST_CHECK(RejectShieldedHeightGateViolation(*transparent, disabled, 100, reject));
        BOOST_CHECK(reject.empty());
    }
}

BOOST_AUTO_TEST_CASE(height_gate_keeps_previous_behaviour_when_pool_enabled)
{
    const CTransactionRef shielded_tx{BuildShieldedTx()};

    Consensus::Params enabled{Params().GetConsensus()};
    enabled.fShieldedPoolDisabled = false;
    BOOST_REQUIRE(enabled.nShieldedSunsetHeight > 100);
    BOOST_REQUIRE(!enabled.IsShieldedPoolCreditDisabled(100));

    // Pre-sunset: the gate has nothing to say about a structurally minimal bundle.
    {
        std::string reject;
        BOOST_CHECK_MESSAGE(RejectShieldedHeightGateViolation(*shielded_tx, enabled, 100, reject), reject);
        BOOST_CHECK(reject.empty());
    }
    // At the sunset the legacy rules (not the disabled rule) apply: a legacy
    // bundle that is not a strict outflow exit is rejected by the sunset gate.
    {
        std::string reject;
        BOOST_CHECK(!RejectShieldedHeightGateViolation(*shielded_tx, enabled, enabled.nShieldedSunsetHeight, reject));
        BOOST_CHECK_NE(reject, "bad-shielded-disabled");
        BOOST_CHECK(!reject.empty());
    }
}

BOOST_AUTO_TEST_CASE(shielded_tx_is_nonstandard_when_pool_disabled)
{
    const CTransactionRef shielded_tx{BuildShieldedTx()};
    const CTransactionRef transparent{MakeTransactionRef(BuildTransparentTx())};

    Consensus::Params disabled{Params().GetConsensus()};
    disabled.fShieldedPoolDisabled = true;
    Consensus::Params enabled{Params().GetConsensus()};
    enabled.fShieldedPoolDisabled = false;

    std::string reason;
    BOOST_CHECK(!IsShieldedTxPolicyStandard(*shielded_tx, disabled, reason));
    BOOST_CHECK_EQUAL(reason, "shielded-disabled");

    reason.clear();
    BOOST_CHECK(IsShieldedTxPolicyStandard(*transparent, disabled, reason));
    BOOST_CHECK(reason.empty());

    BOOST_CHECK(IsShieldedTxPolicyStandard(*shielded_tx, enabled, reason));
    BOOST_CHECK(reason.empty());

    const auto main_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    BOOST_CHECK(!IsShieldedTxPolicyStandard(*shielded_tx, main_params->GetConsensus(), reason));
    BOOST_CHECK_EQUAL(reason, "shielded-disabled");
}

// CheckBlock is context-free and runs on every chainstate before a block is
// stored, relayed or connected, so a bundle on a disabled chain is a consensus
// rejection there (the bundle is txid- and therefore merkle-committed, so the
// header is legitimately invalidated, unlike the N-5 payload vectors).
BOOST_AUTO_TEST_CASE(checkblock_rejects_bundle_when_pool_disabled)
{
    const CTransactionRef shielded_tx{BuildShieldedTx()};

    Consensus::Params disabled{Params().GetConsensus()};
    disabled.fShieldedPoolDisabled = true;
    Consensus::Params enabled{Params().GetConsensus()};
    enabled.fShieldedPoolDisabled = false;

    {
        CBlock block{BuildBlockWith(shielded_tx)};
        BlockValidationState state;
        BOOST_CHECK(!CheckBlock(block, state, disabled, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false));
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_CONSENSUS);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-shielded-disabled");
    }
    {
        // Same block, flag clear: CheckBlock has no context-free objection.
        CBlock block{BuildBlockWith(shielded_tx)};
        BlockValidationState state;
        BOOST_CHECK_MESSAGE(CheckBlock(block, state, enabled, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false), state.ToString());
    }
    {
        // A block without any bundle is unaffected by the flag.
        CBlock block{BuildBlockWith(MakeTransactionRef(BuildTransparentTx()))};
        BlockValidationState state;
        BOOST_CHECK_MESSAGE(CheckBlock(block, state, disabled, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false), state.ToString());
    }
}

BOOST_AUTO_TEST_SUITE_END()
