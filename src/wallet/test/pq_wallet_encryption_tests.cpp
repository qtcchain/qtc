// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <outputtype.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace wallet {
namespace {

//! Keep the keypool tiny: every P2MR index costs an ML-DSA and an SLH-DSA key derivation.
constexpr int64_t PQ_ENCRYPTION_TEST_KEYPOOL{1};
//! An index outside the pre-generated keypool, so deriving it needs the seed rather than the cache.
constexpr int32_t PQ_ENCRYPTION_TEST_INDEX{2};
const SecureString PASSPHRASE{"pq-seed-passphrase"};

bool HasRecordOfType(WalletDatabase& db, const std::string& type)
{
    std::unique_ptr<DatabaseBatch> batch = db.MakeBatch(false);
    BOOST_REQUIRE(batch);
    std::unique_ptr<DatabaseCursor> cursor = batch->GetNewCursor();
    BOOST_REQUIRE(cursor);
    while (true) {
        DataStream key{};
        DataStream value{};
        const DatabaseCursor::Status status = cursor->Next(key, value);
        BOOST_REQUIRE(status != DatabaseCursor::Status::FAIL);
        if (status == DatabaseCursor::Status::DONE) break;
        std::string record_type;
        key >> record_type;
        if (record_type == type) return true;
    }
    return false;
}

template <typename... Args>
SerializeData MakeRecord(const Args&... args)
{
    DataStream s{};
    SerializeMany(s, args...);
    return {s.begin(), s.end()};
}

std::array<unsigned char, 32> MakePQSeed(unsigned char seed)
{
    std::array<unsigned char, 32> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<unsigned char>(seed + i);
    }
    return out;
}

//! Create a PQ-native descriptor wallet (two active P2MR managers), the way createwallet does.
std::shared_ptr<CWallet> CreatePQWallet(const WalletTestingSetup& setup)
{
    auto wallet = std::make_shared<CWallet>(setup.m_node.chain.get(), "", CreateMockableWalletDatabase());
    LOCK(wallet->cs_wallet);
    wallet->LoadMinVersion(FEATURE_LATEST);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet->m_keypool_size = PQ_ENCRYPTION_TEST_KEYPOOL;
    wallet->SetupDescriptorScriptPubKeyMans();
    return wallet;
}

std::shared_ptr<CWallet> LoadPQWallet(const WalletTestingSetup& setup, const MockableData& records)
{
    auto wallet = std::make_shared<CWallet>(setup.m_node.chain.get(), "", CreateMockableWalletDatabase(records));
    BOOST_REQUIRE_EQUAL(wallet->LoadWallet(), DBErrors::LOAD_OK);
    return wallet;
}

DescriptorScriptPubKeyMan& GetExternalP2MRSpkm(CWallet& wallet)
{
    auto* spkm = dynamic_cast<DescriptorScriptPubKeyMan*>(wallet.GetScriptPubKeyMan(OutputType::P2MR, /*internal=*/false));
    BOOST_REQUIRE(spkm != nullptr);
    return *spkm;
}

bool DescriptorHasPQSeed(DescriptorScriptPubKeyMan& spkm)
{
    LOCK(spkm.cs_desc_man);
    return spkm.GetWalletDescriptor().descriptor->ExtractPQSeed().has_value();
}

