// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/amount.h>
#include <chainparams.h>
#include <hash.h>
#include <pqkey.h>
#include <script/ctv.h>
#include <script/interpreter.h>
#include <script/pqm.h>
#include <script/script.h>
#include <script/script_error.h>
#include <script/sigcache.h>
#include <serialize.h>
#include <shielded/v2_bundle.h>
#include <streams.h>
#include <test/util/shielded_account_registry_test_util.h>
#include <test/util/setup_common.h>
#include <test/util/shielded_smile_test_util.h>
#include <test/util/transaction_utils.h>
#include <util/chaintype.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

constexpr unsigned int P2MR_SCRIPT_FLAGS =
    SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_NULLFAIL |
    SCRIPT_VERIFY_CHECKTEMPLATEVERIFY | SCRIPT_VERIFY_CHECKSIGFROMSTACK;

smile2::CompactPublicAccount MakeSmileAccount(uint32_t seed)
{
    return test::shielded::MakeDeterministicCompactPublicAccount(seed);
}

class StubPQChecker final : public BaseSignatureChecker
{
public:
    bool CheckPQSignature(Span<const unsigned char>, Span<const unsigned char>, PQAlgorithm, uint8_t, SigVersion, ScriptExecutionData&, bool) const override
    {
        return true;
    }
};

class StubCTVChecker final : public BaseSignatureChecker
{
public:
    bool CheckCTVHash(Span<const unsigned char>) const override
    {
        return true;
    }
};

class AlwaysTruePQChecker final : public BaseSignatureChecker
{
public:
    bool CheckPQSignature(Span<const unsigned char>, Span<const unsigned char>, PQAlgorithm, uint8_t, SigVersion, ScriptExecutionData&, bool) const override
    {
        return true;
    }
};

struct P2MRSpendContext {
    CMutableTransaction tx_credit;
    CMutableTransaction tx_spend;
    PrecomputedTransactionData txdata;

    explicit P2MRSpendContext(const CScript& script_pub_key)
        : tx_credit(BuildCreditingTransaction(script_pub_key, /*nValue=*/5000)),
          tx_spend(BuildSpendingTransaction(CScript{}, CScriptWitness{}, CTransaction{tx_credit}))
    {
        txdata.Init(tx_spend, {tx_credit.vout.at(0)}, /*force=*/true);
    }
};

std::vector<unsigned char> ToBytes(const uint256& hash)
{
    return std::vector<unsigned char>(hash.begin(), hash.end());
}

CShieldedBundle BuildCTVV2ShieldedBundle()
{
    using namespace shielded::v2;

    const auto spend_account = MakeSmileAccount(0x30);
    const uint256 spend_note_commitment = uint256{0x33};
    const auto registry_witness =
        test::shielded::MakeSingleLeafRegistryWitness(spend_note_commitment, spend_account);
    BOOST_REQUIRE(registry_witness.has_value());

    EncryptedNotePayload encrypted_note;
    encrypted_note.scan_domain = ScanDomain::USER;
    encrypted_note.scan_hint.fill(0x29);
    encrypted_note.ciphertext = {0x90, 0x91, 0x92};
    encrypted_note.ephemeral_key = ComputeLegacyPayloadEphemeralKey(
        Span<const uint8_t>{encrypted_note.ciphertext.data(), encrypted_note.ciphertext.size()});

    const uint256 spend_anchor{0x32};

    SpendDescription spend;
    spend.nullifier = uint256{0x31};
    spend.merkle_anchor = spend_anchor;
    spend.account_leaf_commitment = registry_witness->second.account_leaf_commitment;
    spend.account_registry_proof = registry_witness->second;
    spend.note_commitment = spend_note_commitment;
    spend.value_commitment = uint256{0x34};

    OutputDescription output;
    output.note_class = NoteClass::USER;
    output.smile_account = MakeSmileAccount(0x35);
    output.note_commitment = smile2::ComputeCompactPublicAccountHash(*output.smile_account);
    output.value_commitment = smile2::ComputeSmileOutputCoinHash(output.smile_account->public_coin);
    output.encrypted_note = encrypted_note;

    SendPayload payload;
    payload.spend_anchor = spend_anchor;
    payload.account_registry_anchor = registry_witness->first;
    payload.spends = {spend};
    payload.outputs = {output};
    payload.fee = 9;
    payload.value_balance = payload.fee;

    ProofEnvelope envelope;
    envelope.proof_kind = ProofKind::DIRECT_SMILE;
    envelope.membership_proof_kind = ProofComponentKind::SMILE_MEMBERSHIP;
    envelope.amount_proof_kind = ProofComponentKind::SMILE_BALANCE;
    envelope.balance_proof_kind = ProofComponentKind::SMILE_BALANCE;
    envelope.settlement_binding_kind = SettlementBindingKind::NONE;
    envelope.statement_digest = uint256{0x38};

    TransactionBundle tx_bundle;
    tx_bundle.header.family_id = TransactionFamily::V2_SEND;
    tx_bundle.header.proof_envelope = envelope;
    tx_bundle.header.payload_digest = ComputeSendPayloadDigest(payload);
    tx_bundle.payload = payload;
    tx_bundle.proof_payload = {0xAA, 0xBB, 0xCC};

    CShieldedBundle bundle;
    bundle.v2_bundle = tx_bundle;
    return bundle;
}

CScript BuildP2MROutput(const uint256& merkle_root);

std::vector<unsigned char> BuildP2MRLeafWithSize(size_t size)
{
    std::vector<unsigned char> script(size, static_cast<unsigned char>(OP_NOP));
    if (!script.empty()) {
        script.front() = static_cast<unsigned char>(OP_1);
    }
    return script;
}

std::vector<unsigned char> BuildOversizedPushLeafScript(size_t push_size)
{
    std::vector<unsigned char> script;
    script.reserve(5 + push_size);
    script.push_back(static_cast<unsigned char>(OP_PUSHDATA4));
    script.push_back(static_cast<unsigned char>(push_size & 0xFF));
    script.push_back(static_cast<unsigned char>((push_size >> 8) & 0xFF));
    script.push_back(static_cast<unsigned char>((push_size >> 16) & 0xFF));
    script.push_back(static_cast<unsigned char>((push_size >> 24) & 0xFF));
    script.insert(script.end(), push_size, 0x42);
    return script;
}

std::vector<unsigned char> BuildCTVOnlyLeafScript(const uint256& ctv_hash)
{
    CScript script;
    script << ToBytes(ctv_hash) << OP_CHECKTEMPLATEVERIFY;
    return std::vector<unsigned char>(script.begin(), script.end());
}

std::vector<unsigned char> BuildCTVChecksigLeafScript(const uint256& ctv_hash, PQAlgorithm algo, Span<const unsigned char> pubkey)
{
    CScript prefix;
    prefix << ToBytes(ctv_hash) << OP_CHECKTEMPLATEVERIFY << OP_DROP;
    std::vector<unsigned char> script(prefix.begin(), prefix.end());
    const std::vector<unsigned char> checksig_leaf = BuildP2MRScript(algo, pubkey);
    script.insert(script.end(), checksig_leaf.begin(), checksig_leaf.end());
    return script;
}

uint256 ComputeCTVHashForTemplateSpend()
{
    const CMutableTransaction tx_credit = BuildCreditingTransaction(BuildP2MROutput(uint256::ONE), /*nValue=*/5000);
    const CMutableTransaction tx_spend = BuildSpendingTransaction(CScript{}, CScriptWitness{}, CTransaction{tx_credit});
    PrecomputedTransactionData txdata;
    txdata.Init(tx_spend, {tx_credit.vout.at(0)}, /*force=*/true);
    return ComputeCTVHash(CTransaction{tx_spend}, /*nIn=*/0, txdata);
}

uint256 ComputeCTVHashForInputIndexOneTemplate()
{
    CMutableTransaction tx;
    tx.version = 1;
    tx.nLockTime = 0;
    tx.vin.resize(2);
    tx.vout.resize(1);
    tx.vin[0].nSequence = CTxIn::SEQUENCE_FINAL;
    tx.vin[1].nSequence = CTxIn::SEQUENCE_FINAL;
    tx.vout[0].nValue = 5000;
    tx.vout[0].scriptPubKey = CScript{};

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);
    return ComputeCTVHash(tx, /*nIn=*/1, txdata);
}

template <typename T>
std::vector<unsigned char> BuildCTVPreimage(const T& tx, uint32_t nIn, const PrecomputedTransactionData& txdata)
{
    DataStream preimage{};
    preimage << tx.version;
    preimage << tx.nLockTime;
    if (txdata.m_ctv_has_scriptsigs) {
        preimage << txdata.m_ctv_scriptsigs_hash;
    }
    preimage << static_cast<uint32_t>(tx.vin.size());
    preimage << txdata.m_sequences_single_hash;
    preimage << static_cast<uint32_t>(tx.vout.size());
    preimage << txdata.m_outputs_single_hash;
    if (txdata.m_ctv_has_shielded_bundle) {
        preimage << txdata.m_ctv_shielded_bundle_hash;
    }
    preimage << nIn;
    return std::vector<unsigned char>(UCharCast(preimage.data()), UCharCast(preimage.data()) + preimage.size());
}

CScript BuildP2MROutput(const uint256& merkle_root)
{
    CScript script;
    script << OP_2 << ToBytes(merkle_root);
    return script;
}

CBlock BuildSingleCoinbaseBlock(const std::vector<CTxOut>& outputs)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript{} << 1 << OP_0;
    coinbase.vout = outputs;

    CBlock block;
    block.vtx.emplace_back(MakeTransactionRef(std::move(coinbase)));
    return block;
}

std::optional<uint256> ComputeP2MRSighash(
    P2MRSpendContext& ctx,
    Span<const unsigned char> leaf_script,
    uint8_t leaf_version = P2MR_LEAF_VERSION,
    uint8_t hash_type = SIGHASH_DEFAULT,
    const std::vector<unsigned char>* annex = nullptr)
{
    ScriptExecutionData execdata;
    if (annex != nullptr) {
        execdata.m_annex_hash = (HashWriter{} << *annex).GetSHA256();
        execdata.m_annex_present = true;
    } else {
        execdata.m_annex_present = false;
    }
    execdata.m_annex_init = true;
    execdata.m_tapleaf_hash = ComputeP2MRLeafHash(leaf_version, leaf_script);
    execdata.m_tapleaf_hash_init = true;
    execdata.m_codeseparator_pos = 0xFFFFFFFFU;
    execdata.m_codeseparator_pos_init = true;

    uint256 sighash;
    if (!SignatureHashSchnorr(
            sighash,
            execdata,
            ctx.tx_spend,
            /*in_pos=*/0,
            hash_type,
            SigVersion::P2MR,
            ctx.txdata,
            MissingDataBehavior::ASSERT_FAIL)) {
        return std::nullopt;
    }
    return sighash;
}

