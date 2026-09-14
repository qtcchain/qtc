// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <hash.h>
#include <pqkey.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/pqm.h>
#include <script/script_error.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <test/util/transaction_utils.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <vector>

namespace {

class P2MRTemplateChecker final : public BaseSignatureChecker
{
public:
    explicit P2MRTemplateChecker(bool locktime_ok) : m_locktime_ok(locktime_ok) {}

    bool CheckPQSignature(Span<const unsigned char>, Span<const unsigned char>, PQAlgorithm, uint8_t, SigVersion, ScriptExecutionData&, bool) const override
    {
        return true;
    }

    bool CheckLockTime(const CScriptNum&) const override
    {
        return m_locktime_ok;
    }

private:
    bool m_locktime_ok;
};

std::vector<unsigned char> Hash160Bytes(Span<const unsigned char> data)
{
    const uint160 hash = Hash160(data);
    return {hash.begin(), hash.end()};
}

std::vector<unsigned char> Sha256Bytes(Span<const unsigned char> data)
{
    uint256 hash;
    CSHA256().Write(data.data(), data.size()).Finalize(hash.begin());
    return {hash.begin(), hash.end()};
}

constexpr unsigned int HTLC_VERIFY_FLAGS =
    SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_NULLFAIL |
    SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY | SCRIPT_VERIFY_CHECKSIGFROMSTACK;

CScript BuildP2MROutputScript(const uint256& merkle_root)
{
    CScript script;
    script << OP_2 << ToByteVector(merkle_root);
    return script;
}

// Single-leaf P2MR funding output plus a spending transaction, ready for the
// wallet signer (ProduceSignature) and consensus verification (VerifyScript).
struct HtlcSignFixture {
    CMutableTransaction tx_credit;
    CMutableTransaction tx_spend;
    PrecomputedTransactionData txdata;
    uint256 merkle_root;
    std::vector<unsigned char> leaf_script;