//! Expand the descriptor at `index` directly from its pqhd() providers (no cache), which requires the seed.
std::optional<CScript> ExpandP2MRScript(DescriptorScriptPubKeyMan& spkm, int32_t index)
{
    LOCK(spkm.cs_desc_man);
    const WalletDescriptor desc = spkm.GetWalletDescriptor();
    FlatSigningProvider provider;
    FlatSigningProvider out_keys;
    std::vector<CScript> scripts;
    if (!desc.descriptor->Expand(index, provider, scripts, out_keys)) return std::nullopt;
    if (scripts.size() != 1) return std::nullopt;
    return scripts[0];
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(pq_wallet_encryption_tests, WalletTestingSetup)

//! N-6: encrypting a wallet must move the PQ descriptor seed under the master key, the seed must
//! only become available on unlock, and unlock must reject a master key that does not authenticate.
BOOST_AUTO_TEST_CASE(pq_descriptor_seed_encrypted_on_encryptwallet)
{
    MockableData records;
    uint256 spkm_id;
    CScript expected_script;
    {
        auto wallet = CreatePQWallet(*this);
        DescriptorScriptPubKeyMan& spkm = GetExternalP2MRSpkm(*wallet);
        spkm_id = spkm.GetID();

        // Unencrypted wallets keep the plaintext record (on-disk format unchanged).
        BOOST_CHECK(HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEED));
        BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDCRYPT));
        BOOST_CHECK(!spkm.HaveCryptedPQSeeds());

        const auto pre_encryption = ExpandP2MRScript(spkm, PQ_ENCRYPTION_TEST_INDEX);
        BOOST_REQUIRE(pre_encryption.has_value());
        expected_script = *pre_encryption;

        BOOST_REQUIRE(wallet->EncryptWallet(PASSPHRASE));
        wallet->Flush();
        BOOST_CHECK(wallet->IsCrypted());

        // No plaintext PQ seed record survives encryption; the encrypted record replaces it.
        BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEED));
        BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDMAP));
        BOOST_CHECK(HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDCRYPT));
        BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDMAPCRYPT));
        BOOST_CHECK(spkm.HaveCryptedPQSeeds());
        {
            WalletBatch batch(wallet->GetDatabase());
            uint256 iv;
            std::vector<unsigned char> ciphertext;
            BOOST_CHECK(batch.ReadCryptedPQDescriptorSeed(spkm_id, iv, ciphertext));
            BOOST_CHECK(!ciphertext.empty());
            std::vector<unsigned char> plaintext;
            BOOST_CHECK(!batch.ReadPQDescriptorSeed(spkm_id, plaintext));
        }

        records = GetMockableDatabase(*wallet).m_records;
    }

    {
        auto wallet = LoadPQWallet(*this, records);
        BOOST_CHECK(wallet->IsCrypted());
        BOOST_CHECK(wallet->IsLocked());
        DescriptorScriptPubKeyMan& spkm = GetExternalP2MRSpkm(*wallet);
        BOOST_CHECK_EQUAL(spkm.GetID().ToString(), spkm_id.ToString());
        BOOST_CHECK(spkm.HaveCryptedPQSeeds());
        BOOST_CHECK(spkm.HavePrivateKeys());

        // While locked the seed is not in memory and nothing can be derived beyond the cached keypool.
        BOOST_CHECK(!DescriptorHasPQSeed(spkm));
        BOOST_CHECK(!ExpandP2MRScript(spkm, PQ_ENCRYPTION_TEST_INDEX).has_value());

        // A PQ-only manager has no crypted secp256k1 keys; a wrong master key must still be rejected.
        CKeyingMaterial wrong_master_key(WALLET_CRYPTO_KEY_SIZE, 0x5a);
        BOOST_CHECK(!spkm.CheckDecryptionKey(wrong_master_key));
        BOOST_CHECK(!DescriptorHasPQSeed(spkm));
        BOOST_CHECK(!wallet->Unlock(SecureString{"not-the-passphrase"}));
        BOOST_CHECK(wallet->IsLocked());
        BOOST_CHECK(!DescriptorHasPQSeed(spkm));

        // The right passphrase injects the seed and derivation matches the pre-encryption wallet.
        BOOST_REQUIRE(wallet->Unlock(PASSPHRASE));
        BOOST_CHECK(!wallet->IsLocked());
        BOOST_CHECK(DescriptorHasPQSeed(spkm));
        const auto post_unlock = ExpandP2MRScript(spkm, PQ_ENCRYPTION_TEST_INDEX);
        BOOST_REQUIRE(post_unlock.has_value());
        BOOST_CHECK(*post_unlock == expected_script);
        BOOST_CHECK(wallet->GetNewDestination(OutputType::P2MR, ""));

        // Unlocking never re-introduces plaintext seed material on disk.
        wallet->Flush();
        BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEED));
        BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDMAP));
        BOOST_CHECK(HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDCRYPT));
    }
}