std::optional<CScriptWitness> BuildSignedMultisigLeafP2MRWitness(
    P2MRSpendContext& ctx,
    const std::vector<CPQKey>& keys_in_script_order,
    size_t threshold,
    Span<const unsigned char> leaf_script)
{
    if (keys_in_script_order.empty()) return std::nullopt;
    if (threshold < 1 || threshold > keys_in_script_order.size()) return std::nullopt;

    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    if (!sighash.has_value()) return std::nullopt;

    std::vector<std::vector<unsigned char>> sigs(keys_in_script_order.size());
    for (size_t i = 0; i < threshold; ++i) {
        if (!keys_in_script_order[i].Sign(*sighash, sigs[i])) return std::nullopt;
    }

    CScriptWitness witness;
    witness.stack.reserve(sigs.size() + 2);
    for (size_t i = sigs.size(); i > 0; --i) {
        witness.stack.push_back(std::move(sigs[i - 1]));
    }
    witness.stack.push_back(std::vector<unsigned char>(leaf_script.begin(), leaf_script.end()));
    witness.stack.push_back({P2MR_LEAF_VERSION});
    return witness;
}

bool VerifyP2MRSpend(P2MRSpendContext& ctx, const CScriptWitness& witness, ScriptError& error)
{
    ctx.tx_spend.vin.at(0).scriptWitness = witness;
    return VerifyScript(
        ctx.tx_spend.vin.at(0).scriptSig,
        ctx.tx_credit.vout.at(0).scriptPubKey,
        &ctx.tx_spend.vin.at(0).scriptWitness,
        P2MR_SCRIPT_FLAGS,
        MutableTransactionSignatureChecker(
            &ctx.tx_spend,
            /*nIn=*/0,
            ctx.tx_credit.vout.at(0).nValue,
            ctx.txdata,
            MissingDataBehavior::ASSERT_FAIL),
        &error);
}

bool VerifyP2MRSpendWithFlags(P2MRSpendContext& ctx, const CScriptWitness& witness, unsigned int flags, ScriptError& error)
{
    ctx.tx_spend.vin.at(0).scriptWitness = witness;
    return VerifyScript(
        ctx.tx_spend.vin.at(0).scriptSig,
        ctx.tx_credit.vout.at(0).scriptPubKey,
        &ctx.tx_spend.vin.at(0).scriptWitness,
        flags,
        MutableTransactionSignatureChecker(
            &ctx.tx_spend,
            /*nIn=*/0,
            ctx.tx_credit.vout.at(0).nValue,
            ctx.txdata,
            MissingDataBehavior::ASSERT_FAIL),
        &error);
}

uint256 ComputeCSFSHash(Span<const unsigned char> msg)
{
    HashWriter hasher = HASHER_CSFS;
    hasher.write(MakeByteSpan(msg));
    return hasher.GetSHA256();
}

std::vector<unsigned char> CreateCSFSSignature(const CPQKey& key, Span<const unsigned char> msg)
{
    std::vector<unsigned char> sig;
    const bool ok = key.Sign(ComputeCSFSHash(msg), sig);
    assert(ok);
    return sig;
}

bool EvalP2MRScript(std::vector<std::vector<unsigned char>>& stack,
                    const CScript& script,
                    unsigned int flags,
                    const BaseSignatureChecker& checker,
                    ScriptExecutionData& execdata,
                    ScriptError& err)
{
    return EvalScript(stack, script, flags, checker, SigVersion::P2MR, execdata, &err);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(pq_consensus_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(witness_v2_recognized_as_p2mr)
{
    const uint256 merkle_root = uint256::ONE;
    const CScript script_pub_key = BuildP2MROutput(merkle_root);

    int version = -1;
    std::vector<unsigned char> program;
    BOOST_REQUIRE(script_pub_key.IsWitnessProgram(version, program));
    BOOST_CHECK_EQUAL(version, 2);
    BOOST_CHECK_EQUAL(program.size(), WITNESS_V2_P2MR_SIZE);
}

BOOST_AUTO_TEST_CASE(p2mr_single_leaf_mldsa_valid_spend)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf_hash});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(*sighash, signature));

    CScriptWitness witness;
    witness.stack = {signature, leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(p2mr_single_leaf_mldsa_invalid_sig_fails)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf_hash});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(*sighash, signature));
    signature.front() ^= 0x01;

    CScriptWitness witness;
    witness.stack = {signature, leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SIG_MLDSA);
}

BOOST_AUTO_TEST_CASE(p2mr_two_leaf_spend_leaf0_mldsa)
{
    CPQKey ml_key;
    ml_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(ml_key.IsValid());
    CPQKey slh_key;
    slh_key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(slh_key.IsValid());

    const std::vector<unsigned char> leaf0_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, ml_key.GetPubKey());
    const std::vector<unsigned char> leaf1_script = BuildP2MRScript(PQAlgorithm::SLH_DSA_128S, slh_key.GetPubKey());

    const uint256 leaf0 = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf0_script);
    const uint256 leaf1 = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf1_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf0, leaf1});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash = ComputeP2MRSighash(ctx, leaf0_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(ml_key.Sign(*sighash, signature));

    std::vector<unsigned char> control{P2MR_LEAF_VERSION};
    control.insert(control.end(), leaf1.begin(), leaf1.end());

    CScriptWitness witness;
    witness.stack = {signature, leaf0_script, control};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(p2mr_two_leaf_spend_leaf1_slhdsa)
{
    CPQKey ml_key;
    ml_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(ml_key.IsValid());
    CPQKey slh_key;
    slh_key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(slh_key.IsValid());

    const std::vector<unsigned char> leaf0_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, ml_key.GetPubKey());
    const std::vector<unsigned char> leaf1_script = BuildP2MRScript(PQAlgorithm::SLH_DSA_128S, slh_key.GetPubKey());

    const uint256 leaf0 = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf0_script);
    const uint256 leaf1 = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf1_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf0, leaf1});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash = ComputeP2MRSighash(ctx, leaf1_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(slh_key.Sign(*sighash, signature));

    std::vector<unsigned char> control{P2MR_LEAF_VERSION};
    control.insert(control.end(), leaf0.begin(), leaf0.end());

    CScriptWitness witness;
    witness.stack = {signature, leaf1_script, control};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(p2mr_wrong_merkle_proof_fails)
{
    CPQKey ml_key;
    ml_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(ml_key.IsValid());
    CPQKey slh_key;
    slh_key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(slh_key.IsValid());

    const std::vector<unsigned char> leaf0_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, ml_key.GetPubKey());
    const std::vector<unsigned char> leaf1_script = BuildP2MRScript(PQAlgorithm::SLH_DSA_128S, slh_key.GetPubKey());

    const uint256 leaf0 = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf0_script);
    const uint256 leaf1 = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf1_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf0, leaf1});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash = ComputeP2MRSighash(ctx, leaf0_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(ml_key.Sign(*sighash, signature));

    std::vector<unsigned char> control{P2MR_LEAF_VERSION};
    control.insert(control.end(), leaf1.begin(), leaf1.end());
    control.back() ^= 0x01;

    CScriptWitness witness;
    witness.stack = {signature, leaf0_script, control};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH);
}

BOOST_AUTO_TEST_CASE(p2mr_wrong_leaf_version_fails)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf_hash});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(*sighash, signature));

    CScriptWitness witness;
    witness.stack = {signature, leaf_script, {0xc0}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_P2MR_WRONG_LEAF_VERSION);
}

BOOST_AUTO_TEST_CASE(p2mr_empty_witness_fails)
{
    const uint256 merkle_root = uint256::ONE;
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    CScriptWitness witness;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY);
}

BOOST_AUTO_TEST_CASE(p2mr_wrong_control_size_fails)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf_hash});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(*sighash, signature));

    CScriptWitness witness;
    witness.stack = {signature, leaf_script, {P2MR_LEAF_VERSION, 0x00}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_P2MR_WRONG_CONTROL_SIZE);
}

BOOST_AUTO_TEST_CASE(p2mr_reserved_falcon_opsuccess_slots_succeed)
{
    const std::array<opcodetype, 3> reserved_slots{
        OP_CHECKSIG_FALCON,
        OP_CHECKSIGADD_FALCON,
        OP_CHECKSIGFROMSTACK_FALCON,
    };

    for (const opcodetype opcode : reserved_slots) {
        const std::vector<unsigned char> leaf_script{static_cast<unsigned char>(opcode)};
        const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
        const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf_hash});

        P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
        CScriptWitness witness;
        witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK_MESSAGE(VerifyP2MRSpend(ctx, witness, err), GetOpName(opcode));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
    }
}

BOOST_AUTO_TEST_CASE(p2mr_reserved_falcon_opsuccess_slot_discouraged_by_flag)
{
    const std::vector<unsigned char> leaf_script{static_cast<unsigned char>(OP_CHECKSIG_FALCON)};
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf_hash});

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    CScriptWitness witness;
    witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpendWithFlags(ctx, witness, P2MR_SCRIPT_FLAGS | SCRIPT_VERIFY_DISCOURAGE_OP_SUCCESS, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_DISCOURAGE_OP_SUCCESS);
}

// QTC-SECURITY-REVIEW N-1: a P2MR annex used to be consensus-valid (popped before
// the element-size check, so it could be arbitrarily large and inflate the
// per-input validation budget) while WitnessSigOps did not pop it and therefore
// scanned the control block and counted zero PQ sigops. The annex is now rejected
// at consensus with SCRIPT_ERR_P2MR_ANNEX_UNSUPPORTED, whether or not the
// signature committed to it. The sighash function itself still commits to the
// annex hash when told one is present, which is what the first check pins.
BOOST_AUTO_TEST_CASE(p2mr_annex_rejected_at_consensus)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({leaf_hash});
    const std::vector<unsigned char> annex{ANNEX_TAG, 0x01, 0x02, 0x03};

    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    const auto sighash_with_annex = ComputeP2MRSighash(ctx, leaf_script, P2MR_LEAF_VERSION, SIGHASH_DEFAULT, &annex);
    BOOST_REQUIRE(sighash_with_annex.has_value());
    const auto sighash_without_annex = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash_without_annex.has_value());
    BOOST_CHECK(*sighash_with_annex != *sighash_without_annex);

    std::vector<unsigned char> sig_with_annex;
    BOOST_REQUIRE(key.Sign(*sighash_with_annex, sig_with_annex));
    std::vector<unsigned char> sig_without_annex;
    BOOST_REQUIRE(key.Sign(*sighash_without_annex, sig_without_annex));

    {
        // Control: the same leaf spends fine without an annex.
        CScriptWitness witness;
        witness.stack = {sig_without_annex, leaf_script, {P2MR_LEAF_VERSION}};
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
    }

    {
        // Signature that committed to the annex: rejected before any signature check.
        CScriptWitness witness;
        witness.stack = {sig_with_annex, leaf_script, {P2MR_LEAF_VERSION}, annex};
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_P2MR_ANNEX_UNSUPPORTED);
    }

    {
        // Signature that did not commit to the annex: same rejection, not SIG_MLDSA.
        CScriptWitness witness;
        witness.stack = {sig_without_annex, leaf_script, {P2MR_LEAF_VERSION}, annex};
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_P2MR_ANNEX_UNSUPPORTED);
    }

    {
        // An oversized annex can no longer buy validation budget: it is rejected as
        // an annex, before the element-size check that it used to bypass.
        std::vector<unsigned char> big_annex(5 * MAX_P2MR_ELEMENT_SIZE, 0x00);
        big_annex.front() = ANNEX_TAG;
        CScriptWitness witness;
        witness.stack = {sig_without_annex, leaf_script, {P2MR_LEAF_VERSION}, big_annex};
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_P2MR_ANNEX_UNSUPPORTED);
    }

    {
        // Only a third-or-later trailing element is an annex candidate (the shared
        // detection condition is stack.size() >= 3). A two-element witness whose
        // control block happens to start with 0x50 is a bad leaf version, not an annex.
        CScriptWitness witness;
        witness.stack = {leaf_script, {ANNEX_TAG}};
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_P2MR_WRONG_LEAF_VERSION);
    }
}