    explicit HtlcSignFixture(std::vector<unsigned char> leaf)
        : leaf_script(std::move(leaf))
    {
        merkle_root = ComputeP2MRMerkleRoot({ComputeP2MRLeafHash(P2MR_LEAF_VERSION, leaf_script)});
        tx_credit = BuildCreditingTransaction(BuildP2MROutputScript(merkle_root), /*nValue=*/5'000);
        tx_spend = BuildSpendingTransaction(CScript{}, CScriptWitness{}, CTransaction{tx_credit});
        txdata.Init(tx_spend, {tx_credit.vout.at(0)}, /*force=*/true);
    }

    FlatSigningProvider Provider(const CPQKey* key) const
    {
        FlatSigningProvider provider;
        P2MRSpendData spenddata;
        spenddata.scripts[leaf_script].insert({P2MR_LEAF_VERSION});
        provider.p2mr_spends[WitnessV2P2MR{merkle_root}] = spenddata;
        if (key) provider.pq_keys[key->GetPubKey()] = *key;
        return provider;
    }

    bool Sign(const FlatSigningProvider& provider, SignatureData& sigdata) const
    {
        MutableTransactionSignatureCreator creator(
            tx_spend, /*input_idx=*/0, tx_credit.vout.at(0).nValue, &txdata, SIGHASH_DEFAULT);
        return ProduceSignature(provider, creator, tx_credit.vout.at(0).scriptPubKey, sigdata);
    }

    bool Verify(const SignatureData& sigdata, ScriptError& serror)
    {
        tx_spend.vin.at(0).scriptWitness = sigdata.scriptWitness;
        return VerifyScript(
            tx_spend.vin.at(0).scriptSig,
            tx_credit.vout.at(0).scriptPubKey,
            &tx_spend.vin.at(0).scriptWitness,
            HTLC_VERIFY_FLAGS,
            MutableTransactionSignatureChecker(
                &tx_spend, /*nIn=*/0, tx_credit.vout.at(0).nValue, txdata, MissingDataBehavior::ASSERT_FAIL),
            &serror);
    }
};

void CheckStandardHtlcWitness(const SignatureData& sigdata,
                              const std::vector<unsigned char>& preimage,
                              const std::vector<unsigned char>& leaf_script,
                              PQAlgorithm algo)
{
    const auto& stack = sigdata.scriptWitness.stack;
    BOOST_REQUIRE_EQUAL(stack.size(), 4U);
    BOOST_CHECK_EQUAL(stack[0].size(), GetPQSignatureSize(algo));
    BOOST_CHECK(stack[1] == preimage);
    BOOST_CHECK(stack[2] == leaf_script);
    BOOST_CHECK(stack[3] == std::vector<unsigned char>({P2MR_LEAF_VERSION}));
}

bool EvalP2MRScript(const CScript& script, std::vector<std::vector<unsigned char>>& stack, const BaseSignatureChecker& checker, ScriptExecutionData& execdata, ScriptError& serror)
{
    constexpr unsigned int flags = SCRIPT_VERIFY_NULLFAIL | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    return EvalScript(stack, script, flags, checker, SigVersion::P2MR, execdata, &serror);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(script_htlc_templates_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(legacy_htlc_leaf_valid_build)
{
    const std::vector<unsigned char> preimage_hash(20, 0x11);
    const std::vector<unsigned char> oracle_pubkey(MLDSA44_PUBKEY_SIZE, 0x22);
    const std::vector<unsigned char> script = BuildP2MRHTLCLeaf(preimage_hash, PQAlgorithm::ML_DSA_44, oracle_pubkey);
    BOOST_REQUIRE(!script.empty());

    CScript expected;
    expected << preimage_hash << OP_OVER << OP_HASH160 << OP_EQUALVERIFY
             << oracle_pubkey << OP_CHECKSIGFROMSTACK;
    BOOST_CHECK_EQUAL_COLLECTIONS(script.begin(), script.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(htlc_tx_leaf_valid_build)
{
    const std::vector<unsigned char> preimage_hash(20, 0x31);
    const std::vector<unsigned char> claimant_pubkey(MLDSA44_PUBKEY_SIZE, 0x32);
    const std::vector<unsigned char> script =
        BuildP2MRHTLCTxLeaf(preimage_hash, PQAlgorithm::ML_DSA_44, claimant_pubkey);
    BOOST_REQUIRE(!script.empty());

    CScript expected;
    expected << preimage_hash << OP_OVER << OP_HASH160 << OP_EQUALVERIFY << OP_DROP
             << claimant_pubkey << OP_CHECKSIG_MLDSA;
    BOOST_CHECK_EQUAL_COLLECTIONS(script.begin(), script.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(htlc_sha256_leaf_valid_build)
{
    const std::vector<unsigned char> preimage_hash(32, 0x41);
    const std::vector<unsigned char> claimant_pubkey(MLDSA44_PUBKEY_SIZE, 0x42);
    const std::vector<unsigned char> script =
        BuildP2MRHTLCSha256Leaf(preimage_hash, PQAlgorithm::ML_DSA_44, claimant_pubkey);
    BOOST_REQUIRE(!script.empty());

    CScript expected;
    expected << OP_SHA256 << preimage_hash << OP_EQUALVERIFY
             << claimant_pubkey << OP_CHECKSIG_MLDSA;
    BOOST_CHECK_EQUAL_COLLECTIONS(script.begin(), script.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(htlc_leaf_parsers_roundtrip)
{
    const std::vector<unsigned char> hash160(20, 0x51);
    const std::vector<unsigned char> sha256(32, 0x52);
    const std::vector<unsigned char> pubkey(SLHDSA128S_PUBKEY_SIZE, 0x53);

    const std::vector<unsigned char> legacy = BuildP2MRHTLCLeaf(hash160, PQAlgorithm::SLH_DSA_128S, pubkey);
    const std::vector<unsigned char> tx_leaf = BuildP2MRHTLCTxLeaf(hash160, PQAlgorithm::SLH_DSA_128S, pubkey);
    const std::vector<unsigned char> sha_leaf = BuildP2MRHTLCSha256Leaf(sha256, PQAlgorithm::SLH_DSA_128S, pubkey);
    BOOST_REQUIRE(!legacy.empty());
    BOOST_REQUIRE(!tx_leaf.empty());
    BOOST_REQUIRE(!sha_leaf.empty());

    std::vector<unsigned char> parsed_hash;
    std::vector<unsigned char> parsed_pubkey;
    PQAlgorithm parsed_algo{PQAlgorithm::ML_DSA_44};

    BOOST_CHECK(ParseP2MRLegacyHTLCLeaf(legacy, parsed_hash, parsed_algo, parsed_pubkey));
    BOOST_CHECK(parsed_algo == PQAlgorithm::SLH_DSA_128S);
    BOOST_CHECK(parsed_hash == hash160);
    BOOST_CHECK(parsed_pubkey == pubkey);
    BOOST_CHECK(!ParseP2MRHTLCTxLeaf(legacy, parsed_hash, parsed_algo, parsed_pubkey));
    BOOST_CHECK(!ParseP2MRHTLCSha256Leaf(legacy, parsed_hash, parsed_algo, parsed_pubkey));

    parsed_algo = PQAlgorithm::ML_DSA_44;
    BOOST_CHECK(ParseP2MRHTLCTxLeaf(tx_leaf, parsed_hash, parsed_algo, parsed_pubkey));
    BOOST_CHECK(parsed_algo == PQAlgorithm::SLH_DSA_128S);
    BOOST_CHECK(parsed_hash == hash160);
    BOOST_CHECK(parsed_pubkey == pubkey);
    BOOST_CHECK(!ParseP2MRLegacyHTLCLeaf(tx_leaf, parsed_hash, parsed_algo, parsed_pubkey));
    BOOST_CHECK(!ParseP2MRHTLCSha256Leaf(tx_leaf, parsed_hash, parsed_algo, parsed_pubkey));

    parsed_algo = PQAlgorithm::ML_DSA_44;
    BOOST_CHECK(ParseP2MRHTLCSha256Leaf(sha_leaf, parsed_hash, parsed_algo, parsed_pubkey));
    BOOST_CHECK(parsed_algo == PQAlgorithm::SLH_DSA_128S);
    BOOST_CHECK(parsed_hash == sha256);
    BOOST_CHECK(parsed_pubkey == pubkey);
    BOOST_CHECK(!ParseP2MRLegacyHTLCLeaf(sha_leaf, parsed_hash, parsed_algo, parsed_pubkey));
    BOOST_CHECK(!ParseP2MRHTLCTxLeaf(sha_leaf, parsed_hash, parsed_algo, parsed_pubkey));
}

BOOST_AUTO_TEST_CASE(htlc_leaf_invalid_preimage_size)
{
    const std::vector<unsigned char> wrong_hash(19, 0x01);
    const std::vector<unsigned char> oracle_pubkey(MLDSA44_PUBKEY_SIZE, 0x02);
    BOOST_CHECK(BuildP2MRHTLCLeaf(wrong_hash, PQAlgorithm::ML_DSA_44, oracle_pubkey).empty());
    BOOST_CHECK(BuildP2MRHTLCTxLeaf(wrong_hash, PQAlgorithm::ML_DSA_44, oracle_pubkey).empty());
    BOOST_CHECK(BuildP2MRHTLCSha256Leaf(wrong_hash, PQAlgorithm::ML_DSA_44, oracle_pubkey).empty());
}

BOOST_AUTO_TEST_CASE(refund_leaf_valid_build)
{
    const std::vector<unsigned char> sender_pubkey(MLDSA44_PUBKEY_SIZE, 0x33);
    const std::vector<unsigned char> script = BuildP2MRRefundLeaf(/*timeout=*/500, PQAlgorithm::ML_DSA_44, sender_pubkey);
    BOOST_REQUIRE(!script.empty());

    CScript expected;
    expected << CScriptNum{500} << OP_CHECKLOCKTIMEVERIFY << OP_DROP << sender_pubkey << OP_CHECKSIG_MLDSA;
    BOOST_CHECK_EQUAL_COLLECTIONS(script.begin(), script.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(htlc_leaf_size_within_policy)
{
    const std::vector<unsigned char> preimage_hash(32, 0x44);
    const std::vector<unsigned char> oracle_pubkey(MLDSA44_PUBKEY_SIZE, 0x55);
    const std::vector<unsigned char> script = BuildP2MRHTLCSha256Leaf(
        preimage_hash, PQAlgorithm::ML_DSA_44, oracle_pubkey);
    BOOST_REQUIRE(!script.empty());
    BOOST_CHECK_LT(script.size(), 1650U);
}

BOOST_AUTO_TEST_CASE(htlc_correct_preimage_succeeds)
{
    CPQKey oracle_key;
    oracle_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(oracle_key.IsValid());

    const std::vector<unsigned char> preimage(32, 0x66);
    const std::vector<unsigned char> preimage_hash = Sha256Bytes(preimage);
    const std::vector<unsigned char> script_bytes = BuildP2MRHTLCSha256Leaf(
        preimage_hash, PQAlgorithm::ML_DSA_44, oracle_key.GetPubKey());
    BOOST_REQUIRE(!script_bytes.empty());
    const CScript script{script_bytes.begin(), script_bytes.end()};

    // The template checker accepts a correctly-sized transaction signature; full
    // transaction binding is exercised by the wallet HTLC functional tests.
    // Claim witness (bottom->top): <tx_sig> <preimage>.
    std::vector<std::vector<unsigned char>> stack;
    stack.push_back(std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01));
    stack.push_back(preimage);

    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 5000;
    ScriptError serror = SCRIPT_ERR_OK;
    const P2MRTemplateChecker checker{/*locktime_ok=*/true};
    BOOST_REQUIRE(EvalP2MRScript(script, stack, checker, execdata, serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_OK);
    // Consensus (ExecuteWitnessScript) requires exactly one truthy cleanstack item.
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(CScriptNum(stack.back(), /*fRequireMinimal=*/true).GetInt64(), 1);
}

BOOST_AUTO_TEST_CASE(htlc_wrong_preimage_fails)
{
    CPQKey oracle_key;
    oracle_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(oracle_key.IsValid());

    const std::vector<unsigned char> correct_preimage(32, 0x77);
    const std::vector<unsigned char> wrong_preimage(32, 0x88);
    const std::vector<unsigned char> preimage_hash = Sha256Bytes(correct_preimage);
    const std::vector<unsigned char> script_bytes = BuildP2MRHTLCSha256Leaf(
        preimage_hash, PQAlgorithm::ML_DSA_44, oracle_key.GetPubKey());
    BOOST_REQUIRE(!script_bytes.empty());
    const CScript script{script_bytes.begin(), script_bytes.end()};

    std::vector<std::vector<unsigned char>> stack;
    stack.push_back(std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01));
    stack.push_back(wrong_preimage);

    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 5000;
    ScriptError serror = SCRIPT_ERR_OK;
    const P2MRTemplateChecker checker{/*locktime_ok=*/true};
    BOOST_CHECK(!EvalP2MRScript(script, stack, checker, execdata, serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_EQUALVERIFY);
}

BOOST_AUTO_TEST_CASE(htlc_tx_correct_preimage_succeeds)
{
    CPQKey claimant_key;
    claimant_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(claimant_key.IsValid());

    const std::vector<unsigned char> preimage(32, 0x69);
    const std::vector<unsigned char> preimage_hash = Hash160Bytes(preimage);
    const std::vector<unsigned char> script_bytes = BuildP2MRHTLCTxLeaf(
        preimage_hash, PQAlgorithm::ML_DSA_44, claimant_key.GetPubKey());
    BOOST_REQUIRE(!script_bytes.empty());
    const CScript script{script_bytes.begin(), script_bytes.end()};

    std::vector<std::vector<unsigned char>> stack;
    stack.push_back(std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01));
    stack.push_back(preimage);

    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 5000;
    ScriptError serror = SCRIPT_ERR_OK;
    const P2MRTemplateChecker checker{/*locktime_ok=*/true};
    BOOST_REQUIRE(EvalP2MRScript(script, stack, checker, execdata, serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_OK);
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(CScriptNum(stack.back(), /*fRequireMinimal=*/true).GetInt64(), 1);
}

BOOST_AUTO_TEST_CASE(signer_claims_htlc_sha256_leaf_with_correct_preimage)
{
    for (const PQAlgorithm algo : {PQAlgorithm::ML_DSA_44, PQAlgorithm::SLH_DSA_128S}) {
        CPQKey claimant_key;
        claimant_key.MakeNewKey(algo);
        BOOST_REQUIRE(claimant_key.IsValid());

        const std::vector<unsigned char> preimage(32, 0x5a);
        const std::vector<unsigned char> preimage_hash = Sha256Bytes(preimage);
        HtlcSignFixture fixture{BuildP2MRHTLCSha256Leaf(preimage_hash, algo, claimant_key.GetPubKey())};
        BOOST_REQUIRE(!fixture.leaf_script.empty());

        SignatureData sigdata;
        sigdata.sha256_preimages[preimage_hash] = preimage;
        BOOST_REQUIRE_MESSAGE(fixture.Sign(fixture.Provider(&claimant_key), sigdata),
                              "algo=" << static_cast<int>(algo));
        CheckStandardHtlcWitness(sigdata, preimage, fixture.leaf_script, algo);

        ScriptError serror{SCRIPT_ERR_UNKNOWN_ERROR};
        BOOST_CHECK_MESSAGE(fixture.Verify(sigdata, serror),
                            "algo=" << static_cast<int>(algo) << " err=" << ScriptErrorString(serror));
        BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_OK);
    }
}

BOOST_AUTO_TEST_CASE(signer_rejects_htlc_sha256_leaf_with_wrong_preimage)
{
    CPQKey claimant_key;
    claimant_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(claimant_key.IsValid());

    const std::vector<unsigned char> correct_preimage(32, 0x5b);
    const std::vector<unsigned char> wrong_preimage(32, 0x5c);
    const std::vector<unsigned char> preimage_hash = Sha256Bytes(correct_preimage);
    HtlcSignFixture fixture{BuildP2MRHTLCSha256Leaf(preimage_hash, PQAlgorithm::ML_DSA_44, claimant_key.GetPubKey())};
    BOOST_REQUIRE(!fixture.leaf_script.empty());
    const FlatSigningProvider provider = fixture.Provider(&claimant_key);

    // No preimage for the leaf's hashlock: the signer must not produce a claim.
    {
        SignatureData sigdata;
        sigdata.sha256_preimages[Sha256Bytes(wrong_preimage)] = wrong_preimage;
        BOOST_CHECK(!fixture.Sign(provider, sigdata));
        BOOST_CHECK(sigdata.scriptWitness.stack.empty());
    }
    // A HASH160-keyed preimage does not satisfy a SHA-256 lock.
    {
        SignatureData sigdata;
        sigdata.hash160_preimages[Hash160Bytes(correct_preimage)] = correct_preimage;
        BOOST_CHECK(!fixture.Sign(provider, sigdata));
    }
    // A wrong preimage smuggled under the right hashlock: the produced witness fails script
    // verification, so the signer reports an incomplete signature rather than a claim.
    {
        SignatureData sigdata;
        sigdata.sha256_preimages[preimage_hash] = wrong_preimage;
        BOOST_CHECK(!fixture.Sign(provider, sigdata));
        ScriptError serror{SCRIPT_ERR_OK};
        BOOST_CHECK(!fixture.Verify(sigdata, serror));
    }
    // Without the claimant key nothing can be signed.
    {
        SignatureData sigdata;
        sigdata.sha256_preimages[preimage_hash] = correct_preimage;
        BOOST_CHECK(!fixture.Sign(fixture.Provider(nullptr), sigdata));
    }
}

BOOST_AUTO_TEST_CASE(signer_claims_htlc_tx_leaf_with_correct_preimage)
{
    CPQKey claimant_key;
    claimant_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(claimant_key.IsValid());

    const std::vector<unsigned char> preimage(32, 0x6a);
    const std::vector<unsigned char> preimage_hash = Hash160Bytes(preimage);
    HtlcSignFixture fixture{BuildP2MRHTLCTxLeaf(preimage_hash, PQAlgorithm::ML_DSA_44, claimant_key.GetPubKey())};
    BOOST_REQUIRE(!fixture.leaf_script.empty());

    SignatureData sigdata;
    sigdata.hash160_preimages[preimage_hash] = preimage;
    BOOST_REQUIRE(fixture.Sign(fixture.Provider(&claimant_key), sigdata));
    CheckStandardHtlcWitness(sigdata, preimage, fixture.leaf_script, PQAlgorithm::ML_DSA_44);

    ScriptError serror{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK_MESSAGE(fixture.Verify(sigdata, serror), ScriptErrorString(serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(signer_rejects_htlc_tx_leaf_with_wrong_preimage)
{
    CPQKey claimant_key;
    claimant_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(claimant_key.IsValid());

    const std::vector<unsigned char> correct_preimage(32, 0x6b);
    const std::vector<unsigned char> wrong_preimage(32, 0x6c);
    const std::vector<unsigned char> preimage_hash = Hash160Bytes(correct_preimage);
    HtlcSignFixture fixture{BuildP2MRHTLCTxLeaf(preimage_hash, PQAlgorithm::ML_DSA_44, claimant_key.GetPubKey())};
    BOOST_REQUIRE(!fixture.leaf_script.empty());
    const FlatSigningProvider provider = fixture.Provider(&claimant_key);

    {
        SignatureData sigdata;
        sigdata.hash160_preimages[Hash160Bytes(wrong_preimage)] = wrong_preimage;
        BOOST_CHECK(!fixture.Sign(provider, sigdata));
    }
    {
        // Wrong preimage under the right hashlock: the witness fails verification, so signing
        // reports incomplete instead of handing back a claim that consensus would reject.
        SignatureData sigdata;
        sigdata.hash160_preimages[preimage_hash] = wrong_preimage;
        BOOST_CHECK(!fixture.Sign(provider, sigdata));
        ScriptError serror{SCRIPT_ERR_OK};
        BOOST_CHECK(!fixture.Verify(sigdata, serror));
    }
}

BOOST_AUTO_TEST_CASE(signer_still_claims_legacy_htlc_leaf)
{
    // Pre-existing htlc() locks (HASH160 + OP_CHECKSIGFROMSTACK) stay spendable by the
    // wallet even though policy no longer relays them.
    CPQKey claimant_key;
    claimant_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(claimant_key.IsValid());

    const std::vector<unsigned char> preimage(32, 0x7a);
    const std::vector<unsigned char> preimage_hash = Hash160Bytes(preimage);
    HtlcSignFixture fixture{BuildP2MRHTLCLeaf(preimage_hash, PQAlgorithm::ML_DSA_44, claimant_key.GetPubKey())};
    BOOST_REQUIRE(!fixture.leaf_script.empty());

    SignatureData sigdata;
    sigdata.hash160_preimages[preimage_hash] = preimage;
    BOOST_REQUIRE(fixture.Sign(fixture.Provider(&claimant_key), sigdata));
    const auto& stack = sigdata.scriptWitness.stack;
    BOOST_REQUIRE_EQUAL(stack.size(), 4U);
    BOOST_CHECK(stack[1] == preimage);
    BOOST_CHECK(stack[2] == fixture.leaf_script);

    ScriptError serror{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK_MESSAGE(fixture.Verify(sigdata, serror), ScriptErrorString(serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_OK);
}

BOOST_AUTO_TEST_CASE(refund_after_timeout_succeeds)
{
    const std::vector<unsigned char> sender_pubkey(MLDSA44_PUBKEY_SIZE, 0x99);
    const std::vector<unsigned char> script_bytes = BuildP2MRRefundLeaf(/*timeout=*/700, PQAlgorithm::ML_DSA_44, sender_pubkey);
    BOOST_REQUIRE(!script_bytes.empty());
    const CScript script{script_bytes.begin(), script_bytes.end()};

    std::vector<std::vector<unsigned char>> stack;
    stack.push_back(std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01));

    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 5000;
    ScriptError serror = SCRIPT_ERR_OK;
    const P2MRTemplateChecker checker{/*locktime_ok=*/true};
    BOOST_REQUIRE(EvalP2MRScript(script, stack, checker, execdata, serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_OK);
    BOOST_REQUIRE_EQUAL(stack.size(), 1U);
    BOOST_CHECK_EQUAL(CScriptNum(stack.back(), /*fRequireMinimal=*/true).GetInt64(), 1);
}

BOOST_AUTO_TEST_CASE(refund_before_timeout_fails)
{
    const std::vector<unsigned char> sender_pubkey(MLDSA44_PUBKEY_SIZE, 0xaa);
    const std::vector<unsigned char> script_bytes = BuildP2MRRefundLeaf(/*timeout=*/900, PQAlgorithm::ML_DSA_44, sender_pubkey);
    BOOST_REQUIRE(!script_bytes.empty());
    const CScript script{script_bytes.begin(), script_bytes.end()};

    std::vector<std::vector<unsigned char>> stack;
    stack.push_back(std::vector<unsigned char>(MLDSA44_SIGNATURE_SIZE, 0x01));

    ScriptExecutionData execdata;
    execdata.m_validation_weight_left_init = true;
    execdata.m_validation_weight_left = 5000;
    ScriptError serror = SCRIPT_ERR_OK;
    const P2MRTemplateChecker checker{/*locktime_ok=*/false};
    BOOST_CHECK(!EvalP2MRScript(script, stack, checker, execdata, serror));
    BOOST_CHECK_EQUAL(serror, SCRIPT_ERR_UNSATISFIED_LOCKTIME);
}

BOOST_AUTO_TEST_CASE(csv_multisig_rejects_non_bip68_sequence_bits)
{
    // csv_multi_pq requires >=2 pubkeys; threshold-1 of a 2-key set is the
    // smallest valid BuildP2MRCSVMultisigScript input.
    const std::vector<unsigned char> pk1(MLDSA44_PUBKEY_SIZE, 0x12);
    const std::vector<unsigned char> pk2(MLDSA44_PUBKEY_SIZE, 0x13);
    const std::vector<std::pair<PQAlgorithm, std::vector<unsigned char>>> keys{
        {PQAlgorithm::ML_DSA_44, pk1},
        {PQAlgorithm::ML_DSA_44, pk2},
    };
    const auto build = [&](int64_t sequence) {
        return BuildP2MRCSVMultisigScript(sequence, /*threshold=*/1, keys);
    };

    BOOST_CHECK(IsP2MRCSVSequenceBIP68Valid(144));
    BOOST_CHECK(IsP2MRCSVSequenceBIP68Valid(144 | CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG));
    BOOST_CHECK(!IsP2MRCSVSequenceBIP68Valid(100000));
    BOOST_CHECK(!IsP2MRCSVSequenceBIP68Valid(int64_t{1} << 16));

    BOOST_CHECK(!build(144).empty());
    BOOST_CHECK(!build(144 | CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG).empty());
    BOOST_CHECK(build(100000).empty());
    BOOST_CHECK(build(int64_t{1} << 16).empty());
    BOOST_CHECK(build(static_cast<int64_t>(CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG)).empty());
    BOOST_CHECK(build(0).empty());
    BOOST_CHECK(build(static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1).empty());
}

BOOST_AUTO_TEST_CASE(two_leaf_merkle_htlc)
{
    CPQKey oracle_key;
    oracle_key.MakeNewKey(PQAlgorithm::ML_DSA_44);
    BOOST_REQUIRE(oracle_key.IsValid());

    const std::vector<unsigned char> preimage_hash(32, 0xbb);
    const std::vector<unsigned char> sender_pubkey(MLDSA44_PUBKEY_SIZE, 0xcc);
    const std::vector<unsigned char> htlc_leaf = BuildP2MRHTLCSha256Leaf(
        preimage_hash, PQAlgorithm::ML_DSA_44, oracle_key.GetPubKey());
    const std::vector<unsigned char> refund_leaf = BuildP2MRRefundLeaf(/*timeout=*/1024, PQAlgorithm::ML_DSA_44, sender_pubkey);
    BOOST_REQUIRE(!htlc_leaf.empty());
    BOOST_REQUIRE(!refund_leaf.empty());

    const uint256 htlc_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, htlc_leaf);
    const uint256 refund_hash = ComputeP2MRLeafHash(P2MR_LEAF_VERSION, refund_leaf);
    const uint256 root = ComputeP2MRMerkleRoot({htlc_hash, refund_hash});
    const std::vector<unsigned char> program(root.begin(), root.end());

    std::vector<unsigned char> htlc_control;
    htlc_control.push_back(P2MR_LEAF_VERSION);
    htlc_control.insert(htlc_control.end(), refund_hash.begin(), refund_hash.end());
    BOOST_CHECK(VerifyP2MRCommitment(htlc_control, program, htlc_hash));

    std::vector<unsigned char> refund_control;
    refund_control.push_back(P2MR_LEAF_VERSION);
    refund_control.insert(refund_control.end(), htlc_hash.begin(), htlc_hash.end());
    BOOST_CHECK(VerifyP2MRCommitment(refund_control, program, refund_hash));
}

BOOST_AUTO_TEST_SUITE_END()