//! importdescriptors into an encrypted wallet must persist the pqhd() seeds encrypted only, and
//! must refuse to import seed-bearing descriptors while locked.
BOOST_AUTO_TEST_CASE(pq_descriptor_import_into_encrypted_wallet)
{
    auto wallet = std::make_shared<CWallet>(m_node.chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet->cs_wallet);
        wallet->LoadMinVersion(FEATURE_LATEST);
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetWalletFlag(WALLET_FLAG_BLANK_WALLET);
        wallet->m_keypool_size = PQ_ENCRYPTION_TEST_KEYPOOL;
    }
    BOOST_REQUIRE(wallet->EncryptWallet(PASSPHRASE));
    BOOST_REQUIRE(wallet->IsCrypted());
    BOOST_REQUIRE(wallet->IsLocked());

    const std::array<unsigned char, 32> seed = MakePQSeed(0x42);
    const WalletDescriptor generated = GeneratePQWalletDescriptor(seed, /*internal=*/false);
    WalletDescriptor desc(generated.descriptor, generated.creation_time, /*range_start=*/0,
                          /*range_end=*/PQ_ENCRYPTION_TEST_KEYPOOL, /*next_index=*/0);
    const FlatSigningProvider no_keys;

    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->AddWalletDescriptor(desc, no_keys, "", /*internal=*/false) == nullptr);
    }
    BOOST_REQUIRE(wallet->Unlock(PASSPHRASE));

    uint256 spkm_id;
    {
        LOCK(wallet->cs_wallet);
        ScriptPubKeyMan* spkm = wallet->AddWalletDescriptor(desc, no_keys, "", /*internal=*/false);
        BOOST_REQUIRE(spkm != nullptr);
        spkm_id = spkm->GetID();
    }
    wallet->Flush();
    BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEED));
    BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDMAP));
    BOOST_CHECK(HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDCRYPT));
    BOOST_CHECK(HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEEDMAPCRYPT));

    // The seed round-trips through the encrypted records on reload + unlock.
    auto reloaded = LoadPQWallet(*this, GetMockableDatabase(*wallet).m_records);
    BOOST_CHECK(reloaded->IsLocked());
    auto* reloaded_spkm = dynamic_cast<DescriptorScriptPubKeyMan*>(reloaded->GetScriptPubKeyMan(spkm_id));
    BOOST_REQUIRE(reloaded_spkm != nullptr);
    BOOST_CHECK(reloaded_spkm->HaveCryptedPQSeeds());
    BOOST_CHECK(!DescriptorHasPQSeed(*reloaded_spkm));
    BOOST_REQUIRE(reloaded->Unlock(PASSPHRASE));
    {
        LOCK(reloaded_spkm->cs_desc_man);
        const auto injected = reloaded_spkm->GetWalletDescriptor().descriptor->ExtractPQSeed();
        BOOST_REQUIRE(injected.has_value());
        BOOST_CHECK(*injected == seed);
    }
}

//! Encrypted wallets created before PQ seed encryption still carry plaintext seed records; the
//! first unlock must migrate them to encrypted records.
BOOST_AUTO_TEST_CASE(pq_descriptor_plaintext_seed_migrated_on_unlock)
{
    MockableData records;
    uint256 spkm_id;
    std::vector<unsigned char> plaintext_seed;
    {
        auto wallet = CreatePQWallet(*this);
        spkm_id = GetExternalP2MRSpkm(*wallet).GetID();
        {
            WalletBatch batch(wallet->GetDatabase());
            BOOST_REQUIRE(batch.ReadPQDescriptorSeed(spkm_id, plaintext_seed));
            BOOST_REQUIRE_EQUAL(plaintext_seed.size(), 32U);
        }
        BOOST_REQUIRE(wallet->EncryptWallet(PASSPHRASE));
        wallet->Flush();
        records = GetMockableDatabase(*wallet).m_records;
    }

    // Rewrite the external manager's seed the way a pre-fix encrypted wallet stored it.
    BOOST_REQUIRE_EQUAL(records.erase(MakeRecord(DBKeys::WALLETDESCRIPTORPQSEEDCRYPT, spkm_id)), 1U);
    records[MakeRecord(DBKeys::WALLETDESCRIPTORPQSEED, spkm_id)] = MakeRecord(plaintext_seed);

    auto wallet = LoadPQWallet(*this, records);
    BOOST_CHECK(wallet->IsCrypted());
    BOOST_CHECK(wallet->IsLocked());
    DescriptorScriptPubKeyMan& spkm = GetExternalP2MRSpkm(*wallet);
    BOOST_CHECK(!spkm.HaveCryptedPQSeeds());
    BOOST_CHECK(HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEED));

    BOOST_REQUIRE(wallet->Unlock(PASSPHRASE));
    wallet->Flush();
    BOOST_CHECK(spkm.HaveCryptedPQSeeds());
    BOOST_CHECK(!HasRecordOfType(wallet->GetDatabase(), DBKeys::WALLETDESCRIPTORPQSEED));
    {
        WalletBatch batch(wallet->GetDatabase());
        uint256 iv;
        std::vector<unsigned char> ciphertext;
        BOOST_CHECK(batch.ReadCryptedPQDescriptorSeed(spkm_id, iv, ciphertext));
        BOOST_CHECK(!ciphertext.empty());
    }

    // The migrated record authenticates on a subsequent load + unlock and yields the same seed.
    auto reloaded = LoadPQWallet(*this, GetMockableDatabase(*wallet).m_records);
    DescriptorScriptPubKeyMan& reloaded_spkm = GetExternalP2MRSpkm(*reloaded);
    BOOST_CHECK(reloaded_spkm.HaveCryptedPQSeeds());
    BOOST_CHECK(!DescriptorHasPQSeed(reloaded_spkm));
    BOOST_REQUIRE(reloaded->Unlock(PASSPHRASE));
    {
        LOCK(reloaded_spkm.cs_desc_man);
        const auto injected = reloaded_spkm.GetWalletDescriptor().descriptor->ExtractPQSeed();
        BOOST_REQUIRE(injected.has_value());
        BOOST_CHECK(std::vector<unsigned char>(injected->begin(), injected->end()) == plaintext_seed);
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