BOOST_AUTO_TEST_CASE(op_checksig_mldsa_pops_correct_stack)
{
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x01);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x02);
    const std::vector<unsigned char> script_bytes = BuildP2MRScript(PQAlgorithm::ML_DSA_44, pubkey);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const StubPQChecker checker;

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(stack.back().size(), 1U);
    BOOST_CHECK_EQUAL(stack.back().front(), 1U);
}

BOOST_AUTO_TEST_CASE(op_checksig_mldsa_disallowed_flag_fails)
{
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x01);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x02);
    const std::vector<unsigned char> script_bytes = BuildP2MRScript(PQAlgorithm::ML_DSA_44, pubkey);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const StubPQChecker checker;

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_DISALLOW_MLDSA, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_DISABLED_OPCODE);
}

BOOST_AUTO_TEST_CASE(op_checksig_mldsa_wrong_pubkey_size_fails)
{
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x01);
    const std::vector<unsigned char> script_bytes{
        static_cast<unsigned char>(OP_PUSHDATA2),
        0x01,
        0x00,
        0x02,
        static_cast<unsigned char>(OP_CHECKSIG_MLDSA),
    };
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const StubPQChecker checker;

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_PUBKEY_SIZE);
}

BOOST_AUTO_TEST_CASE(op_checksig_slhdsa_pops_correct_stack)
{
    const std::vector<unsigned char> signature(SLHDSA128S_SIGNATURE_SIZE, 0x03);
    const std::vector<unsigned char> pubkey(SLHDSA128S_PUBKEY_SIZE, 0x04);
    const std::vector<unsigned char> script_bytes = BuildP2MRScript(PQAlgorithm::SLH_DSA_128S, pubkey);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const StubPQChecker checker;

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(stack.back().size(), 1U);
    BOOST_CHECK_EQUAL(stack.back().front(), 1U);
}

BOOST_AUTO_TEST_CASE(op_checksig_slhdsa_with_mldsa_disable_flag_still_succeeds)
{
    const std::vector<unsigned char> signature(SLHDSA128S_SIGNATURE_SIZE, 0x03);
    const std::vector<unsigned char> pubkey(SLHDSA128S_PUBKEY_SIZE, 0x04);
    const std::vector<unsigned char> script_bytes = BuildP2MRScript(PQAlgorithm::SLH_DSA_128S, pubkey);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const StubPQChecker checker;

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_DISALLOW_MLDSA, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(stack.back().size(), 1U);
    BOOST_CHECK_EQUAL(stack.back().front(), 1U);
}

BOOST_AUTO_TEST_CASE(op_checksig_pq_rejected_in_tapscript)
{
    const StubPQChecker checker;
    ScriptExecutionData execdata;
    for (const auto opcode : {OP_CHECKSIG_MLDSA, OP_CHECKSIG_SLHDSA}) {
        std::vector<std::vector<unsigned char>> stack;
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        const CScript script{opcode};
        BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::TAPSCRIPT, execdata, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_BAD_OPCODE);
    }
}

BOOST_AUTO_TEST_CASE(op_checksigadd_mldsa_increments_counter)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x61);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x62);

    CScript script;
    script << pubkey << OP_CHECKSIGADD_MLDSA;

    std::vector<std::vector<unsigned char>> stack{signature, CScriptNum{7}.getvch()};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP + 3;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(CScriptNum(stack.back(), /*fRequireMinimal=*/true).getint(), 8);
    BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, 3);
}

BOOST_AUTO_TEST_CASE(op_checksigadd_mldsa_disallowed_flag_fails)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x61);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x62);

    CScript script;
    script << pubkey << OP_CHECKSIGADD_MLDSA;

    std::vector<std::vector<unsigned char>> stack{signature, CScriptNum{7}.getvch()};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP + 3;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_DISALLOW_MLDSA, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_DISABLED_OPCODE);
}


BOOST_AUTO_TEST_CASE(op_checksigadd_slhdsa_empty_sig_keeps_counter)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> pubkey(SLHDSA128S_PUBKEY_SIZE, 0x63);

    CScript script;
    script << pubkey << OP_CHECKSIGADD_SLHDSA;

    std::vector<std::vector<unsigned char>> stack{{}, CScriptNum{5}.getvch()};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_SLHDSA_MULTISIG_SIGOP + 9;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(CScriptNum(stack.back(), /*fRequireMinimal=*/true).getint(), 5);
    BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, VALIDATION_WEIGHT_PER_SLHDSA_MULTISIG_SIGOP + 9);
}

BOOST_AUTO_TEST_CASE(op_checksigadd_rejected_outside_p2mr)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature{0x64};
    const std::vector<unsigned char> pubkey{0x65};
    const std::vector<unsigned char> counter = CScriptNum{1}.getvch();

    for (const auto opcode : {OP_CHECKSIGADD_MLDSA, OP_CHECKSIGADD_SLHDSA}) {
        CScript script;
        script << pubkey << opcode;
        std::vector<std::vector<unsigned char>> stack{signature, counter};
        ScriptExecutionData execdata;
        execdata.m_validation_weight_left_init = true;
        execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP;
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::TAPSCRIPT, execdata, &err));
        BOOST_CHECK_EQUAL(err, SCRIPT_ERR_BAD_OPCODE);
    }
}

BOOST_AUTO_TEST_CASE(op_checksigadd_weight_exhaustion_fails)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x66);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x67);

    CScript script;
    script << pubkey << OP_CHECKSIGADD_MLDSA;

    std::vector<std::vector<unsigned char>> stack{signature, CScriptNum{0}.getvch()};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP - 1;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT);
}

BOOST_AUTO_TEST_CASE(op_checksigadd_wrong_pubkey_size_fails)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x68);

    CScript script;
    script << std::vector<unsigned char>{0x01} << OP_CHECKSIGADD_MLDSA;

    std::vector<std::vector<unsigned char>> stack{signature, CScriptNum{0}.getvch()};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_PUBKEY_SIZE);
}

BOOST_AUTO_TEST_CASE(op_checksigadd_requires_minimal_counter_encoding)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x69);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x6A);

    CScript script;
    script << pubkey << OP_CHECKSIGADD_MLDSA;

    // Non-minimal encoding of value 1.
    std::vector<std::vector<unsigned char>> stack{signature, std::vector<unsigned char>{0x01, 0x00}};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
}


BOOST_AUTO_TEST_CASE(p2mr_existing_primitives_fit_element_limit)
{
    BOOST_CHECK(MLDSA44_PUBKEY_SIZE <= MAX_P2MR_ELEMENT_SIZE);
    BOOST_CHECK(MLDSA44_SIGNATURE_SIZE + 1 <= MAX_P2MR_ELEMENT_SIZE);
    BOOST_CHECK(SLHDSA128S_PUBKEY_SIZE <= MAX_P2MR_ELEMENT_SIZE);
    BOOST_CHECK(SLHDSA128S_SIGNATURE_SIZE + 1 <= MAX_P2MR_ELEMENT_SIZE);
}

BOOST_AUTO_TEST_CASE(p2mr_checksig_mldsa_decrements_validation_weight)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x11);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x22);
    const std::vector<unsigned char> script_bytes = BuildP2MRScript(PQAlgorithm::ML_DSA_44, pubkey);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_SIGOP + 77;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, 77);
}

BOOST_AUTO_TEST_CASE(p2mr_checksig_slhdsa_decrements_validation_weight)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(SLHDSA128S_SIGNATURE_SIZE, 0x33);
    const std::vector<unsigned char> pubkey(SLHDSA128S_PUBKEY_SIZE, 0x44);
    const std::vector<unsigned char> script_bytes = BuildP2MRScript(PQAlgorithm::SLH_DSA_128S, pubkey);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_SLHDSA_SIGOP + 101;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, 101);
}

BOOST_AUTO_TEST_CASE(p2mr_checksig_weight_exhaustion_fails)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> signature(MLDSA44_SIGNATURE_SIZE, 0x51);
    const std::vector<unsigned char> pubkey(MLDSA44_PUBKEY_SIZE, 0x52);
    const std::vector<unsigned char> script_bytes = BuildP2MRScript(PQAlgorithm::ML_DSA_44, pubkey);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{signature};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_SIGOP - 1;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT);
}

BOOST_AUTO_TEST_CASE(p2mr_oversized_exec_stack_element_is_rejected)
{
    const std::vector<unsigned char> leaf_script{
        static_cast<unsigned char>(OP_DROP),
        static_cast<unsigned char>(OP_1),
    };
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    CScriptWitness witness;
    witness.stack = {
        std::vector<unsigned char>(MAX_P2MR_ELEMENT_SIZE + 1, 0x99),
        leaf_script,
        {P2MR_LEAF_VERSION},
    };

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PUSH_SIZE);
}

BOOST_AUTO_TEST_CASE(p2mr_oversized_push_value_is_rejected)
{
    const StubPQChecker checker;
    const std::vector<unsigned char> script_bytes = BuildOversizedPushLeafScript(MAX_P2MR_ELEMENT_SIZE + 1);
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack;
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PUSH_SIZE);
}

BOOST_AUTO_TEST_CASE(p2mr_script_size_cap_rejects_oversized_leaf)
{
    const std::vector<unsigned char> leaf_script = BuildP2MRLeafWithSize(MAX_P2MR_SCRIPT_SIZE + 1);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    CScriptWitness witness;
    witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SCRIPT_SIZE);
}

BOOST_AUTO_TEST_CASE(p2mr_script_size_cap_accepts_boundary_leaf)
{
    const std::vector<unsigned char> leaf_script = BuildP2MRLeafWithSize(MAX_P2MR_SCRIPT_SIZE);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    CScriptWitness witness;
    witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(ctv_preimage_without_scriptsigs_is_84_bytes)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.resize(1);
    tx.vin[0].nSequence = 7;
    tx.vout.resize(1);
    tx.vout[0].nValue = 10'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);
    BOOST_REQUIRE(!txdata.m_ctv_has_scriptsigs);
    BOOST_CHECK_EQUAL(BuildCTVPreimage(tx, 0, txdata).size(), 84U);
}

BOOST_AUTO_TEST_CASE(ctv_preimage_with_scriptsigs_is_116_bytes)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = CScript{} << OP_1;
    tx.vin[0].nSequence = 7;
    tx.vout.resize(1);
    tx.vout[0].nValue = 10'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);
    BOOST_REQUIRE(txdata.m_ctv_has_scriptsigs);
    BOOST_CHECK_EQUAL(BuildCTVPreimage(tx, 0, txdata).size(), 116U);
}

BOOST_AUTO_TEST_CASE(ctv_preimage_with_shielded_bundle_adds_32_byte_digest)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.resize(1);
    tx.vin[0].nSequence = 7;
    tx.vout.resize(1);
    tx.vout[0].nValue = 10'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    CShieldedBundle bundle;
    bundle.value_balance = -200;
    CShieldedInput input;
    input.nullifier = uint256::ONE;
    input.ring_positions = std::vector<uint64_t>(16, 0);
    bundle.shielded_inputs.push_back(input);
    CShieldedOutput output;
    output.note_commitment = uint256::ONE;
    bundle.shielded_outputs.push_back(output);
    tx.shielded_bundle = std::move(bundle);

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);
    BOOST_REQUIRE(!txdata.m_ctv_has_scriptsigs);
    BOOST_REQUIRE(txdata.m_ctv_has_shielded_bundle);
    BOOST_CHECK(!txdata.m_ctv_shielded_bundle_hash.IsNull());
    BOOST_CHECK_EQUAL(BuildCTVPreimage(tx, 0, txdata).size(), 116U);
}

BOOST_AUTO_TEST_CASE(ctv_hash_commits_to_shielded_bundle_fields)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.resize(1);
    tx.vin[0].nSequence = 7;
    tx.vout.resize(1);
    tx.vout[0].nValue = 10'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    CShieldedBundle bundle;
    bundle.value_balance = -200;
    CShieldedInput input;
    input.nullifier = uint256::ONE;
    input.ring_positions = std::vector<uint64_t>(16, 0);
    bundle.shielded_inputs.push_back(input);
    CShieldedOutput output;
    output.note_commitment = uint256::ONE;
    bundle.shielded_outputs.push_back(output);
    CViewGrant grant;
    grant.kem_ct.fill(0x11);
    grant.nonce.fill(0x22);
    grant.encrypted_data = {0x33, 0x44, 0x55};
    bundle.view_grants.push_back(grant);
    bundle.proof = {0xAA, 0xBB, 0xCC};
    tx.shielded_bundle = std::move(bundle);

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);
    HashWriter shielded_hw{};
    shielded_hw << tx.shielded_bundle.value_balance;
    shielded_hw << tx.shielded_bundle.shielded_inputs;
    shielded_hw << tx.shielded_bundle.shielded_outputs;
    shielded_hw << tx.shielded_bundle.view_grants;
    shielded_hw << tx.shielded_bundle.proof;
    BOOST_CHECK_EQUAL(txdata.m_ctv_shielded_bundle_hash, shielded_hw.GetSHA256());
    const uint256 baseline = ComputeCTVHash(tx, 0, txdata);

    CMutableTransaction tx_value_balance = tx;
    tx_value_balance.shielded_bundle.value_balance = -201;
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_value_balance, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_value_balance, 0, txdata) != baseline);

    CMutableTransaction tx_output_commitment = tx;
    tx_output_commitment.shielded_bundle.shielded_outputs[0].note_commitment = uint256{};
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_output_commitment, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_output_commitment, 0, txdata) != baseline);

    CMutableTransaction tx_input_nullifier = tx;
    tx_input_nullifier.shielded_bundle.shielded_inputs[0].nullifier = uint256{};
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_input_nullifier, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_input_nullifier, 0, txdata) != baseline);

    CMutableTransaction tx_ring_positions = tx;
    tx_ring_positions.shielded_bundle.shielded_inputs[0].ring_positions[0] ^= 1;
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_ring_positions, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_ring_positions, 0, txdata) != baseline);

    CMutableTransaction tx_view_grant = tx;
    tx_view_grant.shielded_bundle.view_grants[0].encrypted_data[0] ^= 1;
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_view_grant, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_view_grant, 0, txdata) != baseline);

    CMutableTransaction tx_proof = tx;
    tx_proof.shielded_bundle.proof[0] ^= 1;
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_proof, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_proof, 0, txdata) != baseline);
}

BOOST_AUTO_TEST_CASE(ctv_hash_commits_to_v2_shielded_bundle_bytes)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.resize(1);
    tx.vin[0].nSequence = 7;
    tx.vout.resize(1);
    tx.vout[0].nValue = 10'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;
    tx.shielded_bundle = BuildCTVV2ShieldedBundle();

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);
    BOOST_REQUIRE(txdata.m_ctv_has_shielded_bundle);
    BOOST_CHECK_EQUAL(txdata.m_ctv_shielded_bundle_hash, ComputeShieldedBundleCtvHash(tx.shielded_bundle));
    const uint256 baseline = ComputeCTVHash(tx, 0, txdata);

    CMutableTransaction tx_payload = tx;
    tx_payload.shielded_bundle.v2_bundle->proof_payload[0] ^= 0x01;
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_payload, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_payload, 0, txdata) != baseline);

    CMutableTransaction tx_output = tx;
    auto& output_payload =
        std::get<shielded::v2::SendPayload>(tx_output.shielded_bundle.v2_bundle->payload);
    BOOST_REQUIRE(output_payload.outputs[0].smile_account.has_value());
    output_payload.outputs[0].smile_account->public_coin.t_msg[0].coeffs[0] += 1;
    output_payload.outputs[0].note_commitment =
        smile2::ComputeCompactPublicAccountHash(*output_payload.outputs[0].smile_account);
    tx_output.shielded_bundle.v2_bundle->header.payload_digest =
        shielded::v2::ComputeSendPayloadDigest(output_payload);
    txdata = PrecomputedTransactionData{};
    txdata.Init(tx_output, {}, /*force=*/true);
    BOOST_CHECK(ComputeCTVHash(tx_output, 0, txdata) != baseline);
}

BOOST_AUTO_TEST_CASE(ctv_roundtrip_happy_path_p2mr)
{
    const uint256 ctv_hash = ComputeCTVHashForTemplateSpend();
    const std::vector<unsigned char> leaf_script = BuildCTVOnlyLeafScript(ctv_hash);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    CScriptWitness witness;
    witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(ctv_mismatch_outputs_fails)
{
    const uint256 ctv_hash = ComputeCTVHashForTemplateSpend();
    const std::vector<unsigned char> leaf_script = BuildCTVOnlyLeafScript(ctv_hash);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    ctx.tx_spend.vout[0].nValue -= 1;
    ctx.txdata = PrecomputedTransactionData{};
    ctx.txdata.Init(ctx.tx_spend, {ctx.tx_credit.vout.at(0)}, /*force=*/true);

    CScriptWitness witness;
    witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CTV_HASH_MISMATCH);
}

BOOST_AUTO_TEST_CASE(ctv_mismatch_locktime_fails)
{
    const uint256 ctv_hash = ComputeCTVHashForTemplateSpend();
    const std::vector<unsigned char> leaf_script = BuildCTVOnlyLeafScript(ctv_hash);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    ctx.tx_spend.nLockTime = 42;
    ctx.txdata = PrecomputedTransactionData{};
    ctx.txdata.Init(ctx.tx_spend, {ctx.tx_credit.vout.at(0)}, /*force=*/true);

    CScriptWitness witness;
    witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CTV_HASH_MISMATCH);
}

BOOST_AUTO_TEST_CASE(ctv_mismatch_input_index_fails)
{
    const uint256 ctv_hash = ComputeCTVHashForInputIndexOneTemplate();
    const std::vector<unsigned char> leaf_script = BuildCTVOnlyLeafScript(ctv_hash);
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    CScriptWitness witness;
    witness.stack = {leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CTV_HASH_MISMATCH);
}

BOOST_AUTO_TEST_CASE(ctv_cleanstack_no_pop_behavior)
{
    const StubCTVChecker checker;
    std::vector<std::vector<unsigned char>> stack;
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    const std::vector<unsigned char> hash32(32, 0xAB);
    CScript script;
    script << hash32 << OP_CHECKTEMPLATEVERIFY;

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_CHECKTEMPLATEVERIFY, checker, SigVersion::P2MR, execdata, &err));
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back() == hash32);
}

BOOST_AUTO_TEST_CASE(ctv_non_32_byte_arg_fails_20_bytes)
{
    const StubCTVChecker checker;
    std::vector<std::vector<unsigned char>> stack{std::vector<unsigned char>(20, 0x01)};
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKTEMPLATEVERIFY};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_CHECKTEMPLATEVERIFY, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CTV_HASH_SIZE);
}

BOOST_AUTO_TEST_CASE(ctv_non_32_byte_arg_fails_33_bytes)
{
    const StubCTVChecker checker;
    std::vector<std::vector<unsigned char>> stack{std::vector<unsigned char>(33, 0x01)};
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKTEMPLATEVERIFY};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_CHECKTEMPLATEVERIFY, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CTV_HASH_SIZE);
}

BOOST_AUTO_TEST_CASE(ctv_non_32_byte_arg_fails_empty)
{
    const StubCTVChecker checker;
    std::vector<std::vector<unsigned char>> stack{std::vector<unsigned char>()};
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKTEMPLATEVERIFY};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_CHECKTEMPLATEVERIFY, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_CTV_HASH_SIZE);
}

BOOST_AUTO_TEST_CASE(ctv_empty_stack_fails)
{
    const StubCTVChecker checker;
    std::vector<std::vector<unsigned char>> stack;
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKTEMPLATEVERIFY};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_CHECKTEMPLATEVERIFY, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

BOOST_AUTO_TEST_CASE(ctv_hash_differs_by_input_index)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 3;
    tx.vin.resize(2);
    tx.vin[0].nSequence = 1;
    tx.vin[1].nSequence = 2;
    tx.vout.resize(1);
    tx.vout[0].nValue = 5'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);

    const uint256 hash0 = ComputeCTVHash(tx, 0, txdata);
    const uint256 hash1 = ComputeCTVHash(tx, 1, txdata);
    BOOST_CHECK(hash0 != hash1);
}

BOOST_AUTO_TEST_CASE(ctv_flag_unset_behaves_as_nop)
{
    const StubCTVChecker checker;
    std::vector<unsigned char> hash32(32, 0x11);
    std::vector<std::vector<unsigned char>> stack{hash32};
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKTEMPLATEVERIFY};

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_NONE, checker, SigVersion::P2MR, execdata, &err));
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back() == hash32);
}

BOOST_AUTO_TEST_CASE(ctv_flag_unset_with_discourage_nops_fails)
{
    const StubCTVChecker checker;
    std::vector<std::vector<unsigned char>> stack{std::vector<unsigned char>(32, 0x11)};
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKTEMPLATEVERIFY};

    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS, checker, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);
}

BOOST_AUTO_TEST_CASE(ctv_flag_set_non_p2mr_context_is_nop)
{
    const StubCTVChecker checker;
    std::vector<unsigned char> hash32(32, 0x11);
    std::vector<std::vector<unsigned char>> stack{hash32};
    ScriptExecutionData execdata;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKTEMPLATEVERIFY};

    BOOST_REQUIRE(EvalScript(stack, script, SCRIPT_VERIFY_CHECKTEMPLATEVERIFY, checker, SigVersion::BASE, execdata, &err));
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back() == hash32);
}

BOOST_AUTO_TEST_CASE(ctv_check_missing_txdata_fails_safely)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.resize(1);
    MutableTransactionSignatureChecker checker(&tx, /*nIn=*/0, /*amount=*/0, MissingDataBehavior::FAIL);
    const std::vector<unsigned char> hash32(32, 0x01);
    BOOST_CHECK(!checker.CheckCTVHash(hash32));
}

BOOST_AUTO_TEST_CASE(ctv_sequences_subhash_matches_manual)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 9;
    tx.vin.resize(2);
    tx.vin[0].nSequence = 1;
    tx.vin[1].nSequence = 2;
    tx.vout.resize(1);
    tx.vout[0].nValue = 9'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);

    HashWriter ss{};
    for (const auto& txin : tx.vin) {
        ss << txin.nSequence;
    }
    BOOST_CHECK_EQUAL(txdata.m_sequences_single_hash, ss.GetSHA256());
}

BOOST_AUTO_TEST_CASE(ctv_outputs_subhash_matches_manual)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 9;
    tx.vin.resize(1);
    tx.vin[0].nSequence = 1;
    tx.vout.resize(2);
    tx.vout[0].nValue = 9'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;
    tx.vout[1].nValue = 8'000;
    tx.vout[1].scriptPubKey = CScript{} << OP_0;

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);

    HashWriter ss{};
    for (const auto& txout : tx.vout) {
        ss << txout;
    }
    BOOST_CHECK_EQUAL(txdata.m_outputs_single_hash, ss.GetSHA256());
}

BOOST_AUTO_TEST_CASE(ctv_scriptsigs_subhash_matches_manual)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 9;
    tx.vin.resize(2);
    tx.vin[0].scriptSig = CScript{} << OP_1;
    tx.vin[0].nSequence = 1;
    tx.vin[1].scriptSig = CScript{} << std::vector<unsigned char>{0xCA, 0xFE};
    tx.vin[1].nSequence = 2;
    tx.vout.resize(1);
    tx.vout[0].nValue = 9'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);

    HashWriter ss{};
    for (const auto& txin : tx.vin) {
        ss << txin.scriptSig;
    }
    BOOST_CHECK_EQUAL(txdata.m_ctv_scriptsigs_hash, ss.GetSHA256());
}

BOOST_AUTO_TEST_CASE(ctv_precompute_sets_ready_flag)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = 0;
    tx.vin.resize(1);
    tx.vin[0].nSequence = 1;
    tx.vout.resize(1);
    tx.vout[0].nValue = 1'000;
    tx.vout[0].scriptPubKey = CScript{} << OP_1;

    PrecomputedTransactionData txdata;
    txdata.Init(tx, {}, /*force=*/true);
    BOOST_CHECK(txdata.m_ctv_ready);
    BOOST_CHECK(!txdata.m_ctv_has_shielded_bundle);
    BOOST_CHECK(txdata.m_ctv_shielded_bundle_hash.IsNull());
}

BOOST_AUTO_TEST_CASE(ctv_and_checksig_combined_leaf_succeeds)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());

    const uint256 ctv_hash = ComputeCTVHashForTemplateSpend();
    const std::vector<unsigned char> leaf_script = BuildCTVChecksigLeafScript(ctv_hash, PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());

    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(*sighash, signature));

    CScriptWitness witness;
    witness.stack = {signature, leaf_script, {P2MR_LEAF_VERSION}};

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(csv_multisig_leaf_succeeds_when_sequence_is_met)
{
    CPQKey key1;
    key1.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key1.IsValid());
    CPQKey key2;
    key2.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(key2.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRCSVMultisigScript(
        /*sequence=*/7,
        /*threshold=*/1,
        {
            {PQAlgorithm::ML_DSA_44, key1.GetPubKey()},
            {PQAlgorithm::SLH_DSA_128S, key2.GetPubKey()},
        });
    BOOST_REQUIRE(!leaf_script.empty());
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    ctx.tx_spend.version = 2;
    ctx.tx_spend.vin.at(0).nSequence = 7;
    ctx.txdata = PrecomputedTransactionData{};
    ctx.txdata.Init(ctx.tx_spend, {ctx.tx_credit.vout.at(0)}, /*force=*/true);

    const std::vector<CPQKey> signer_keys{key1, key2};
    const auto witness = BuildSignedMultisigLeafP2MRWitness(ctx, signer_keys, /*threshold=*/1, leaf_script);
    BOOST_REQUIRE(witness.has_value());

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpendWithFlags(ctx, *witness, P2MR_SCRIPT_FLAGS | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(csv_multisig_leaf_fails_when_sequence_is_unmet)
{
    CPQKey key1;
    key1.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key1.IsValid());
    CPQKey key2;
    key2.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(key2.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRCSVMultisigScript(
        /*sequence=*/7,
        /*threshold=*/1,
        {
            {PQAlgorithm::ML_DSA_44, key1.GetPubKey()},
            {PQAlgorithm::SLH_DSA_128S, key2.GetPubKey()},
        });
    BOOST_REQUIRE(!leaf_script.empty());
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};
    ctx.tx_spend.version = 2;
    ctx.tx_spend.vin.at(0).nSequence = 6;
    ctx.txdata = PrecomputedTransactionData{};
    ctx.txdata.Init(ctx.tx_spend, {ctx.tx_credit.vout.at(0)}, /*force=*/true);

    const std::vector<CPQKey> signer_keys{key1, key2};
    const auto witness = BuildSignedMultisigLeafP2MRWitness(ctx, signer_keys, /*threshold=*/1, leaf_script);
    BOOST_REQUIRE(witness.has_value());

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!VerifyP2MRSpendWithFlags(ctx, *witness, P2MR_SCRIPT_FLAGS | SCRIPT_VERIFY_CHECKSEQUENCEVERIFY, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
}

BOOST_AUTO_TEST_CASE(csfs_mldsa_happy_path)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0xAA, 0xBB, 0xCC};
    const std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back() == std::vector<unsigned char>({1}));
}

BOOST_AUTO_TEST_CASE(csfs_slhdsa_happy_path)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x11, 0x22};
    const std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::SLH_DSA_128S, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back() == std::vector<unsigned char>({1}));
}

BOOST_AUTO_TEST_CASE(csfs_slhdsa_corrupted_signature_pushes_false)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x40, 0x41, 0x42};
    std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    sig[0] ^= 1;
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::SLH_DSA_128S, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back().empty());
}

BOOST_AUTO_TEST_CASE(csfs_slhdsa_empty_signature_pushes_false_and_does_not_consume_weight)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x01};
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::SLH_DSA_128S, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{{}, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 1337;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back().empty());
    BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, 1337);
}

BOOST_AUTO_TEST_CASE(csfs_slhdsa_nullfail_nonempty_failed_sig_errors)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x99, 0x98};
    std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    sig.back() ^= 0x80;
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::SLH_DSA_128S, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalP2MRScript(stack, script, SCRIPT_VERIFY_NULLFAIL, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SIG_SLHDSA);
}

BOOST_AUTO_TEST_CASE(csfs_corrupted_signature_pushes_false)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x40, 0x41, 0x42};
    std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    sig[0] ^= 1;
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back().empty());
}

BOOST_AUTO_TEST_CASE(csfs_empty_signature_pushes_false_and_does_not_consume_weight)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x01};
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{{}, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 1337;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back().empty());
    BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, 1337);
}

BOOST_AUTO_TEST_CASE(csfs_nullfail_nonempty_failed_sig_errors)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x99, 0x98};
    std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    sig.back() ^= 0x80;
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};

    BOOST_CHECK(!EvalP2MRScript(stack, script, SCRIPT_VERIFY_NULLFAIL, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SIG_MLDSA);
}

BOOST_AUTO_TEST_CASE(csfs_stack_underflow_fails)
{
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 1000;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKSIGFROMSTACK};

    std::vector<std::vector<unsigned char>> stack{};
    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);

    std::vector<std::vector<unsigned char>> stack2{{0x01}, {0x02}};
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_CHECK(!EvalScript(stack2, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_INVALID_STACK_OPERATION);
}

BOOST_AUTO_TEST_CASE(csfs_pubkey_size_rejections)
{
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 1000;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKSIGFROMSTACK};

    std::vector<std::vector<unsigned char>> bad33{std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01), {0x01}, std::vector<unsigned char>(33, 0x11)};
    BOOST_CHECK(!EvalScript(bad33, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_PUBKEY_SIZE);

    std::vector<std::vector<unsigned char>> bad0{std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01), {0x01}, {}};
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_CHECK(!EvalScript(bad0, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_PUBKEY_SIZE);

    std::vector<std::vector<unsigned char>> bad1311{std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01), {0x01}, std::vector<unsigned char>(MLDSA44_PUBKEY_SIZE - 1, 0x11)};
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_CHECK(!EvalScript(bad1311, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, SigVersion::P2MR, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_PQ_PUBKEY_SIZE);
}

BOOST_AUTO_TEST_CASE(csfs_tagged_hash_domain_separation)
{
    const std::vector<unsigned char> msg{0xDE, 0xAD, 0xBE, 0xEF};

    HashWriter tagged = HASHER_CSFS;
    tagged.write(MakeByteSpan(msg));
    const uint256 tagged_hash = tagged.GetSHA256();

    const uint256 plain_hash = Hash(msg);

    BOOST_CHECK(tagged_hash != plain_hash);
}

BOOST_AUTO_TEST_CASE(csfs_write_vs_stream_operator_hashing_differs)
{
    const std::vector<unsigned char> msg{0x00, 0x11, 0x22};

    HashWriter via_write = HASHER_CSFS;
    via_write.write(MakeByteSpan(msg));
    const uint256 hash_write = via_write.GetSHA256();

    HashWriter via_stream = HASHER_CSFS;
    via_stream << msg;
    const uint256 hash_stream = via_stream.GetSHA256();

    BOOST_CHECK(hash_write != hash_stream);
}

// CSFS has no transaction context, so it must not go through the sighash-computing
// CheckPQSignature path. Since QTC-SECURITY-REVIEW N-1 it goes through
// BaseSignatureChecker::VerifyPQSignature instead (see
// csfs_routes_verification_through_checker_verifypqsignature below); a checker that
// only overrides CheckPQSignature therefore still performs a real verification here.
BOOST_AUTO_TEST_CASE(csfs_does_not_use_checkpqsignature_path)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x14, 0x15};
    std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    sig[0] ^= 0x55;
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const AlwaysTruePQChecker checker;

    BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, checker, execdata, err));
    BOOST_CHECK_EQUAL(stack.size(), 1U);
    BOOST_CHECK(stack.back().empty());
}

BOOST_AUTO_TEST_CASE(csfs_sigversion_gating_bad_opcode)
{
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 1000;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const CScript script{OP_CHECKSIGFROMSTACK};

    std::vector<std::vector<unsigned char>> stack{{0x01}, {0x02}, {0x03}};
    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, SigVersion::BASE, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_BAD_OPCODE);

    err = SCRIPT_ERR_UNKNOWN_ERROR;
    stack = {{0x01}, {0x02}, {0x03}};
    BOOST_CHECK(!EvalScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, SigVersion::WITNESS_V0, execdata, &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_BAD_OPCODE);
}

BOOST_AUTO_TEST_CASE(csfs_message_size_boundaries)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());

    for (const size_t msg_len : {size_t{520}, size_t{0}, size_t{521}}) {
        std::vector<unsigned char> msg(msg_len, 0x77);
        const std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
        std::vector<std::vector<unsigned char>> stack{sig, msg};
        ScriptExecutionData execdata;
        execdata.m_validation_weight_left_init = true;
        execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
        BOOST_CHECK_EQUAL(stack.size(), 1U);
        BOOST_CHECK(stack.back() == std::vector<unsigned char>({1}));
    }
}

BOOST_AUTO_TEST_CASE(csfs_signature_size_enforced_exactly)
{
    CPQKey mldsa;
    mldsa.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(mldsa.IsValid());
    const std::vector<unsigned char> msg{0x22, 0x23};
    const std::vector<unsigned char> mldsa_script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, mldsa.GetPubKey());
    const CScript mldsa_script(mldsa_script_bytes.begin(), mldsa_script_bytes.end());

    std::vector<unsigned char> mldsa_plus_one = CreateCSFSSignature(mldsa, msg);
    mldsa_plus_one.push_back(SIGHASH_DEFAULT);
    std::vector<std::vector<unsigned char>> stack{mldsa_plus_one, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!EvalP2MRScript(stack, mldsa_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SIG_MLDSA);

    std::vector<std::vector<unsigned char>> wrong_size{std::vector<unsigned char>(100, 0x01), msg};
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_CHECK(!EvalP2MRScript(wrong_size, mldsa_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SIG_MLDSA);

    CPQKey slh;
    slh.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(slh.IsValid());
    const std::vector<unsigned char> slh_script_bytes = BuildP2MRCSFSScript(PQAlgorithm::SLH_DSA_128S, slh.GetPubKey());
    const CScript slh_script(slh_script_bytes.begin(), slh_script_bytes.end());
    std::vector<unsigned char> slh_plus_one = CreateCSFSSignature(slh, msg);
    slh_plus_one.push_back(SIGHASH_DEFAULT);
    std::vector<std::vector<unsigned char>> slh_stack{slh_plus_one, msg};
    ScriptExecutionData slh_exec;
    slh_exec.m_validation_weight_left_init = true;
    slh_exec.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_CHECK(!EvalP2MRScript(slh_stack, slh_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, slh_exec, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_SIG_SLHDSA);
}

BOOST_AUTO_TEST_CASE(csfs_validation_weight_accounting)
{
    CPQKey mldsa;
    mldsa.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(mldsa.IsValid());
    const std::vector<unsigned char> msg{0x51, 0x52};
    const std::vector<unsigned char> sig = CreateCSFSSignature(mldsa, msg);
    const std::vector<unsigned char> ml_script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, mldsa.GetPubKey());
    const CScript ml_script(ml_script_bytes.begin(), ml_script_bytes.end());

    std::vector<std::vector<unsigned char>> stack{sig, msg};
    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_SIGOP + 99;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_REQUIRE(EvalP2MRScript(stack, ml_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, execdata, err));
    BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, 99);

    CPQKey slh;
    slh.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(slh.IsValid());
    const std::vector<unsigned char> slh_msg{0x61, 0x62};
    const std::vector<unsigned char> slh_sig = CreateCSFSSignature(slh, slh_msg);
    const std::vector<unsigned char> slh_script_bytes = BuildP2MRCSFSScript(PQAlgorithm::SLH_DSA_128S, slh.GetPubKey());
    const CScript slh_script(slh_script_bytes.begin(), slh_script_bytes.end());
    std::vector<std::vector<unsigned char>> slh_stack{slh_sig, slh_msg};
    ScriptExecutionData slh_exec;
    slh_exec.m_validation_weight_left_init = true;
    slh_exec.m_validation_weight_left = VALIDATION_WEIGHT_PER_SLHDSA_SIGOP + 77;
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_REQUIRE(EvalP2MRScript(slh_stack, slh_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, slh_exec, err));
    BOOST_CHECK_EQUAL(slh_exec.m_validation_weight_left, 77);
}

BOOST_AUTO_TEST_CASE(csfs_validation_weight_exhaustion_and_boundary)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x70, 0x71};
    const std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);

    CScript repeat_script;
    repeat_script << key.GetPubKey() << OP_CHECKSIGFROMSTACK << OP_DROP << key.GetPubKey() << OP_CHECKSIGFROMSTACK;

    std::vector<std::vector<unsigned char>> repeat_stack{sig, msg, sig, msg};
    ScriptExecutionData exhausted;
    exhausted.m_validation_weight_left_init = true;
    exhausted.m_validation_weight_left = (2 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP) - 1;
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(!EvalP2MRScript(repeat_stack, repeat_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, exhausted, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT);

    const std::vector<unsigned char> single_script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript single_script(single_script_bytes.begin(), single_script_bytes.end());
    std::vector<std::vector<unsigned char>> at_limit_stack{sig, msg};
    ScriptExecutionData at_limit;
    at_limit.m_validation_weight_left_init = true;
    at_limit.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_REQUIRE(EvalP2MRScript(at_limit_stack, single_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, at_limit, err));
    BOOST_CHECK_EQUAL(at_limit.m_validation_weight_left, 0);

    std::vector<std::vector<unsigned char>> over_limit_stack{sig, msg};
    ScriptExecutionData over_limit;
    over_limit.m_validation_weight_left_init = true;
    over_limit.m_validation_weight_left = VALIDATION_WEIGHT_PER_MLDSA_SIGOP - 1;
    err = SCRIPT_ERR_UNKNOWN_ERROR;
    BOOST_CHECK(!EvalP2MRScript(over_limit_stack, single_script, SCRIPT_VERIFY_NONE, BaseSignatureChecker{}, over_limit, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT);
}

BOOST_AUTO_TEST_CASE(csfs_standard_delegation_oracle_and_spender)
{
    CPQKey oracle_key;
    oracle_key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(oracle_key.IsValid());
    CPQKey spender_key;
    spender_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(spender_key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRDelegationScript(
        PQAlgorithm::SLH_DSA_128S, oracle_key.GetPubKey(),
        PQAlgorithm::ML_DSA_44, spender_key.GetPubKey());
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());
    std::vector<unsigned char> sig_checksig;
    BOOST_REQUIRE(spender_key.Sign(*sighash, sig_checksig));

    const std::vector<unsigned char> msg{0x01, 0x02, 0x03};
    const std::vector<unsigned char> sig_csfs = CreateCSFSSignature(oracle_key, msg);

    CScriptWitness witness;
    witness.stack = {sig_checksig, sig_csfs, msg, leaf_script, {P2MR_LEAF_VERSION}};
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(csfs_delegation_invalid_oracle_signature_fails)
{
    CPQKey oracle_key;
    oracle_key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(oracle_key.IsValid());
    CPQKey spender_key;
    spender_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(spender_key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRDelegationScript(
        PQAlgorithm::SLH_DSA_128S, oracle_key.GetPubKey(),
        PQAlgorithm::ML_DSA_44, spender_key.GetPubKey());
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());
    std::vector<unsigned char> sig_checksig;
    BOOST_REQUIRE(spender_key.Sign(*sighash, sig_checksig));

    const std::vector<unsigned char> msg{0x0A, 0x0B};
    std::vector<unsigned char> sig_csfs = CreateCSFSSignature(oracle_key, msg);
    sig_csfs[0] ^= 0x01;

    CScriptWitness witness;
    witness.stack = {sig_checksig, sig_csfs, msg, leaf_script, {P2MR_LEAF_VERSION}};
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const unsigned int flags = P2MR_SCRIPT_FLAGS & ~SCRIPT_VERIFY_NULLFAIL;
    BOOST_CHECK(!VerifyP2MRSpendWithFlags(ctx, witness, flags, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_VERIFY);
}

BOOST_AUTO_TEST_CASE(csfs_delegation_invalid_spender_signature_fails)
{
    CPQKey oracle_key;
    oracle_key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(oracle_key.IsValid());
    CPQKey spender_key;
    spender_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(spender_key.IsValid());

    const std::vector<unsigned char> leaf_script = BuildP2MRDelegationScript(
        PQAlgorithm::SLH_DSA_128S, oracle_key.GetPubKey(),
        PQAlgorithm::ML_DSA_44, spender_key.GetPubKey());
    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    const auto sighash = ComputeP2MRSighash(ctx, leaf_script);
    BOOST_REQUIRE(sighash.has_value());
    std::vector<unsigned char> sig_checksig;
    BOOST_REQUIRE(spender_key.Sign(*sighash, sig_checksig));
    sig_checksig[0] ^= 0x01;

    const std::vector<unsigned char> msg{0xFA, 0xFB};
    const std::vector<unsigned char> sig_csfs = CreateCSFSSignature(oracle_key, msg);

    CScriptWitness witness;
    witness.stack = {sig_checksig, sig_csfs, msg, leaf_script, {P2MR_LEAF_VERSION}};
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    const unsigned int flags = P2MR_SCRIPT_FLAGS & ~SCRIPT_VERIFY_NULLFAIL;
    BOOST_CHECK(!VerifyP2MRSpendWithFlags(ctx, witness, flags, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_EVAL_FALSE);
}

BOOST_AUTO_TEST_CASE(csfs_delegation_leaf_size_expectations)
{
    CPQKey slh_oracle;
    slh_oracle.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(slh_oracle.IsValid());
    CPQKey ml_spender;
    ml_spender.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(ml_spender.IsValid());
    const std::vector<unsigned char> standard_leaf = BuildP2MRDelegationScript(
        PQAlgorithm::SLH_DSA_128S, slh_oracle.GetPubKey(),
        PQAlgorithm::ML_DSA_44, ml_spender.GetPubKey());
    BOOST_CHECK_LE(standard_leaf.size(), 1650U);

    CPQKey ml_oracle;
    ml_oracle.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(ml_oracle.IsValid());
    CPQKey ml_spender2;
    ml_spender2.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(ml_spender2.IsValid());
    const std::vector<unsigned char> two_ml_leaf = BuildP2MRDelegationScript(
        PQAlgorithm::ML_DSA_44, ml_oracle.GetPubKey(),
        PQAlgorithm::ML_DSA_44, ml_spender2.GetPubKey());
    BOOST_CHECK_GT(two_ml_leaf.size(), 1650U);
    BOOST_CHECK_LT(two_ml_leaf.size(), MAX_P2MR_SCRIPT_SIZE);

    const uint256 merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, two_ml_leaf)});
    P2MRSpendContext ctx{BuildP2MROutput(merkle_root)};

    const auto sighash = ComputeP2MRSighash(ctx, two_ml_leaf);
    BOOST_REQUIRE(sighash.has_value());
    std::vector<unsigned char> sig_checksig;
    BOOST_REQUIRE(ml_spender2.Sign(*sighash, sig_checksig));
    const std::vector<unsigned char> msg{0x33, 0x44};
    const std::vector<unsigned char> sig_csfs = CreateCSFSSignature(ml_oracle, msg);

    CScriptWitness witness;
    witness.stack = {sig_checksig, sig_csfs, msg, two_ml_leaf, {P2MR_LEAF_VERSION}};
    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyP2MRSpend(ctx, witness, err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(consensus_rejects_non_p2mr_outputs_in_blocks)
{
    const auto main_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    CTxOut non_p2mr_out;
    non_p2mr_out.nValue = 50 * COIN;
    non_p2mr_out.scriptPubKey = CScript{} << OP_TRUE;

    CBlock block = BuildSingleCoinbaseBlock({non_p2mr_out});
    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(block, state, main_params->GetConsensus(), /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-nonp2mr-output");
}

BOOST_AUTO_TEST_CASE(consensus_accepts_anchor_outputs_in_blocks)
{
    const auto main_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    CTxOut anchor_out;
    anchor_out.nValue = 50 * COIN;
    anchor_out.scriptPubKey = CScript{} << OP_1 << std::vector<unsigned char>{0x4e, 0x73};
    BOOST_REQUIRE(anchor_out.scriptPubKey.IsPayToAnchor());

    CBlock block = BuildSingleCoinbaseBlock({anchor_out});
    BlockValidationState state;
    BOOST_CHECK(CheckBlock(block, state, main_params->GetConsensus(), /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false));
    BOOST_CHECK_NE(state.GetRejectReason(), "bad-txns-nonp2mr-output");
}

BOOST_AUTO_TEST_CASE(consensus_accepts_p2mr_outputs_in_blocks)
{
    const auto main_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const uint256 root = uint256::ONE;
    CTxOut p2mr_out;
    p2mr_out.nValue = 50 * COIN;
    p2mr_out.scriptPubKey = BuildP2MROutput(root);

    CBlock block = BuildSingleCoinbaseBlock({p2mr_out});
    BlockValidationState state;
    BOOST_CHECK(CheckBlock(block, state, main_params->GetConsensus(), /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false));
}

BOOST_AUTO_TEST_CASE(consensus_accepts_opreturn_and_p2mr_outputs_in_blocks)
{
    const auto main_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    CTxOut p2mr_out;
    p2mr_out.nValue = 50 * COIN;
    p2mr_out.scriptPubKey = BuildP2MROutput(uint256::ONE);

    CTxOut op_return_out;
    op_return_out.nValue = 0;
    op_return_out.scriptPubKey = CScript{} << OP_RETURN << std::vector<unsigned char>{0x42, 0x54, 0x58};

    CBlock block = BuildSingleCoinbaseBlock({p2mr_out, op_return_out});
    BlockValidationState state;
    BOOST_CHECK(CheckBlock(block, state, main_params->GetConsensus(), /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false));
}

BOOST_AUTO_TEST_CASE(consensus_rejects_oversized_serialized_blocks)
{
    const auto main_params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& consensus = main_params->GetConsensus();

    CTxOut p2mr_out;
    p2mr_out.nValue = 50 * COIN;
    p2mr_out.scriptPubKey = BuildP2MROutput(uint256::ONE);

    CBlock block = BuildSingleCoinbaseBlock({p2mr_out});
    CMutableTransaction coinbase{*block.vtx.at(0)};

    const size_t block_size = ::GetSerializeSize(TX_WITH_WITNESS(block));
    BOOST_REQUIRE_LT(block_size, consensus.nMaxBlockSerializedSize + 1);
    const size_t witness_padding_size = consensus.nMaxBlockSerializedSize + 1 - block_size;
    coinbase.vin.at(0).scriptWitness.stack.emplace_back(witness_padding_size, 0x42);
    block.vtx.at(0) = MakeTransactionRef(std::move(coinbase));

    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(block, state, consensus, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-blk-length");
}

// ---------------------------------------------------------------------------
// QTC-SECURITY-REVIEW N-1 (PQ sigop accounting) and M-8 (PQ validation weights)
// ---------------------------------------------------------------------------

namespace {

constexpr unsigned int SIGOP_COUNT_FLAGS = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS;

//! Checker that records the arguments OP_CHECKSIGFROMSTACK hands to
//! VerifyPQSignature and returns a fixed result, proving the opcode is routed
//! through the (cacheable) checker hook rather than calling CPQPubKey directly.
class RecordingPQVerifyChecker final : public BaseSignatureChecker
{
public:
    explicit RecordingPQVerifyChecker(bool result) : m_result(result) {}

    bool VerifyPQSignature(Span<const unsigned char> sig, Span<const unsigned char> pubkey, PQAlgorithm algo, const uint256& hash, bool slhdsa_fips205) const override
    {
        ++calls;
        last_sig.assign(sig.begin(), sig.end());
        last_pubkey.assign(pubkey.begin(), pubkey.end());
        last_algo = algo;
        last_hash = hash;
        last_fips205 = slhdsa_fips205;
        return m_result;
    }

    mutable size_t calls{0};
    mutable std::vector<unsigned char> last_sig;
    mutable std::vector<unsigned char> last_pubkey;
    mutable std::optional<PQAlgorithm> last_algo;
    mutable std::optional<uint256> last_hash;
    mutable bool last_fips205{false};

private:
    const bool m_result;
};

//! Concatenate `copies` copies of a leaf fragment; only used for static sigop
//! counting, so the result need not be executable.
std::vector<unsigned char> RepeatLeaf(const std::vector<unsigned char>& fragment, size_t copies)
{
    std::vector<unsigned char> out;
    out.reserve(fragment.size() * copies);
    for (size_t i = 0; i < copies; ++i) out.insert(out.end(), fragment.begin(), fragment.end());
    return out;
}

//! Static PQ sigop count for a single-leaf P2MR spend whose witness is
//! [inputs..., leaf, control(, annex)].
size_t CountP2MRLeafSigOps(const std::vector<unsigned char>& leaf_script, size_t n_inputs, const std::vector<unsigned char>* annex = nullptr)
{
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    const CScript script_pub_key = BuildP2MROutput(ComputeP2MRMerkleRoot({leaf_hash}));
    CScriptWitness witness;
    for (size_t i = 0; i < n_inputs; ++i) witness.stack.emplace_back();
    witness.stack.push_back(leaf_script);
    witness.stack.push_back({P2MR_LEAF_VERSION});
    if (annex != nullptr) witness.stack.push_back(*annex);
    return CountWitnessSigOps(CScript{}, script_pub_key, &witness, SIGOP_COUNT_FLAGS);
}

//! Leaf that verifies one witness SLH-DSA signature `k` times against the same key:
//!   (OP_DUP <pk> OP_CHECKSIG_SLHDSA OP_VERIFY) x k  OP_DROP OP_1
//! Each pass costs VALIDATION_WEIGHT_PER_SLHDSA_SIGOP while the witness carries a
//! single signature, so k can exhaust the per-input budget.
std::vector<unsigned char> BuildRepeatedSLHDSAChecksigLeaf(Span<const unsigned char> pubkey, size_t k)
{
    const std::vector<unsigned char> pk(pubkey.begin(), pubkey.end());
    CScript script;
    for (size_t i = 0; i < k; ++i) {
        script << OP_DUP << pk << OP_CHECKSIG_SLHDSA << OP_VERIFY;
    }
    script << OP_DROP << OP_1;
    return {script.begin(), script.end()};
}

int64_t P2MRInputBudget(const CScriptWitness& witness)
{
    return static_cast<int64_t>(::GetSerializeSize(witness.stack)) + VALIDATION_WEIGHT_OFFSET;
}

} // namespace

// M-8: pin the weights this fork ships with, and the invariant that keeps every
// PQ opcode spendable: a per-signature charge must not exceed the validation
// budget that one signature of that algorithm contributes to its own input
// (signature bytes + 3-byte compactsize prefix). This is why the SLH-DSA
// OP_CHECKSIGADD weight was NOT scaled 10x with the single-sig weight.
static_assert(VALIDATION_WEIGHT_PER_MLDSA_SIGOP == 50, "QTC-SECURITY-REVIEW M-8: ML-DSA-44 weight");
static_assert(VALIDATION_WEIGHT_PER_SLHDSA_SIGOP == 1000, "QTC-SECURITY-REVIEW M-8: SLH-DSA-128s weight raised from 500");
static_assert(VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP == 500, "QTC-SECURITY-REVIEW M-8: ML-DSA-44 CHECKSIGADD weight");
static_assert(VALIDATION_WEIGHT_PER_SLHDSA_MULTISIG_SIGOP == 5000, "QTC-SECURITY-REVIEW M-8: SLH-DSA-128s CHECKSIGADD weight");
static_assert(VALIDATION_WEIGHT_PER_MLDSA_SIGOP <= static_cast<int64_t>(MLDSA44_SIGNATURE_SIZE) + 3, "ML-DSA CHECKSIG would be unspendable");
static_assert(VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP <= static_cast<int64_t>(MLDSA44_SIGNATURE_SIZE) + 3, "ML-DSA CHECKSIGADD would be unspendable");
static_assert(VALIDATION_WEIGHT_PER_SLHDSA_SIGOP <= static_cast<int64_t>(SLHDSA128S_SIGNATURE_SIZE) + 3, "SLH-DSA CHECKSIG would be unspendable");
static_assert(VALIDATION_WEIGHT_PER_SLHDSA_MULTISIG_SIGOP <= static_cast<int64_t>(SLHDSA128S_SIGNATURE_SIZE) + 3, "SLH-DSA CHECKSIGADD would be unspendable");
// N-1: the static CSFS count (SLH-DSA weight) must dominate whatever the executor
// charges for the algorithm it discovers at run time.
static_assert(VALIDATION_WEIGHT_PER_SLHDSA_SIGOP >= VALIDATION_WEIGHT_PER_MLDSA_SIGOP, "CSFS static count must be the maximum");

// N-1 (a): with the annex rejected, the counter and the executor read the leaf from
// the same witness position; an appended annex can no longer zero the count.
BOOST_AUTO_TEST_CASE(p2mr_witness_sigops_unaffected_by_annex)
{
    const std::vector<unsigned char> pk(MLDSA44_PUBKEY_SIZE, 0x11);
    const std::vector<unsigned char> fragment = BuildP2MRScript(PQAlgorithm::ML_DSA_44, pk);
    const std::vector<unsigned char> annex{ANNEX_TAG, 0xAA, 0xBB};
    std::vector<unsigned char> big_annex(3 * MAX_P2MR_ELEMENT_SIZE, 0x00);
    big_annex.front() = ANNEX_TAG;

    for (size_t k = 1; k <= 4; ++k) {
        const std::vector<unsigned char> leaf = RepeatLeaf(fragment, k);
        const size_t expected = k * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
        BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(leaf, /*n_inputs=*/1), expected);
        // Pre-fix this returned 0: the counter scanned the control block.
        BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(leaf, /*n_inputs=*/1, &annex), expected);
        BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(leaf, /*n_inputs=*/1, &big_annex), expected);
    }

    // Two-element witness: the trailing element is the control block even if it
    // starts with 0x50, exactly as in VerifyWitnessProgram.
    {
        const std::vector<unsigned char> leaf = RepeatLeaf(fragment, 2);
        const CScript script_pub_key = BuildP2MROutput(ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf)}));
        CScriptWitness witness;
        witness.stack = {leaf, {ANNEX_TAG}};
        BOOST_CHECK_EQUAL(CountWitnessSigOps(CScript{}, script_pub_key, &witness, SIGOP_COUNT_FLAGS), static_cast<size_t>(2 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP));
    }
}

// N-1 (a): OP_CHECKSIGFROMSTACK is counted at the SLH-DSA weight whatever pubkey
// the leaf carries, since the executor picks the algorithm from the pubkey size at
// run time and the static scan must never under-count.
BOOST_AUTO_TEST_CASE(p2mr_witness_sigops_count_csfs_at_slhdsa_weight)
{
    const std::vector<unsigned char> ml_pk(MLDSA44_PUBKEY_SIZE, 0x21);
    const std::vector<unsigned char> slh_pk(SLHDSA128S_PUBKEY_SIZE, 0x22);
    const std::vector<unsigned char> ml_csfs = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, ml_pk);
    const std::vector<unsigned char> slh_csfs = BuildP2MRCSFSScript(PQAlgorithm::SLH_DSA_128S, slh_pk);
    const std::vector<unsigned char> annex{ANNEX_TAG, 0x01};

    for (size_t k = 1; k <= 3; ++k) {
        const size_t expected = k * VALIDATION_WEIGHT_PER_SLHDSA_SIGOP;
        // Pre-fix CSFS was absent from the counter's switch and counted 0.
        BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(RepeatLeaf(ml_csfs, k), /*n_inputs=*/2), expected);
        BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(RepeatLeaf(slh_csfs, k), /*n_inputs=*/2), expected);
        BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(RepeatLeaf(ml_csfs, k), /*n_inputs=*/2, &annex), expected);
    }

    // The bare opcode (pubkey supplied from the witness) is counted the same way.
    const std::vector<unsigned char> bare{static_cast<unsigned char>(OP_CHECKSIGFROMSTACK)};
    BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(bare, /*n_inputs=*/3), static_cast<size_t>(VALIDATION_WEIGHT_PER_SLHDSA_SIGOP));
}

// N-1 (a): every PQ-verifying opcode the executor charges is charged by the counter.
// The reserved Falcon slots are OP_SUCCESSx in P2MR (they verify nothing) and count 0.
BOOST_AUTO_TEST_CASE(p2mr_witness_sigops_cover_every_pq_opcode)
{
    const std::vector<unsigned char> ml_pk(MLDSA44_PUBKEY_SIZE, 0x31);
    const std::vector<unsigned char> slh_pk(SLHDSA128S_PUBKEY_SIZE, 0x32);

    CScript leaf;
    leaf << ml_pk << OP_CHECKSIG_MLDSA
         << slh_pk << OP_CHECKSIG_SLHDSA
         << ml_pk << OP_CHECKSIGADD_MLDSA
         << slh_pk << OP_CHECKSIGADD_SLHDSA
         << ml_pk << OP_CHECKSIGFROMSTACK
         << OP_CHECKSIG_FALCON << OP_CHECKSIGADD_FALCON << OP_CHECKSIGFROMSTACK_FALCON;
    const std::vector<unsigned char> leaf_bytes(leaf.begin(), leaf.end());

    const size_t expected = VALIDATION_WEIGHT_PER_MLDSA_SIGOP +
                            VALIDATION_WEIGHT_PER_SLHDSA_SIGOP +
                            VALIDATION_WEIGHT_PER_MLDSA_MULTISIG_SIGOP +
                            VALIDATION_WEIGHT_PER_SLHDSA_MULTISIG_SIGOP +
                            VALIDATION_WEIGHT_PER_SLHDSA_SIGOP; // CSFS at the conservative weight
    BOOST_CHECK_EQUAL(CountP2MRLeafSigOps(leaf_bytes, /*n_inputs=*/1), expected);
}

// N-1 (c): OP_CHECKSIGFROMSTACK verifies through checker.VerifyPQSignature with the
// CSFS-tagged hash of the message, the algorithm derived from the pubkey size and
// the FIPS-205 mode flag, so a caching checker can key its cache on all of them.
BOOST_AUTO_TEST_CASE(csfs_routes_verification_through_checker_verifypqsignature)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(key.IsValid());
    const std::vector<unsigned char> msg{0x5A, 0x5B, 0x5C};
    std::vector<unsigned char> sig = CreateCSFSSignature(key, msg);
    sig[0] ^= 0x01; // corrupted: only a checker that says "true" can make this pass
    const std::vector<unsigned char> script_bytes = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, key.GetPubKey());
    const CScript script(script_bytes.begin(), script_bytes.end());
    const std::vector<unsigned char> pubkey = key.GetPubKey(); // one temporary: begin()/end() of two temporaries is UB

    for (const bool fips205 : {false, true}) {
        const unsigned int flags = fips205 ? SCRIPT_VERIFY_SLHDSA_FIPS205 : SCRIPT_VERIFY_NONE;

        std::vector<std::vector<unsigned char>> stack{sig, msg};
        ScriptExecutionData execdata;
        execdata.m_validation_weight_left_init = true;
        execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        const RecordingPQVerifyChecker checker{/*result=*/true};

        BOOST_REQUIRE(EvalP2MRScript(stack, script, flags, checker, execdata, err));
        BOOST_CHECK_EQUAL(stack.size(), 1U);
        BOOST_CHECK(stack.back() == std::vector<unsigned char>({1}));
        BOOST_CHECK_EQUAL(checker.calls, 1U);
        BOOST_CHECK(checker.last_sig == sig);
        BOOST_CHECK(checker.last_pubkey == pubkey);
        BOOST_REQUIRE(checker.last_algo.has_value());
        BOOST_CHECK(*checker.last_algo == PQAlgorithm::ML_DSA_44);
        BOOST_REQUIRE(checker.last_hash.has_value());
        BOOST_CHECK(*checker.last_hash == ComputeCSFSHash(msg));
        BOOST_CHECK_EQUAL(checker.last_fips205, fips205);
        // The weight is still charged on this path.
        BOOST_CHECK_EQUAL(execdata.m_validation_weight_left, 9 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP);
    }

    // And a checker that says "false" yields a false result (no NULLFAIL here).
    {
        std::vector<std::vector<unsigned char>> stack{sig, msg};
        ScriptExecutionData execdata;
        execdata.m_validation_weight_left_init = true;
        execdata.m_validation_weight_left = 10 * VALIDATION_WEIGHT_PER_MLDSA_SIGOP;
        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        const RecordingPQVerifyChecker checker{/*result=*/false};
        BOOST_REQUIRE(EvalP2MRScript(stack, script, SCRIPT_VERIFY_NONE, checker, execdata, err));
        BOOST_CHECK_EQUAL(stack.size(), 1U);
        BOOST_CHECK(stack.back().empty());
        BOOST_CHECK_EQUAL(checker.calls, 1U);
    }
}

// N-1 (c): a full P2MR CSFS spend verified with CachingTransactionSignatureChecker
// populates the salted signature cache under the CSFS hash / algorithm / mode key.
BOOST_AUTO_TEST_CASE(csfs_verification_is_covered_by_signature_cache)
{
    CPQKey oracle;
    oracle.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(oracle.IsValid());
    const std::vector<unsigned char> pubkey = oracle.GetPubKey(); // one temporary: begin()/end() of two temporaries is UB
    const std::vector<unsigned char> msg{0x71, 0x72, 0x73, 0x74};
    const std::vector<unsigned char> sig = CreateCSFSSignature(oracle, msg);
    const uint256 csfs_hash = ComputeCSFSHash(msg);

    const std::vector<unsigned char> leaf_script = BuildP2MRCSFSScript(PQAlgorithm::ML_DSA_44, pubkey);
    const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script);
    P2MRSpendContext ctx{BuildP2MROutput(ComputeP2MRMerkleRoot({leaf_hash}))};

    CScriptWitness witness;
    witness.stack = {sig, msg, leaf_script, {P2MR_LEAF_VERSION}};
    ctx.tx_spend.vin.at(0).scriptWitness = witness;
    const CTransaction tx{ctx.tx_spend};

    SignatureCache cache{1 << 20};
    uint256 entry;
    cache.ComputeEntryPQ(entry, csfs_hash, sig, pubkey, PQAlgorithm::ML_DSA_44, /*slhdsa_fips205=*/false);
    BOOST_CHECK(!cache.Get(entry, /*erase=*/false));

    ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyScript(
        tx.vin.at(0).scriptSig,
        ctx.tx_credit.vout.at(0).scriptPubKey,
        &tx.vin.at(0).scriptWitness,
        P2MR_SCRIPT_FLAGS,
        CachingTransactionSignatureChecker(&tx, /*nIn=*/0, ctx.tx_credit.vout.at(0).nValue, /*storeIn=*/true, cache, ctx.txdata),
        &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);

    // The CSFS verification result is now cached under its own key ...
    BOOST_CHECK(cache.Get(entry, /*erase=*/false));
    // ... and not under the FIPS-205 variant of the same tuple.
    uint256 fips_entry;
    cache.ComputeEntryPQ(fips_entry, csfs_hash, sig, pubkey, PQAlgorithm::ML_DSA_44, /*slhdsa_fips205=*/true);
    BOOST_CHECK(!cache.Get(fips_entry, /*erase=*/false));
}

// M-8 (test 3): the per-input validation budget arithmetic reflects the 1000-unit
// SLH-DSA weight on a real spend. With one 7,856-byte witness signature re-verified
// k times, the budget is ~7,909 + 36k while the cost is 1000k, so k = 8 fits and
// k = 9 does not; under the pre-M-8 weight of 500, k = 9 would have been accepted.
BOOST_AUTO_TEST_CASE(p2mr_slhdsa_spend_budget_reflects_raised_weight)
{
    CPQKey key;
    key.MakeNewKey(PQAlgorithm::SLH_DSA_128S);
    BOOST_REQUIRE(key.IsValid());

    // Find the boundary from sizes alone (no signing needed: SLH-DSA signatures
    // have a fixed size), then sign only the two boundary leaves.
    const std::vector<unsigned char> dummy_sig(SLHDSA128S_SIGNATURE_SIZE, 0x00);
    auto budget_for = [&](size_t k) {
        CScriptWitness w;
        w.stack = {dummy_sig, BuildRepeatedSLHDSAChecksigLeaf(key.GetPubKey(), k), {P2MR_LEAF_VERSION}};
        return P2MRInputBudget(w);
    };
    size_t k_pass{0};
    for (size_t k = 1; k <= 32; ++k) {
        if (static_cast<int64_t>(k) * VALIDATION_WEIGHT_PER_SLHDSA_SIGOP <= budget_for(k)) k_pass = k;
    }
    BOOST_REQUIRE_GE(k_pass, 1U);
    const size_t k_fail = k_pass + 1;
    BOOST_REQUIRE_GT(static_cast<int64_t>(k_fail) * VALIDATION_WEIGHT_PER_SLHDSA_SIGOP, budget_for(k_fail));
    BOOST_CHECK_EQUAL(k_pass, 8U);
    // Documenting the M-8 change: at the old weight (500) the failing leaf fit.
    BOOST_CHECK_LE(static_cast<int64_t>(k_fail) * 500, budget_for(k_fail));

    for (const size_t k : {k_pass, k_fail}) {
        const std::vector<unsigned char> leaf = BuildRepeatedSLHDSAChecksigLeaf(key.GetPubKey(), k);
        const uint256 leaf_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf);
        P2MRSpendContext ctx{BuildP2MROutput(ComputeP2MRMerkleRoot({leaf_hash}))};
        const auto sighash = ComputeP2MRSighash(ctx, leaf);
        BOOST_REQUIRE(sighash.has_value());
        std::vector<unsigned char> sig;
        BOOST_REQUIRE(key.Sign(*sighash, sig));
        BOOST_REQUIRE_EQUAL(sig.size(), SLHDSA128S_SIGNATURE_SIZE);

        CScriptWitness witness;
        witness.stack = {sig, leaf, {P2MR_LEAF_VERSION}};
        BOOST_CHECK_EQUAL(P2MRInputBudget(witness), budget_for(k));

        ScriptError err{SCRIPT_ERR_UNKNOWN_ERROR};
        const bool ok = VerifyP2MRSpend(ctx, witness, err);
        if (k == k_pass) {
            BOOST_CHECK_MESSAGE(ok, "k=" << k << " should fit the budget");
            BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
        } else {
            BOOST_CHECK_MESSAGE(!ok, "k=" << k << " should exhaust the budget");
            BOOST_CHECK_EQUAL(err, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
