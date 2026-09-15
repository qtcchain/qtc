// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <logging.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>


using namespace util::hex_literals;

// Workaround MSVC bug triggering C7595 when calling consteval constructors in
// initializer lists.
// A fix may be on the way:
// https://developercommunity.visualstudio.com/t/consteval-conversion-function-fails/1579014
#if defined(_MSC_VER)
auto consteval_ctor(auto&& input) { return input; }
#else
#define consteval_ctor(input) (input)
#endif

static constexpr int32_t QTC_SHIELDED_SUNSET_HEIGHT{125'000};
static constexpr int32_t QTC_SHIELDED_POOL_CREDIT_DISABLE_HEIGHT{QTC_SHIELDED_SUNSET_HEIGHT};
static constexpr int32_t QTC_SHIELDED_DIRECT_SEND_PUBLIC_FLOW_DISABLE_HEIGHT{128'000};
// Future consensus-bundle activation point for post-sunset zero-output V2_SEND
// exact exits. Keep disabled until the release that coordinates this with the
// other shielded-exit consensus changes. When that height is chosen, set this
// constant for the production-like networks and keep it >= QTC_SHIELDED_SUNSET_HEIGHT.
static constexpr int32_t QTC_SHIELDED_V2_SEND_ZERO_OUTPUT_EXIT_ACTIVATION_HEIGHT{
    std::numeric_limits<int32_t>::max()};
static constexpr int32_t QTC_EMPTY_BLOCK_SUBSIDY_PENALTY_HEIGHT{130'000};
static constexpr int32_t QTC_V03210_HARDENING_HEIGHT{130'500};
static constexpr int32_t QTC_V03211_HARDENING_HEIGHT{132'000};
static constexpr int32_t QTC_SHIELDED_UNSHIELD_VELOCITY_END_HEIGHT{135'000};
static constexpr CAmount QTC_SHIELDED_UNSHIELD_VELOCITY_MIN_CAP{10'000 * COIN};

static CBlock CreateGenesisBlock(const char* pszTimestamp,
                                 const CScript& genesisOutputScript,
                                 uint32_t nTime,
                                 uint32_t nNonce,
                                 uint64_t nNonce64,
                                 uint32_t nBits,
                                 int32_t nVersion,
                                 const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nNonce64 = nNonce64;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

static CBlock CreateQTCGenesisBlock(uint32_t nTime,
                                    uint32_t nNonce,
                                    uint64_t nNonce64,
                                    uint32_t nBits,
                                    int32_t nVersion,
                                    const CAmount& genesisReward,
                                    uint16_t matmul_dim,
                                    const uint256& matmul_digest)
{
    const char* pszTimestamp = "QTC genesis — any-GPU MatMul PoW";
    // Unspendable P2MR commitment:
    // OP_2 <32-byte commitment>, commitment = SHA256("QTC P2MR Genesis - Quantum Safe Since Block 0")
    const auto genesis_script_bytes{ParseHex("52206535040ed4451045d161571a53ad8fe69d074273dc2cc5ac61f6fb46cb7a08be")};
    const CScript genesisOutputScript{genesis_script_bytes.begin(), genesis_script_bytes.end()};
    CBlock genesis = CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nNonce64, nBits, nVersion, genesisReward);
    genesis.matmul_dim = matmul_dim;
    // Deterministic genesis seeds derived for prevhash=0 and height=0.
    genesis.seed_a = uint256{"a8a82ec830e8346550cad66c4cf43985dddd6a056d4bed2a5dcace445fa924ab"};
    genesis.seed_b = uint256{"f9aaa742cdbfb26be3d22d743b548740ff0a9e00f9cc977c1fb03df85fdf978d"};
    genesis.matmul_digest = matmul_digest;
    return genesis;
}

static CBlock CreateShieldedV2DevGenesisBlock(uint32_t nTime,
                                              uint32_t nNonce,
                                              uint64_t nNonce64,
                                              uint32_t nBits,
                                              int32_t nVersion,
                                              const CAmount& genesisReward,
                                              uint16_t matmul_dim,
                                              const uint256& matmul_digest)
{
    const char* pszTimestamp = "QTC 14/Mar/2026 shieldedv2dev genesis";
    const auto genesis_script_bytes{ParseHex("5220afa45d6891836c7314dded4dbd0e7aacde3de0d7fa9a12aeac06e2296c794226")};
    const CScript genesisOutputScript{genesis_script_bytes.begin(), genesis_script_bytes.end()};
    CBlock genesis = CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nNonce64, nBits, nVersion, genesisReward);
    genesis.matmul_dim = matmul_dim;
    genesis.seed_a = uint256{"a8a82ec830e8346550cad66c4cf43985dddd6a056d4bed2a5dcace445fa924ab"};
    genesis.seed_b = uint256{"f9aaa742cdbfb26be3d22d743b548740ff0a9e00f9cc977c1fb03df85fdf978d"};
    genesis.matmul_digest = matmul_digest;
    return genesis;
}

/**
 * Main network on which people trade goods and services.
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        m_chain_type = ChainType::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210'000; // QTC: Bitcoin's schedule (4-year halvings at 600 s)
        consensus.script_flag_exceptions.clear(); // New chain has no exceptions
        consensus.BIP34Height = 0;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 0;
        consensus.BIP66Height = 0;
        consensus.CSVHeight = 0;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        // MatMul powLimit calibrated for fast-phase SLA targeting ~0.25s blocks
        // on current Apple Silicon throughput (n=512), while retaining compact
        // headroom above genesis nBits so bootstrap scaling is not clamped out.
        // 2026-03-08 retune: eased from 0x205aa936 to 0x2066c154 based on live
        // throughput telemetry (~3.53 bps, ~0.283s over a 30s run) to target
        // the configured fast-phase SLA (~0.25s mean) on this host profile.
        // QTC Option B (QTC-LAUNCH-SAFETY.md D1/D3): ASERT governs from genesis and
        // this powLimit IS the hard floor. Compact-exact (0x1e033333 = 0x033333 * 2^216)
        // and the genesis nBits below MUST equal
        // compact(powLimit): consensus clamps a looser anchor, but rpc GetTarget
        // aborts on getblockheader 0 otherwise (verified in the §7 simulation).
        // SIZED 2026-09-10 (security review H2, D3): 0x033333 * 2^216, compact 0x1e033333,
        // P = 1.91e-7 per full digest. One RTX A6000 at its measured 8,800 digests/s
        // (n=512, oracle v2, digest v4, containerized) holds the 600 s schedule alone at
        // the floor; A6000 + RTX 4000 Ada (est.) ~400 s until ASERT settles (~163 blocks
        // ahead over ~8 days); 100 CPU cores ~6 floor blocks/day. Model and alternatives:
        // iCloud QTC/software/QTC_powLimit_Sizing_2026-09-10 (powlimit_sizing.py). Re-run the
        // model if the launch fleet changes materially; genesis is regenerated at launch (H1).
        consensus.powLimit = uint256{"0000033333000000000000000000000000000000000000000000000000000000"};
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 600; // QTC: 10-minute blocks (Bitcoin)
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = true;
        consensus.fPowNoRetargeting = false;
        consensus.fKAWPOW = false;
        consensus.fSkipKAWPOWValidation = false;
        consensus.fReducedDataLimits = true;
        consensus.fEnforceP2MROnlyOutputs = true;
        consensus.nKAWPOWHeight = std::numeric_limits<int>::max();
        consensus.fMatMulPOW = true;
        consensus.nMatMulDimension = 512;
        consensus.nMatMulTranscriptBlockSize = 16;
        consensus.nMatMulNoiseRank = 8;
        consensus.nMatMulValidationWindow = 1000;
        consensus.nMatMulPhase2FailBanThreshold = 1;
        consensus.fMatMulStrictPunishment = false;
        consensus.nMatMulSnapshotInterval = 10'000;
        consensus.nMatMulProofPruneDepth = 10'000;
        // Freivalds' O(n^2) probabilistic verification (k=2 rounds, error < 2^-62).
        consensus.fMatMulFreivaldsEnabled = true;
        consensus.nMatMulFreivaldsRounds = 2;
        // The static "require payload" flag stays false, but the Freivalds product
        // payload is already CONSENSUS-REQUIRED at and above nMatMulProductDigestHeight
        // (61'000) via IsMatMulProductPayloadRequired(); the flag is only a legacy
        // global override for networks that require it from genesis. Because the C'
        // product payload is a trailing CBlock appendage that BIP152 compact blocks
        // cannot carry, compact-block serving is intentionally disabled for blocks at
        // these heights (a reconstructed payload-less block would fail validation) --
        // see ProcessGetBlockData in net_processing.cpp. There is no scheduled upgrade
        // that re-enables compact serving; that would require a payload-carrying P2P
        // extension (getmatmulproof/matmulproof, qtc-matmul-pow-spec.md S13.3).
        consensus.fMatMulRequireProductPayload = false;
        consensus.fMatMulRejectLegacyPayloadVectors = true; // QTC security review N-5
        // QTC D8: all QTC hardening active from genesis (no flag-day replay).
        consensus.nMatMulFreivaldsBindingHeight = 0;
        consensus.nMatMulProductDigestHeight = 0;
        consensus.nMaxReorgDepth = 12;
        consensus.nReorgProtectionStartHeight = 0; // QTC D8
        // QTC D8: QTC's empty-block subsidy penalty (130,000 / strict 130,500) was
        // rolled back forward-only at 132,000; the steady state is 'never active'.
        consensus.nEmptyBlockSubsidyPenaltyHeight = std::numeric_limits<int32_t>::max();
        consensus.nEmptyBlockSubsidyStrictPenaltyHeight = std::numeric_limits<int32_t>::max();
        consensus.nEmptyBlockSubsidyPenaltyEndHeight = std::numeric_limits<int32_t>::max();
        consensus.nPowTargetSpacingFastMs = 250;
        // Fast-phase bootstrap scale for heights [0, nFastMineHeight). Effective
        // ease is bounded by powLimit; keep this >1 so fast bootstrap can
        // converge to the configured floor.
        consensus.nFastMineDifficultyScale = 6;
        consensus.nPowTargetSpacingNormal = 600;
        // QTC Option B: no fast-mine bootstrap phase. nPowTargetSpacingFastMs and
        // nFastMineDifficultyScale above are inert with nFastMineHeight = 0.
        consensus.nFastMineHeight = 0;
        // DGW is NOT used for MatMul mining. These heights are disabled.
        // ASERT governs all difficulty adjustment from nFastMineHeight onward.
        // Do not re-enable DGW -- see pow.cpp design invariant comments.
        consensus.nDgwAsymmetricClampHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwEasingBoostHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwWindowAlignmentHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwSlewGuardHeight = std::numeric_limits<int32_t>::max();
        // ASERT activates at nFastMineHeight. This MUST equal nFastMineHeight.
        consensus.nMatMulAsertHeight = 0;
        // QTC Option B (D2): QTC's post-incident 4 h half-life from genesis.
        consensus.nMatMulAsertHalfLife = 172'800; // 2 days = 288 blocks at 600 s (Bitcoin Cash ASERT tuning)
        // Inert with a genesis anchor (next_height == nMatMulAsertHeight is never
        // reached); must be non-zero for ValidateMatMulAsertParams.
        consensus.nMatMulAsertBootstrapFactor = 1;
        // No retune or half-life upgrade needed — fresh chain starts with
        // the target 3,600s half-life directly.
        consensus.nMatMulAsertRetuneHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetuneHardeningFactor = 1;
        consensus.nMatMulAsertRetune2Height = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetune2TargetNum = 1;
        consensus.nMatMulAsertRetune2TargetDen = 1;
        consensus.nMatMulAsertHalfLifeUpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertHalfLifeUpgrade = 172'800; // unused (upgrade height disabled)
        // Height 118,482 is approximately six hours from the observed public
        // tip near 118,242 at the 90-second target spacing, while bounding
        // future-dated timestamp shocks to one ASERT half-life.
        consensus.nMatMulMaxFutureMtpDriftHeight = 0; // QTC Option B (D4): from genesis
        // The bound is relative to the parent's MTP, and the 11-block median lags ~5
        // blocks, so the chain clock can advance at most drift/6 per block. At 600 s
        // spacing 3,600 s would pin that to exactly one target spacing and freeze
        // post-stall ASERT easing (regtest stall simulation, 2026-09-07). Keep the
        // 90 s design's ratio instead: drift = tau / 4 = 43,200 s, i.e. a timestamp
        // shock is still bounded to a quarter half-life per block.
        consensus.nMatMulMaxFutureMtpDrift = 43'200;
        // a5 fix: flag-day activation of the timewarp/drift bound reconciliation. Mainnet is
        // already past the only drift-cap activation boundary (118,482), so no inversion can
        // occur here and the reconciliation is behaviorally inert -- it is scheduled at the
        // shared height-125,000 hardening flag day for rollout consistency and to protect any future
        // network that activates the drift cap at a non-genesis height.
        consensus.nMatMulTimewarpReconcileHeight = 0; // QTC Option B (D4): a5 reconcile from genesis
        // Hardened pre-hash epsilon (18 bits) has been active on mainnet since
        // the historical ASERT transition at 50,000.
        // QTC Option B (D5): hardened 18-bit header gate from block 0.
        consensus.nMatMulPreHashEpsilonBits = 18;
        consensus.nMatMulPreHashEpsilonBitsUpgradeHeight = 0;
        consensus.nMatMulPreHashEpsilonBitsUpgrade = 18;
        // E1 hardening: after the shielded sunset boundary, MatMul seeds are
        // bound to the mutable header so miners cannot reuse one fixed A/B
        // instance across nonce attempts.
        consensus.nMatMulNonceSeedHeight = 0; // QTC D8
        // v0.32.10 hardening: bind MatMul seeds to the actual parent MTP so
        // templates cannot be prebuilt against one parent and replayed across
        // alternate withheld parents.
        consensus.nMatMulParentMtpSeedHeight = 0; // QTC D8
        consensus.nMaxBlockWeight = 24'000'000;
        consensus.nMaxBlockSerializedSize = 24'000'000;
        consensus.nMaxBlockSigOpsCost = 480'000;
        consensus.nDefaultBlockMaxWeight = 24'000'000;
        consensus.nDefaultMempoolMaxSizeMB = 2048;
        consensus.nMaxShieldedTxSize = 6'500'000;
        consensus.nMaxShieldedRingSize = 32;
        consensus.nShieldedMerkleTreeDepth = 32;
        consensus.nShieldedPoolActivationHeight = 0;
        // QTC D8: shielded proof/format hardening from genesis.
        consensus.nShieldedTxBindingActivationHeight = 0;
        consensus.nShieldedBridgeTagActivationHeight = 0;
        consensus.nShieldedSmileRiceCodecDisableHeight = 0;
        consensus.nShieldedMatRiCTDisableHeight = 0;
        consensus.nShieldedSpendPathRecoveryActivationHeight = 0;
        consensus.nShieldedC002ActivationHeight = 0;
        consensus.nShieldedPQ128UpgradeHeight = std::numeric_limits<int32_t>::max();
        // QTC D9 (product decision, see QTC-LAUNCH-SAFETY.md §9): launch in QTC's
        // LIVE post-125,000 rule state -- the shielded pool is sunset from block 0
        // (no shielding, no pool credit, no direct-send public flow; recovery-exit
        // path active against an empty tree). Re-opening a pool later is a normal
        // activation height; closing a live one is the path that cost QTC.
        consensus.nShieldedPoolCreditDisableHeight = 0;
        consensus.nShieldedSunsetHeight = 0;
        consensus.nShieldedDirectSendPublicFlowDisableHeight = 0;
        consensus.nShieldedV2SendZeroOutputExitActivationHeight =
            QTC_SHIELDED_V2_SEND_ZERO_OUTPUT_EXIT_ACTIVATION_HEIGHT;
        // QTC security review: the pool is sunset from genesis and can never hold value, so no recovery exit can
        // ever succeed; disable the whole shielded surface (S-1/S-2/C-3/C-4) instead of exercising it.
        consensus.nShieldedRecoveryExitActivationHeight = std::numeric_limits<int32_t>::max();
        consensus.fShieldedPoolDisabled = true;
        // v0.32.0-v0.32.12: shielded unshield (z->t) velocity cap from the 125,000
        // sunset through block 134,999. The v0.32.11 minimum-cap floor still starts at
        // 132,000, and v0.32.12 ends the quota at 135,000 after the recovery window has
        // matured so remaining legacy exits are no longer rate-limited.
        // QTC D9: QTC's post-sunset unshield-velocity window (125,000-134,999) is a
        // closed recovery window; with an empty pool from genesis it never opens.
        consensus.nShieldedUnshieldVelocityActivationHeight = std::numeric_limits<int32_t>::max();
        consensus.nShieldedUnshieldVelocityEndHeight = std::numeric_limits<int32_t>::max();
        consensus.nShieldedUnshieldVelocityMinCapHeight = std::numeric_limits<int32_t>::max();
        consensus.nShieldedUnshieldVelocityMinCap = 0;
        consensus.nShieldedSettlementAnchorMaturity = 6;
        consensus.nMLDSADisableHeight = std::numeric_limits<int32_t>::max();
        consensus.nRuleChangeActivationThreshold = 1815; // 90% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;

        // Mainnet anchor refreshed on 2026-07-10 at height 155'700 from a
        // synced canonical node so stale history below the current public
        // release floor is rejected quickly.
        consensus.nMinimumChainWork = uint256{}; // fresh chain: no accumulated work to require yet
        // No anchored assume-valid block on a brand-new chain.
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        // QTC mainnet network magic — distinct from QTC (b7 54 58 xx) so QTC and
        // QTC nodes never cross-connect. 0x51 0x54 0x43 = 'Q','T','C'.
        pchMessageStart[0] = 0x51;
        pchMessageStart[1] = 0x54;
        pchMessageStart[2] = 0x43; // 'C'
        pchMessageStart[3] = 0x01;
        nDefaultPort = 19755;
        nPruneAfterHeight = 100000;
        // Measured from the 2026-07-10 mainnet archive datadirs near height
        // 155'870: ~104 GB of blocks plus chain/shielded state, rounded up so
        // users see a conservative disk estimate before sync begins.
        m_assumed_blockchain_size = 106;
        m_assumed_chain_state_size = 1;

        genesis = CreateQTCGenesisBlock(
            1789063200,  // Sep 10, 2026 18:00:00 UTC — consensus fork v0.0.5 (REGENERATE AGAIN within hours of the real launch, security review H1)
            0,
            1,
            0x1e033333,  // == compact(powLimit); QTC Option B at 600 s, floor sized from the measured A6000 (H2)
            1,
            consensus.nInitialSubsidy,
            static_cast<uint16_t>(consensus.nMatMulDimension),
            uint256{"07226e4fdc368a067ef904b9fdddf9763e2782fda4e695788240077805643edd"});
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"9ba00506445039aa7315dc1ce61eded19ec75d31edbfed3643cb1e4f3c3db8e2"});
        assert(genesis.hashMerkleRoot == uint256{"68668615ec36015c9eacfa8a3c3c95b1cb5f78454e8d1aa58e3e94cbad3ade23"});
        // AUDIT D1: validate the immutable MatMul-ASERT schedule at construction so
        // an invalid parameter set aborts node startup instead of failing closed
        // (hardest target) at some future block. ValidateMatMulAsertParams is a pure
        // function of the params; the height argument is log context only.
        assert(!consensus.fMatMulPOW ||
               ValidateMatMulAsertParams(consensus, consensus.nMatMulAsertHeight));

        // QTC address prefixes (distinct from QTC). PUBKEY 58 → base58 'Q' lead;
        // SECRET = PUBKEY + 128 per convention.
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,58);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,63);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,186);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "qtc";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        // Live bootstrap DNS seeds for mainnet peer discovery. Keep these as
        // DNS names, not hard-coded IPs, so seed-host rotation does not require
        // a binary update. The two names are served by independent DNS
        // providers under independently registered domains so that a single
        // provider or registrar outage cannot take down bootstrap.
        vSeeds.clear();
        vSeeds.emplace_back("seed.qtc.gold.");
        vSeeds.emplace_back("seed.qtc.exchange.");

        // Fixed seeds: the launch network's inbound-capable public nodes, compiled
        // from contrib/seeds/nodes_main.txt (see contrib/seeds/README.md). They are
        // the fallback when DNS resolution is unavailable.
        vFixedSeeds = std::vector<uint8_t>{std::begin(chainparams_seed_main), std::end(chainparams_seed_main)};

        // New chain: only the genesis checkpoint. Add real checkpoints once QTC
        // has accumulated history and you want to pin against deep reorgs.
        checkpointData = {
            {
                {0, consensus.hashGenesisBlock},
            }
        };
        // No assumeutxo snapshots and no historical chain-tx stats on a fresh chain.
        m_assumeutxo_data = {};
        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0,
        };
    }
};

/**
 * Testnet (v3): public test network which is reset from time to time.
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        m_chain_type = ChainType::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210'000; // QTC: Bitcoin's schedule (4-year halvings at 600 s)
        consensus.BIP34Height = 0;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 0;
        consensus.BIP66Height = 0;
        consensus.CSVHeight = 0;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        // QTC 2026-09-15 (QTC-LAUNCH-SAFETY.md S14): every MatMul / ASERT / drift /
        // timewarp / pre-hash / payload field below mirrors CMainParams so the burn-in
        // rehearses the launch consensus. powLimit is launch floor candidate D
        // (0x011da5 * 2^216, compact 0x1e011da5); genesis nBits MUST equal compact(powLimit).
        consensus.powLimit = uint256{"0000011da5000000000000000000000000000000000000000000000000000000"}; // compact 0x1e011da5, launch floor candidate D
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 600; // QTC: 10-minute blocks (Bitcoin)
        consensus.fPowAllowMinDifficultyBlocks = false; // as mainnet
        consensus.enforce_BIP94 = true;
        consensus.fPowNoRetargeting = false;
        consensus.fKAWPOW = false;
        consensus.fSkipKAWPOWValidation = false;
        consensus.fReducedDataLimits = true;
        consensus.fEnforceP2MROnlyOutputs = true;
        consensus.nKAWPOWHeight = std::numeric_limits<int>::max();
        consensus.fMatMulPOW = true;
        consensus.nMatMulDimension = 512;
        consensus.nMatMulTranscriptBlockSize = 16;
        consensus.nMatMulNoiseRank = 8;
        consensus.nMatMulValidationWindow = 1000;
        consensus.nMatMulPhase2FailBanThreshold = 1;
        consensus.fMatMulStrictPunishment = false;
        consensus.nMatMulSnapshotInterval = 10'000;
        consensus.nMatMulProofPruneDepth = 10'000;
        consensus.fMatMulFreivaldsEnabled = true;
        consensus.nMatMulFreivaldsRounds = 2;
        consensus.fMatMulRequireProductPayload = false; // as mainnet: payload is consensus-required from genesis via nMatMulProductDigestHeight = 0
        consensus.fMatMulRejectLegacyPayloadVectors = true; // QTC security review N-5
        consensus.nMatMulFreivaldsBindingHeight = 0; // QTC D8, as mainnet
        consensus.nMatMulProductDigestHeight = 0; // QTC D8, as mainnet
        consensus.nMaxReorgDepth = 12;
        consensus.nReorgProtectionStartHeight = 61'000;
        consensus.nEmptyBlockSubsidyPenaltyHeight = QTC_EMPTY_BLOCK_SUBSIDY_PENALTY_HEIGHT;
        consensus.nEmptyBlockSubsidyStrictPenaltyHeight = QTC_V03210_HARDENING_HEIGHT;
        consensus.nEmptyBlockSubsidyPenaltyEndHeight = QTC_V03211_HARDENING_HEIGHT;
        consensus.nPowTargetSpacingFastMs = 250;
        consensus.nFastMineDifficultyScale = 6; // inert with nFastMineHeight = 0
        consensus.nPowTargetSpacingNormal = 600;
        consensus.nFastMineHeight = 0; // QTC Option B: no fast-mine bootstrap phase
        // DGW is NOT used for MatMul mining -- ASERT only. See pow.cpp.
        consensus.nDgwAsymmetricClampHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwEasingBoostHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwWindowAlignmentHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwSlewGuardHeight = std::numeric_limits<int32_t>::max();
        // ASERT activates at nFastMineHeight. This MUST equal nFastMineHeight.
        consensus.nMatMulAsertHeight = 0; // genesis anchor, as mainnet
        consensus.nMatMulAsertHalfLife = 172'800; // QTC: 2 days at 600 s
        consensus.nMatMulAsertBootstrapFactor = 1; // inert with a genesis anchor
        // No retune or half-life upgrade needed — fresh chain starts with
        // the target 3,600s half-life directly.
        consensus.nMatMulAsertRetuneHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetuneHardeningFactor = 1;
        consensus.nMatMulAsertRetune2Height = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetune2TargetNum = 1;
        consensus.nMatMulAsertRetune2TargetDen = 1;
        consensus.nMatMulAsertHalfLifeUpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertHalfLifeUpgrade = 172'800;
        // QTC Option B (D4): MTP drift bound and a5 timewarp reconciliation from genesis, as mainnet.
        consensus.nMatMulMaxFutureMtpDriftHeight = 0;
        consensus.nMatMulMaxFutureMtpDrift = 43'200; // tau / 4 at 600 s
        consensus.nMatMulTimewarpReconcileHeight = 0;
        // QTC Option B (D5): hardened 18-bit header gate from block 0, as mainnet.
        consensus.nMatMulPreHashEpsilonBits = 18;
        consensus.nMatMulPreHashEpsilonBitsUpgradeHeight = 0;
        consensus.nMatMulPreHashEpsilonBitsUpgrade = 18;
        consensus.nMatMulNonceSeedHeight = 0; // QTC D8, as mainnet
        consensus.nMatMulParentMtpSeedHeight = 0; // QTC D8, as mainnet
        consensus.nMaxBlockWeight = 24'000'000;
        consensus.nMaxBlockSerializedSize = 24'000'000;
        consensus.nMaxBlockSigOpsCost = 480'000;
        consensus.nDefaultBlockMaxWeight = 24'000'000;
        consensus.nDefaultMempoolMaxSizeMB = 2048;
        consensus.nMaxShieldedTxSize = 6'500'000;
        consensus.nMaxShieldedRingSize = 32;
        consensus.nShieldedMerkleTreeDepth = 32;
        consensus.nShieldedPoolActivationHeight = 0;
        consensus.nShieldedTxBindingActivationHeight = 61'000;
        consensus.nShieldedBridgeTagActivationHeight = 61'000;
        consensus.nShieldedSmileRiceCodecDisableHeight = 61'000;
        consensus.nShieldedMatRiCTDisableHeight = 61'000;
        consensus.nShieldedSpendPathRecoveryActivationHeight = 88'000;
        consensus.nShieldedPQ128UpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nShieldedPoolCreditDisableHeight = QTC_SHIELDED_POOL_CREDIT_DISABLE_HEIGHT;
        consensus.nShieldedSunsetHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedDirectSendPublicFlowDisableHeight = QTC_SHIELDED_DIRECT_SEND_PUBLIC_FLOW_DISABLE_HEIGHT;
        consensus.nShieldedV2SendZeroOutputExitActivationHeight =
            QTC_SHIELDED_V2_SEND_ZERO_OUTPUT_EXIT_ACTIVATION_HEIGHT;
        consensus.nShieldedRecoveryExitActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        // v0.32.0-v0.32.12: shielded unshield (z->t) velocity cap from the 125,000
        // sunset through block 134,999. The v0.32.11 minimum-cap floor still starts at
        // 132,000, and v0.32.12 ends the quota at 135,000 after the recovery window has
        // matured so remaining legacy exits are no longer rate-limited.
        consensus.nShieldedUnshieldVelocityActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedUnshieldVelocityEndHeight = QTC_SHIELDED_UNSHIELD_VELOCITY_END_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCapHeight = QTC_V03211_HARDENING_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCap = QTC_SHIELDED_UNSHIELD_VELOCITY_MIN_CAP;
        consensus.nShieldedSettlementAnchorMaturity = 6;
        consensus.nMLDSADisableHeight = std::numeric_limits<int32_t>::max();
        consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        // Bootstrap floor: disabled (zero) for young chain; update once the
        // chain has matured and a representative cumulative work is known.
        consensus.nMinimumChainWork = uint256{};
        // Assume signatures valid up to genesis (updated post-launch).
        consensus.defaultAssumeValid = uint256{}; // fresh QTC chain: no anchored assume-valid block

        pchMessageStart[0] = 0x51;
        pchMessageStart[1] = 0x54;
        pchMessageStart[2] = 0x43; // 'C'
        pchMessageStart[3] = 0x02;
        nDefaultPort = 29755;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateQTCGenesisBlock(
            1789063200,  // Sep 10, 2026 18:00:00 UTC — consensus fork v0.0.5 (REGENERATE AGAIN within hours of the real launch, security review H1)
            0,
            238,
            0x1e011da5,  // == compact(powLimit); launch floor candidate D
            1,
            consensus.nInitialSubsidy,
            static_cast<uint16_t>(consensus.nMatMulDimension),
            uint256{"00230371b05217711a10cf44983c2ffc3d82da06369fd0e640b6d20c033e38da"});
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"6da52defc708089bc721409fccf224c549288b242cadc39243b6d12a37e7397c"});
        assert(genesis.hashMerkleRoot == uint256{"68668615ec36015c9eacfa8a3c3c95b1cb5f78454e8d1aa58e3e94cbad3ade23"});
        // AUDIT D1: validate the immutable MatMul-ASERT schedule at construction so
        // an invalid parameter set aborts node startup instead of failing closed
        // (hardest target) at some future block. ValidateMatMulAsertParams is a pure
        // function of the params; the height argument is log context only.
        assert(!consensus.fMatMulPOW ||
               ValidateMatMulAsertParams(consensus, consensus.nMatMulAsertHeight));

        // Testnet DNS seeds mirror the mainnet domains under a testnet- prefix;
        // fixed seeds (contrib/seeds/nodes_test.txt) provide the fallback.
        vSeeds.clear();
        vSeeds.emplace_back("testnet-seed.qtc.gold.");
        vSeeds.emplace_back("testnet-seed.qtc.exchange.");
        vFixedSeeds = std::vector<uint8_t>{std::begin(chainparams_seed_test), std::end(chainparams_seed_test)};

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tqtc";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                {0, consensus.hashGenesisBlock},
            }
        };

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0,
        };
    }
};

/**
 * Testnet (v4): public test network which is reset from time to time.
 */
class CTestNet4Params : public CChainParams {
public:
    CTestNet4Params() {
        m_chain_type = ChainType::TESTNET4;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210'000; // QTC: Bitcoin's schedule (4-year halvings at 600 s)
        consensus.BIP34Height = 0;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 0;
        consensus.BIP66Height = 0;
        consensus.CSVHeight = 0;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        // QTC 2026-09-15 (QTC-LAUNCH-SAFETY.md S14): every MatMul / ASERT / drift /
        // timewarp / pre-hash / payload field below mirrors CMainParams so the burn-in
        // rehearses the launch consensus. powLimit is launch floor candidate D
        // (0x011da5 * 2^216, compact 0x1e011da5); genesis nBits MUST equal compact(powLimit).
        consensus.powLimit = uint256{"0000011da5000000000000000000000000000000000000000000000000000000"}; // compact 0x1e011da5, launch floor candidate D
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 600; // QTC: 10-minute blocks (Bitcoin)
        consensus.fPowAllowMinDifficultyBlocks = false; // as mainnet
        consensus.enforce_BIP94 = true;
        consensus.fPowNoRetargeting = false;
        consensus.fKAWPOW = false;
        consensus.fSkipKAWPOWValidation = false;
        consensus.fReducedDataLimits = true;
        consensus.fEnforceP2MROnlyOutputs = true;
        consensus.nKAWPOWHeight = std::numeric_limits<int>::max();
        consensus.fMatMulPOW = true;
        consensus.nMatMulDimension = 512;
        consensus.nMatMulTranscriptBlockSize = 16;
        consensus.nMatMulNoiseRank = 8;
        consensus.nMatMulValidationWindow = 1000;
        consensus.nMatMulPhase2FailBanThreshold = 1;
        consensus.fMatMulStrictPunishment = false;
        consensus.nMatMulSnapshotInterval = 10'000;
        consensus.nMatMulProofPruneDepth = 10'000;
        consensus.fMatMulFreivaldsEnabled = true;
        consensus.nMatMulFreivaldsRounds = 2;
        consensus.fMatMulRequireProductPayload = false; // as mainnet: payload is consensus-required from genesis via nMatMulProductDigestHeight = 0
        consensus.fMatMulRejectLegacyPayloadVectors = true; // QTC security review N-5
        consensus.nMatMulFreivaldsBindingHeight = 0; // QTC D8, as mainnet
        consensus.nMatMulProductDigestHeight = 0; // QTC D8, as mainnet
        consensus.nMaxReorgDepth = 12;
        consensus.nReorgProtectionStartHeight = 61'000;
        consensus.nEmptyBlockSubsidyPenaltyHeight = QTC_EMPTY_BLOCK_SUBSIDY_PENALTY_HEIGHT;
        consensus.nEmptyBlockSubsidyStrictPenaltyHeight = QTC_V03210_HARDENING_HEIGHT;
        consensus.nEmptyBlockSubsidyPenaltyEndHeight = QTC_V03211_HARDENING_HEIGHT;
        consensus.nPowTargetSpacingFastMs = 250;
        consensus.nFastMineDifficultyScale = 6; // inert with nFastMineHeight = 0
        consensus.nPowTargetSpacingNormal = 600;
        consensus.nFastMineHeight = 0; // QTC Option B: no fast-mine bootstrap phase
        // DGW is NOT used for MatMul mining -- ASERT only. See pow.cpp.
        consensus.nDgwAsymmetricClampHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwEasingBoostHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwWindowAlignmentHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwSlewGuardHeight = std::numeric_limits<int32_t>::max();
        // ASERT activates at nFastMineHeight. This MUST equal nFastMineHeight.
        consensus.nMatMulAsertHeight = 0; // genesis anchor, as mainnet
        consensus.nMatMulAsertHalfLife = 172'800; // QTC: 2 days at 600 s
        consensus.nMatMulAsertBootstrapFactor = 1; // inert with a genesis anchor
        // No retune or half-life upgrade needed — fresh chain starts with
        // the target 3,600s half-life directly.
        consensus.nMatMulAsertRetuneHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetuneHardeningFactor = 1;
        consensus.nMatMulAsertRetune2Height = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetune2TargetNum = 1;
        consensus.nMatMulAsertRetune2TargetDen = 1;
        consensus.nMatMulAsertHalfLifeUpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertHalfLifeUpgrade = 172'800;
        // QTC Option B (D4): MTP drift bound and a5 timewarp reconciliation from genesis, as mainnet.
        consensus.nMatMulMaxFutureMtpDriftHeight = 0;
        consensus.nMatMulMaxFutureMtpDrift = 43'200; // tau / 4 at 600 s
        consensus.nMatMulTimewarpReconcileHeight = 0;
        // QTC Option B (D5): hardened 18-bit header gate from block 0, as mainnet.
        consensus.nMatMulPreHashEpsilonBits = 18;
        consensus.nMatMulPreHashEpsilonBitsUpgradeHeight = 0;
        consensus.nMatMulPreHashEpsilonBitsUpgrade = 18;
        consensus.nMatMulNonceSeedHeight = 0; // QTC D8, as mainnet
        consensus.nMatMulParentMtpSeedHeight = 0; // QTC D8, as mainnet
        consensus.nMaxBlockWeight = 24'000'000;
        consensus.nMaxBlockSerializedSize = 24'000'000;
        consensus.nMaxBlockSigOpsCost = 480'000;
        consensus.nDefaultBlockMaxWeight = 24'000'000;
        consensus.nDefaultMempoolMaxSizeMB = 2048;
        consensus.nMaxShieldedTxSize = 6'500'000;
        consensus.nMaxShieldedRingSize = 32;
        consensus.nShieldedMerkleTreeDepth = 32;
        consensus.nShieldedPoolActivationHeight = 0;
        consensus.nShieldedTxBindingActivationHeight = 61'000;
        consensus.nShieldedBridgeTagActivationHeight = 61'000;
        consensus.nShieldedSmileRiceCodecDisableHeight = 61'000;
        consensus.nShieldedMatRiCTDisableHeight = 61'000;
        consensus.nShieldedSpendPathRecoveryActivationHeight = 88'000;
        consensus.nShieldedPQ128UpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nShieldedPoolCreditDisableHeight = QTC_SHIELDED_POOL_CREDIT_DISABLE_HEIGHT;
        consensus.nShieldedSunsetHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedDirectSendPublicFlowDisableHeight = QTC_SHIELDED_DIRECT_SEND_PUBLIC_FLOW_DISABLE_HEIGHT;
        consensus.nShieldedV2SendZeroOutputExitActivationHeight =
            QTC_SHIELDED_V2_SEND_ZERO_OUTPUT_EXIT_ACTIVATION_HEIGHT;
        consensus.nShieldedRecoveryExitActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        // v0.32.0-v0.32.12: shielded unshield (z->t) velocity cap from the 125,000
        // sunset through block 134,999. The v0.32.11 minimum-cap floor still starts at
        // 132,000, and v0.32.12 ends the quota at 135,000 after the recovery window has
        // matured so remaining legacy exits are no longer rate-limited.
        consensus.nShieldedUnshieldVelocityActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedUnshieldVelocityEndHeight = QTC_SHIELDED_UNSHIELD_VELOCITY_END_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCapHeight = QTC_V03211_HARDENING_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCap = QTC_SHIELDED_UNSHIELD_VELOCITY_MIN_CAP;
        consensus.nShieldedSettlementAnchorMaturity = 6;
        consensus.nMLDSADisableHeight = std::numeric_limits<int32_t>::max();
        consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        // Bootstrap floor: disabled (zero) for young chain; update once the
        // chain has matured and a representative cumulative work is known.
        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{}; // fresh QTC chain: no anchored assume-valid block

        pchMessageStart[0] = 0x1c;
        pchMessageStart[1] = 0x16;
        pchMessageStart[2] = 0x3f;
        pchMessageStart[3] = 0x28;
        nDefaultPort = 48333;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        genesis = CreateQTCGenesisBlock(
            1789063200,  // Sep 10, 2026 18:00:00 UTC — consensus fork v0.0.5 (REGENERATE AGAIN within hours of the real launch, security review H1)
            0,
            238,
            0x1e011da5,  // == compact(powLimit); launch floor candidate D
            1,
            consensus.nInitialSubsidy,
            static_cast<uint16_t>(consensus.nMatMulDimension),
            uint256{"00230371b05217711a10cf44983c2ffc3d82da06369fd0e640b6d20c033e38da"});
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"6da52defc708089bc721409fccf224c549288b242cadc39243b6d12a37e7397c"});
        assert(genesis.hashMerkleRoot == uint256{"68668615ec36015c9eacfa8a3c3c95b1cb5f78454e8d1aa58e3e94cbad3ade23"});
        // AUDIT D1: validate the immutable MatMul-ASERT schedule at construction so
        // an invalid parameter set aborts node startup instead of failing closed
        // (hardest target) at some future block. ValidateMatMulAsertParams is a pure
        // function of the params; the height argument is log context only.
        assert(!consensus.fMatMulPOW ||
               ValidateMatMulAsertParams(consensus, consensus.nMatMulAsertHeight));

        // QTC does not operate a public testnet4 network: no DNS or fixed seeds
        // (chainparams_seed_testnet4 is empty; see contrib/seeds/README.md).
        vSeeds.clear();
        vFixedSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tqtc4";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                {0, consensus.hashGenesisBlock},
            }
        };

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0,
        };
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vFixedSeeds.clear();
        vSeeds.clear();

        if (!options.challenge) {
            // QTC does not operate a default signet.  When no custom
            // --signetchallenge is provided, use a trivial OP_TRUE
            // challenge so that tests and tooling can instantiate signet
            // params without crashing.  This creates an isolated signet
            // that cannot connect to any real network.
            //
            // Note: this constructor is also called from ChainTypeFromMagic()
            // during startup for message-magic detection, so we only log a
            // warning when -signet was explicitly selected (options.seeds is
            // populated or the caller is creating params for actual use).
            bin = {0x51};
        } else {
            bin = *options.challenge;
            LogPrintf("Signet with challenge %s\n", HexStr(bin));
        }

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;
        chainTxData = ChainTxData{
            0,
            0,
            0,
        };

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.nSubsidyHalvingInterval = 210'000; // QTC: Bitcoin's schedule (4-year halvings at 600 s)
        consensus.BIP34Height = 0;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 0;
        consensus.BIP66Height = 0;
        consensus.CSVHeight = 0;
        consensus.SegwitHeight = 0;
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = options.pow_target_spacing;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = true;
        consensus.fPowNoRetargeting = false;
        consensus.fKAWPOW = false;
        consensus.fSkipKAWPOWValidation = false;
        consensus.fReducedDataLimits = true;
        consensus.fEnforceP2MROnlyOutputs = true;
        consensus.nKAWPOWHeight = std::numeric_limits<int>::max();
        consensus.fMatMulPOW = true;
        consensus.nMatMulDimension = 512;
        consensus.nMatMulTranscriptBlockSize = 16;
        consensus.nMatMulNoiseRank = 8;
        consensus.nMatMulValidationWindow = 1000;
        consensus.nMatMulPhase2FailBanThreshold = 1;
        consensus.fMatMulStrictPunishment = false;
        consensus.nMatMulSnapshotInterval = 10'000;
        consensus.nMatMulProofPruneDepth = 10'000;
        consensus.fMatMulFreivaldsEnabled = true;
        consensus.nMatMulFreivaldsRounds = 2;
        consensus.fMatMulRequireProductPayload = false; // as mainnet: payload is consensus-required from genesis via nMatMulProductDigestHeight = 0
        consensus.fMatMulRejectLegacyPayloadVectors = true; // QTC security review N-5
        consensus.nMatMulFreivaldsBindingHeight = 0; // QTC D8, as mainnet
        consensus.nMatMulProductDigestHeight = 0; // QTC D8, as mainnet
        consensus.nMaxReorgDepth = 12;
        consensus.nReorgProtectionStartHeight = 61'000;
        consensus.nEmptyBlockSubsidyPenaltyHeight = QTC_EMPTY_BLOCK_SUBSIDY_PENALTY_HEIGHT;
        consensus.nEmptyBlockSubsidyStrictPenaltyHeight = QTC_V03210_HARDENING_HEIGHT;
        consensus.nEmptyBlockSubsidyPenaltyEndHeight = QTC_V03211_HARDENING_HEIGHT;
        consensus.nPowTargetSpacingFastMs = 250;
        consensus.nFastMineDifficultyScale = 6; // inert with nFastMineHeight = 0
        consensus.nPowTargetSpacingNormal = 600; // as mainnet
        consensus.nFastMineHeight = 0; // QTC Option B: no fast-mine bootstrap phase
        // DGW is NOT used for MatMul mining -- ASERT only. See pow.cpp.
        consensus.nDgwAsymmetricClampHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwEasingBoostHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwWindowAlignmentHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwSlewGuardHeight = std::numeric_limits<int32_t>::max();
        // ASERT activates at nFastMineHeight. This MUST equal nFastMineHeight.
        consensus.nMatMulAsertHeight = 0; // genesis anchor, as mainnet
        consensus.nMatMulAsertHalfLife = 172'800; // as mainnet: 2 days at 600 s
        consensus.nMatMulAsertBootstrapFactor = 1; // inert with a genesis anchor
        // No retune or half-life upgrade needed — fresh chain starts with
        // the target 3,600s half-life directly.
        consensus.nMatMulAsertRetuneHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetuneHardeningFactor = 1;
        consensus.nMatMulAsertRetune2Height = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertRetune2TargetNum = 1;
        consensus.nMatMulAsertRetune2TargetDen = 1;
        consensus.nMatMulAsertHalfLifeUpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertHalfLifeUpgrade = 172'800;
        // QTC Option B (D4): MTP drift bound and a5 timewarp reconciliation from genesis, as mainnet.
        consensus.nMatMulMaxFutureMtpDriftHeight = 0;
        consensus.nMatMulMaxFutureMtpDrift = 43'200; // tau / 4 at 600 s
        consensus.nMatMulTimewarpReconcileHeight = 0;
        // QTC Option B (D5): hardened 18-bit header gate from block 0, as mainnet.
        consensus.nMatMulPreHashEpsilonBits = 18;
        consensus.nMatMulPreHashEpsilonBitsUpgradeHeight = 0;
        consensus.nMatMulPreHashEpsilonBitsUpgrade = 18;
        consensus.nMatMulNonceSeedHeight = 0; // QTC D8, as mainnet
        consensus.nMatMulParentMtpSeedHeight = 0; // QTC D8, as mainnet
        consensus.nMaxBlockWeight = 24'000'000;
        consensus.nMaxBlockSerializedSize = 24'000'000;
        consensus.nMaxBlockSigOpsCost = 480'000;
        consensus.nDefaultBlockMaxWeight = 24'000'000;
        consensus.nDefaultMempoolMaxSizeMB = 2048;
        consensus.nMaxShieldedTxSize = 6'500'000;
        consensus.nMaxShieldedRingSize = 32;
        consensus.nShieldedMerkleTreeDepth = 32;
        consensus.nShieldedPoolActivationHeight = 0;
        consensus.nShieldedTxBindingActivationHeight = 61'000;
        consensus.nShieldedBridgeTagActivationHeight = 61'000;
        consensus.nShieldedSmileRiceCodecDisableHeight = 61'000;
        consensus.nShieldedMatRiCTDisableHeight = 61'000;
        consensus.nShieldedSpendPathRecoveryActivationHeight = 88'000;
        consensus.nShieldedPQ128UpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nShieldedPoolCreditDisableHeight = QTC_SHIELDED_POOL_CREDIT_DISABLE_HEIGHT;
        consensus.nShieldedSunsetHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedDirectSendPublicFlowDisableHeight = QTC_SHIELDED_DIRECT_SEND_PUBLIC_FLOW_DISABLE_HEIGHT;
        consensus.nShieldedV2SendZeroOutputExitActivationHeight =
            QTC_SHIELDED_V2_SEND_ZERO_OUTPUT_EXIT_ACTIVATION_HEIGHT;
        consensus.nShieldedRecoveryExitActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        // v0.32.0-v0.32.12: shielded unshield (z->t) velocity cap from the 125,000
        // sunset through block 134,999. The v0.32.11 minimum-cap floor still starts at
        // 132,000, and v0.32.12 ends the quota at 135,000 after the recovery window has
        // matured so remaining legacy exits are no longer rate-limited.
        consensus.nShieldedUnshieldVelocityActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedUnshieldVelocityEndHeight = QTC_SHIELDED_UNSHIELD_VELOCITY_END_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCapHeight = QTC_V03211_HARDENING_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCap = QTC_SHIELDED_UNSHIELD_VELOCITY_MIN_CAP;
        consensus.nShieldedSettlementAnchorMaturity = 6;
        consensus.nMLDSADisableHeight = std::numeric_limits<int32_t>::max();
        consensus.nRuleChangeActivationThreshold = 1512; // 75% for testchains
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.MinBIP9WarningHeight = 0;
        // QTC 2026-09-15 (QTC-LAUNCH-SAFETY.md S14): every MatMul / ASERT / drift /
        // timewarp / pre-hash / payload field below mirrors CMainParams so the burn-in
        // rehearses the launch consensus. powLimit is launch floor candidate D
        // (0x011da5 * 2^216, compact 0x1e011da5); genesis nBits MUST equal compact(powLimit).
        consensus.powLimit = uint256{"0000011da5000000000000000000000000000000000000000000000000000000"}; // compact 0x1e011da5, launch floor candidate D
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Activation of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 38333;
        nPruneAfterHeight = 1000;

        // Reuse the testnet genesis block for signet.
        genesis = CreateQTCGenesisBlock(
            1789063200,  // Sep 10, 2026 18:00:00 UTC — consensus fork v0.0.5 (REGENERATE AGAIN within hours of the real launch, security review H1)
            0,
            238,
            0x1e011da5,  // == compact(powLimit); launch floor candidate D
            1,
            consensus.nInitialSubsidy,
            static_cast<uint16_t>(consensus.nMatMulDimension),
            uint256{"00230371b05217711a10cf44983c2ffc3d82da06369fd0e640b6d20c033e38da"});
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"6da52defc708089bc721409fccf224c549288b242cadc39243b6d12a37e7397c"});
        assert(genesis.hashMerkleRoot == uint256{"68668615ec36015c9eacfa8a3c3c95b1cb5f78454e8d1aa58e3e94cbad3ade23"});
        // AUDIT D1: validate the immutable MatMul-ASERT schedule at construction so
        // an invalid parameter set aborts node startup instead of failing closed
        // (hardest target) at some future block. ValidateMatMulAsertParams is a pure
        // function of the params; the height argument is log context only.
        assert(!consensus.fMatMulPOW ||
               ValidateMatMulAsertParams(consensus, consensus.nMatMulAsertHeight));

        m_assumeutxo_data = {};

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tqtc";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 150;
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 90;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = opts.enforce_bip94;
        consensus.fPowNoRetargeting = true;
        consensus.fKAWPOW = false;
        consensus.fSkipKAWPOWValidation = !opts.matmul_strict;
        consensus.fReducedDataLimits = true;
        consensus.fEnforceP2MROnlyOutputs = false;
        consensus.nKAWPOWHeight = std::numeric_limits<int>::max();
        consensus.fMatMulPOW = true;
        consensus.fSkipMatMulValidation = !opts.matmul_strict;
        consensus.nMatMulDimension = 64;
        consensus.nMatMulTranscriptBlockSize = 8;
        consensus.nMatMulNoiseRank = 4;
        consensus.nMatMulValidationWindow = 10;
        consensus.nMatMulPhase2FailBanThreshold = std::numeric_limits<uint32_t>::max();
        consensus.fMatMulStrictPunishment = false;
        consensus.nMatMulSnapshotInterval = 10'000;
        consensus.nMatMulProofPruneDepth = 10'000;
        consensus.fMatMulFreivaldsEnabled = true;
        consensus.nMatMulFreivaldsRounds = 2;
        consensus.fMatMulRequireProductPayload = true;
        consensus.fMatMulRejectLegacyPayloadVectors = false; // QTC security review N-5 (regtest keeps legacy payload tests)
        consensus.nMatMulFreivaldsBindingHeight = 0;
        consensus.nMatMulProductDigestHeight = 0;
        consensus.nMatMulPreHashEpsilonBits = 0; // Disable pre-hash filter for fast regtest mining
        consensus.nMatMulPreHashEpsilonBitsUpgrade = consensus.nMatMulPreHashEpsilonBits;
        consensus.nMatMulGlobalVerifyBudgetPerMin = std::numeric_limits<uint32_t>::max(); // No global budget limit in regtest
        consensus.nPowTargetSpacingFastMs = 250;
        consensus.nFastMineDifficultyScale = 4;
        consensus.nPowTargetSpacingNormal = 90;
        consensus.nFastMineHeight = 0;
        // DGW is NOT used for MatMul mining -- ASERT only. See pow.cpp.
        consensus.nDgwAsymmetricClampHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwEasingBoostHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwWindowAlignmentHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwSlewGuardHeight = std::numeric_limits<int32_t>::max();
        // ASERT activates at nFastMineHeight (0 for regtest = immediate).
        consensus.nMatMulAsertHeight = 0;
        consensus.nMatMulAsertHalfLife = 14'400;
        if (opts.matmul_asert) {
            consensus.fPowNoRetargeting = false;
        }
        if (opts.matmul_dgw) {
            consensus.fPowNoRetargeting = false;
            consensus.fPowAllowMinDifficultyBlocks = false;
            // Keep a short fast phase so tests can mine both phases and still
            // exercise ASERT retargeting at practical regtest speed.
            consensus.nFastMineHeight = 2;
            consensus.nMatMulAsertHeight = 2;
        }
        if (opts.matmul_binding_height.has_value()) {
            consensus.nMatMulFreivaldsBindingHeight = *opts.matmul_binding_height;
        }
        if (opts.matmul_product_digest_height.has_value()) {
            consensus.nMatMulProductDigestHeight = *opts.matmul_product_digest_height;
        }
        if (opts.matmul_require_product_payload.has_value()) {
            consensus.fMatMulRequireProductPayload = *opts.matmul_require_product_payload;
        }
        if (opts.matmul_dimension.has_value()) {
            consensus.nMatMulDimension = *opts.matmul_dimension;
        }
        if (opts.matmul_transcript_block_size.has_value()) {
            consensus.nMatMulTranscriptBlockSize = *opts.matmul_transcript_block_size;
        }
        if (opts.matmul_noise_rank.has_value()) {
            consensus.nMatMulNoiseRank = *opts.matmul_noise_rank;
        }
        if (consensus.nMatMulTranscriptBlockSize == 0 ||
            consensus.nMatMulDimension % consensus.nMatMulTranscriptBlockSize != 0) {
            throw std::runtime_error(strprintf(
                "Invalid regtest MatMul shape: dimension %u must be divisible by transcript block size %u.",
                consensus.nMatMulDimension,
                consensus.nMatMulTranscriptBlockSize));
        }
        if (opts.matmul_pow_target_spacing.has_value()) {
            consensus.nPowTargetSpacing = *opts.matmul_pow_target_spacing;
            consensus.nPowTargetSpacingNormal = *opts.matmul_pow_target_spacing;
        }
        if (opts.matmul_asert_half_life.has_value()) {
            consensus.nMatMulAsertHalfLife = *opts.matmul_asert_half_life;
        }
        if (opts.matmul_asert_half_life_upgrade_height.has_value()) {
            consensus.nMatMulAsertHalfLifeUpgradeHeight = *opts.matmul_asert_half_life_upgrade_height;
            consensus.nMatMulAsertHalfLifeUpgrade = *opts.matmul_asert_half_life_upgrade;
        }
        if (opts.matmul_pow_limit.has_value()) {
            consensus.powLimit = *opts.matmul_pow_limit;
        }
        if (opts.matmul_max_future_mtp_drift.has_value()) {
            consensus.nMatMulMaxFutureMtpDrift = *opts.matmul_max_future_mtp_drift;
        }
        if (opts.matmul_max_future_mtp_drift_height.has_value()) {
            consensus.nMatMulMaxFutureMtpDriftHeight = *opts.matmul_max_future_mtp_drift_height;
        }
        if (opts.matmul_timewarp_reconcile_height.has_value()) {
            consensus.nMatMulTimewarpReconcileHeight = *opts.matmul_timewarp_reconcile_height;
        }
        if (opts.matmul_pre_hash_epsilon_bits_upgrade_height.has_value()) {
            consensus.nMatMulPreHashEpsilonBitsUpgradeHeight = *opts.matmul_pre_hash_epsilon_bits_upgrade_height;
            consensus.nMatMulPreHashEpsilonBitsUpgrade = *opts.matmul_pre_hash_epsilon_bits_upgrade;
        }
        if (opts.matmul_nonce_seed_height.has_value()) {
            consensus.nMatMulNonceSeedHeight = *opts.matmul_nonce_seed_height;
        }
        if (opts.matmul_parent_mtp_seed_height.has_value()) {
            consensus.nMatMulParentMtpSeedHeight = *opts.matmul_parent_mtp_seed_height;
        }
        consensus.nMaxBlockWeight = 24'000'000;
        consensus.nMaxBlockSerializedSize = 24'000'000;
        consensus.nMaxBlockSigOpsCost = 480'000;
        consensus.nDefaultBlockMaxWeight = 24'000'000;
        consensus.nDefaultMempoolMaxSizeMB = 2048;
        consensus.nMaxShieldedTxSize = 6'500'000;
        consensus.nMaxShieldedRingSize = 32;
        consensus.nShieldedMerkleTreeDepth = 32;
        consensus.nShieldedPoolActivationHeight = 0;
        consensus.nShieldedTxBindingActivationHeight =
            opts.shielded_tx_binding_activation_height.value_or(0);  // Activate at genesis for instant regtest
        consensus.nShieldedBridgeTagActivationHeight =
            opts.shielded_bridge_tag_activation_height.value_or(0);  // Activate at genesis for instant regtest
        consensus.nShieldedSmileRiceCodecDisableHeight =
            opts.shielded_smile_rice_codec_disable_height.value_or(0);  // Activate at genesis for instant regtest
        consensus.nShieldedMatRiCTDisableHeight =
            opts.shielded_matrict_disable_height.value_or(0);  // Activate at genesis for instant regtest
        consensus.nShieldedSpendPathRecoveryActivationHeight =
            opts.shielded_spend_path_recovery_activation_height.value_or(0);  // Activate at genesis for instant regtest
        consensus.nShieldedC002ActivationHeight =
            opts.shielded_c002_activation_height.value_or(consensus.nShieldedC002ActivationHeight);
        // v0.32.0 velocity cap: inert on regtest by default (so existing shielded tests are unaffected);
        // a functional test lowers it via -regtestshieldedunshieldvelocityactivationheight to exercise it.
        consensus.nShieldedUnshieldVelocityActivationHeight =
            opts.shielded_unshield_velocity_activation_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedUnshieldVelocityEndHeight =
            opts.shielded_unshield_velocity_end_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedUnshieldVelocityMinCapHeight =
            opts.shielded_unshield_velocity_min_cap_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedUnshieldVelocityMinCap =
            opts.shielded_unshield_velocity_min_cap.value_or(0);
        consensus.nShieldedPQ128UpgradeHeight =
            opts.shielded_pq128_upgrade_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedPoolCreditDisableHeight =
            opts.shielded_pool_credit_disable_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedSunsetHeight =
            opts.shielded_sunset_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedDirectSendPublicFlowDisableHeight =
            opts.shielded_direct_send_public_flow_disable_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedV2SendZeroOutputExitActivationHeight =
            opts.shielded_v2_send_zero_output_exit_activation_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedRecoveryExitActivationHeight =
            opts.shielded_recovery_exit_activation_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nShieldedRecoveryExitFrozenRoot =
            opts.shielded_recovery_exit_frozen_root.value_or(uint256{});
        if (opts.reorg_protection_start_height.has_value()) {
            consensus.nReorgProtectionStartHeight = *opts.reorg_protection_start_height;
            consensus.nMaxReorgDepth = 12;
        }
        if (opts.empty_block_subsidy_penalty_height.has_value()) {
            consensus.nEmptyBlockSubsidyPenaltyHeight = *opts.empty_block_subsidy_penalty_height;
        }
        if (opts.empty_block_subsidy_penalty_end_height.has_value()) {
            consensus.nEmptyBlockSubsidyPenaltyEndHeight = *opts.empty_block_subsidy_penalty_end_height;
        }
        consensus.nShieldedSettlementAnchorMaturity = 6;
        consensus.nMLDSADisableHeight = opts.mldsa_disable_height.value_or(std::numeric_limits<int32_t>::max());
        consensus.nRuleChangeActivationThreshold = 108; // 75% for testchains
        consensus.nMinerConfirmationWindow = 144; // Faster than normal for regtest (144 instead of 2016)

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        constexpr MessageStartChars default_message_start{0xfa, 0xbf, 0xb5, 0xda};
        constexpr uint16_t default_port{18444};
        constexpr uint32_t default_genesis_time{1296688602};
        constexpr uint32_t default_genesis_nonce{2};
        constexpr uint32_t default_genesis_bits{0x207fffff};
        constexpr int32_t default_genesis_version{1};

        pchMessageStart = opts.message_start.value_or(default_message_start);
        nDefaultPort = opts.default_port.value_or(default_port);
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        const uint32_t genesis_time = opts.genesis_time.value_or(default_genesis_time);
        const uint32_t genesis_nonce = opts.genesis_nonce.value_or(default_genesis_nonce);
        const uint32_t genesis_bits = opts.genesis_bits.value_or(default_genesis_bits);
        const int32_t genesis_version = opts.genesis_version.value_or(default_genesis_version);

        const bool custom_genesis =
            opts.genesis_time.has_value() ||
            opts.genesis_nonce.has_value() ||
            opts.genesis_bits.has_value() ||
            opts.genesis_version.has_value();
        const bool custom_consensus =
            custom_genesis ||
            !opts.activation_heights.empty() ||
            !opts.version_bits_parameters.empty() ||
            opts.enforce_bip94 ||
            opts.matmul_dgw ||
            opts.matmul_asert ||
            opts.matmul_binding_height.has_value() ||
            opts.matmul_product_digest_height.has_value() ||
            opts.matmul_require_product_payload.has_value() ||
            opts.matmul_dimension.has_value() ||
            opts.matmul_transcript_block_size.has_value() ||
            opts.matmul_noise_rank.has_value() ||
            opts.matmul_pow_target_spacing.has_value() ||
            opts.matmul_asert_half_life.has_value() ||
            opts.matmul_asert_half_life_upgrade_height.has_value() ||
            opts.matmul_nonce_seed_height.has_value() ||
            opts.matmul_parent_mtp_seed_height.has_value() ||
            opts.matmul_pow_limit.has_value() ||
            opts.matmul_max_future_mtp_drift.has_value() ||
            opts.matmul_max_future_mtp_drift_height.has_value() ||
            opts.matmul_timewarp_reconcile_height.has_value() ||
            opts.shielded_tx_binding_activation_height.has_value() ||
            opts.shielded_bridge_tag_activation_height.has_value() ||
            opts.shielded_smile_rice_codec_disable_height.has_value() ||
            opts.shielded_matrict_disable_height.has_value() ||
            opts.shielded_spend_path_recovery_activation_height.has_value() ||
            opts.shielded_pq128_upgrade_height.has_value() ||
            opts.shielded_pool_credit_disable_height.has_value() ||
            opts.shielded_sunset_height.has_value() ||
            opts.shielded_direct_send_public_flow_disable_height.has_value() ||
            opts.shielded_v2_send_zero_output_exit_activation_height.has_value() ||
            opts.reorg_protection_start_height.has_value() ||
            opts.empty_block_subsidy_penalty_height.has_value() ||
            opts.mldsa_disable_height.has_value();

        for (const auto& [dep, height] : opts.activation_heights) {
            switch (dep) {
            case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
                consensus.SegwitHeight = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
                consensus.BIP34Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
                consensus.BIP66Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
                consensus.BIP65Height = int{height};
                break;
            case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
                consensus.CSVHeight = int{height};
                break;
            }
        }

        for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
            consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
            consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
            consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
        }

        genesis = CreateQTCGenesisBlock(
            genesis_time,
            genesis_nonce,
            genesis_nonce,
            genesis_bits,
            genesis_version,
            consensus.nInitialSubsidy,
            static_cast<uint16_t>(consensus.nMatMulDimension),
            custom_genesis
                ? uint256{}
                : uint256{"7ff451fb9e39ebaa8447435600978167d9cb8b9ee1d6933eb5e1ad84d05a2a37"});
        consensus.hashGenesisBlock = genesis.GetHash();
        if (!custom_genesis && !opts.matmul_dimension.has_value()) {
        assert(consensus.hashGenesisBlock == uint256{"25d0b1c272072b56bb0e79aea8566b16378648775e7a517d9d022720f6a1fca6"});
        }
        assert(genesis.hashMerkleRoot == uint256{"68668615ec36015c9eacfa8a3c3c95b1cb5f78454e8d1aa58e3e94cbad3ade23"});
        // AUDIT D1: the -regtestmatmulaserthalflife* / -regtestmatmulpowtargetspacing
        // overrides above are applied before this point, so validate the FINAL
        // regtest ASERT schedule here. An operator-supplied invalid combination
        // (e.g. -regtestmatmulaserthalflifeupgradeheight at/below the ASERT anchor)
        // must fail at startup with a clear message, not fail closed (hardest
        // target) at every block at runtime.
        if (consensus.fMatMulPOW &&
            !ValidateMatMulAsertParams(consensus, consensus.nMatMulAsertHeight)) {
            throw std::runtime_error(strprintf(
                "Invalid regtest MatMul ASERT schedule: half-life %lld s, upgrade height %d / upgrade half-life %lld s, "
                "target spacing %lld s (see debug log for the failing check).",
                static_cast<long long>(consensus.nMatMulAsertHalfLife),
                consensus.nMatMulAsertHalfLifeUpgradeHeight,
                static_cast<long long>(consensus.nMatMulAsertHalfLifeUpgrade),
                static_cast<long long>(consensus.nPowTargetSpacing)));
        }

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        checkpointData = {
            {
                {0, consensus.hashGenesisBlock},
            }
        };

        if (!custom_consensus) {
            m_assumeutxo_data = {
                {
                    // Deterministic TestChain100Setup (regtest) snapshot metadata at height 110.
                    .height = 110,
                    .hash_serialized = AssumeutxoHash{uint256{"703ea63ca4ade1424f0ad35590309974e77c6c65b722121fd9e04dbc3174f0b5"}},
                    .m_chain_tx_count = 111,
                    .blockhash = consteval_ctor(uint256{"b6aaee11a59ad7cad06e57eceb2c822f1332826835a389c72b2037cedb2d29dc"}),
                },
                {
                    // Deterministic TestChain100Setup + QTC-compatible
                    // feature_assumeutxo extension using RAW_P2PKH wallet flows.
                    .height = 299,
                    .hash_serialized = AssumeutxoHash{uint256{"2e5dcf9f04328141c721b5615a32dc265da783050ba7bd3e436a48b5a2013ae1"}},
                    .m_chain_tx_count = 300,
                    .blockhash = consteval_ctor(uint256{"78e6ea382d4d5466b1d8421c1b8789e9c7cde9de8b6da4042be00ca2948a4860"}),
                },
                {
                    // Post-shielded-activation regtest snapshot for qtc-p2p
                    // fast-start testing. IsMockableChain() allows height-only
                    // matching, and validation treats all-zero blockhash /
                    // hash_serialized as a mockable-chain wildcard so any
                    // regtest snapshot at this height can be used.
                    .height = 61'010,
                    .hash_serialized = AssumeutxoHash{uint256{"0000000000000000000000000000000000000000000000000000000000000000"}},
                    .m_chain_tx_count = 61'011,
                    .blockhash = consteval_ctor(uint256{"0000000000000000000000000000000000000000000000000000000000000000"}),
                },
            };
        } else {
            // Consensus-altering regtest overrides invalidate canned snapshot metadata.
            m_assumeutxo_data.clear();
        }

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "qtcrt";
    }
};

class CShieldedV2DevParams : public CChainParams
{
public:
    CShieldedV2DevParams()
    {
        m_chain_type = ChainType::SHIELDEDV2DEV;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 150;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 24 * 60 * 60;
        consensus.nPowTargetSpacing = 90;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false;
        consensus.fPowNoRetargeting = true;
        consensus.fKAWPOW = false;
        consensus.fSkipKAWPOWValidation = true;
        consensus.fReducedDataLimits = true;
        consensus.fEnforceP2MROnlyOutputs = false;
        consensus.nKAWPOWHeight = std::numeric_limits<int>::max();
        consensus.fMatMulPOW = true;
        consensus.fSkipMatMulValidation = true;
        consensus.nMatMulDimension = 64;
        consensus.nMatMulTranscriptBlockSize = 8;
        consensus.nMatMulNoiseRank = 4;
        consensus.nMatMulValidationWindow = 10;
        consensus.nMatMulPhase2FailBanThreshold = std::numeric_limits<uint32_t>::max();
        consensus.fMatMulStrictPunishment = false;
        consensus.nMatMulSnapshotInterval = 10'000;
        consensus.nMatMulProofPruneDepth = 10'000;
        consensus.fMatMulFreivaldsEnabled = true;
        consensus.nMatMulFreivaldsRounds = 2;
        consensus.fMatMulRequireProductPayload = true;
        consensus.fMatMulRejectLegacyPayloadVectors = false; // QTC security review N-5 (dev chain keeps legacy payload tests)
        consensus.nMatMulFreivaldsBindingHeight = 0;
        consensus.nMatMulProductDigestHeight = 0;
        consensus.nMatMulPreHashEpsilonBits = 0;
        consensus.nMatMulPreHashEpsilonBitsUpgrade = consensus.nMatMulPreHashEpsilonBits;
        consensus.nMatMulGlobalVerifyBudgetPerMin = std::numeric_limits<uint32_t>::max();
        consensus.nPowTargetSpacingFastMs = 250;
        consensus.nFastMineDifficultyScale = 4;
        consensus.nPowTargetSpacingNormal = 90;
        consensus.nFastMineHeight = 0;
        consensus.nDgwAsymmetricClampHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwEasingBoostHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwWindowAlignmentHeight = std::numeric_limits<int32_t>::max();
        consensus.nDgwSlewGuardHeight = std::numeric_limits<int32_t>::max();
        consensus.nMatMulAsertHeight = 0;
        consensus.nMatMulAsertHalfLife = 14'400;
        consensus.nMaxBlockWeight = 24'000'000;
        consensus.nMaxBlockSerializedSize = 24'000'000;
        consensus.nMaxBlockSigOpsCost = 480'000;
        consensus.nDefaultBlockMaxWeight = 24'000'000;
        consensus.nDefaultMempoolMaxSizeMB = 2048;
        consensus.nMaxShieldedTxSize = 6'500'000;
        consensus.nMaxShieldedRingSize = 32;
        consensus.nShieldedMerkleTreeDepth = 32;
        consensus.nShieldedPoolActivationHeight = 0;
        consensus.nShieldedTxBindingActivationHeight = 61'000;
        consensus.nShieldedBridgeTagActivationHeight = 61'000;
        consensus.nShieldedSmileRiceCodecDisableHeight = 61'000;
        consensus.nShieldedMatRiCTDisableHeight = 61'000;
        consensus.nShieldedSpendPathRecoveryActivationHeight = 88'000;
        consensus.nShieldedPQ128UpgradeHeight = std::numeric_limits<int32_t>::max();
        consensus.nShieldedPoolCreditDisableHeight = QTC_SHIELDED_POOL_CREDIT_DISABLE_HEIGHT;
        consensus.nShieldedSunsetHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedDirectSendPublicFlowDisableHeight = QTC_SHIELDED_DIRECT_SEND_PUBLIC_FLOW_DISABLE_HEIGHT;
        consensus.nShieldedV2SendZeroOutputExitActivationHeight =
            QTC_SHIELDED_V2_SEND_ZERO_OUTPUT_EXIT_ACTIVATION_HEIGHT;
        consensus.nShieldedRecoveryExitActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        // v0.32.0-v0.32.12: shielded unshield (z->t) velocity cap from the 125,000
        // sunset through block 134,999. The v0.32.11 minimum-cap floor still starts at
        // 132,000, and v0.32.12 ends the quota at 135,000 after the recovery window has
        // matured so remaining legacy exits are no longer rate-limited.
        consensus.nShieldedUnshieldVelocityActivationHeight = QTC_SHIELDED_SUNSET_HEIGHT;
        consensus.nShieldedUnshieldVelocityEndHeight = QTC_SHIELDED_UNSHIELD_VELOCITY_END_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCapHeight = QTC_V03211_HARDENING_HEIGHT;
        consensus.nShieldedUnshieldVelocityMinCap = QTC_SHIELDED_UNSHIELD_VELOCITY_MIN_CAP;
        consensus.nShieldedSettlementAnchorMaturity = 6;
        consensus.nMLDSADisableHeight = std::numeric_limits<int32_t>::max();
        consensus.nRuleChangeActivationThreshold = 108;
        consensus.nMinerConfirmationWindow = 144;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0;

        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0;

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart = MessageStartChars{0xe2, 0xb7, 0xda, 0x7a};
        nDefaultPort = 19444;
        nPruneAfterHeight = 100;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        constexpr uint32_t genesis_time{1773446400};
        constexpr uint32_t genesis_nonce{0};
        constexpr uint32_t genesis_bits{0x207fffff};
        constexpr int32_t genesis_version{1};
        constexpr uint64_t genesis_nonce64{0};

        genesis = CreateShieldedV2DevGenesisBlock(
            genesis_time,
            genesis_nonce,
            genesis_nonce64,
            genesis_bits,
            genesis_version,
            consensus.nInitialSubsidy,
            static_cast<uint16_t>(consensus.nMatMulDimension),
            uint256{});
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"309ae3de50712d4520cec19066979473a70de4a4b73d89b3327a07c845336e1f"});
        // AUDIT D1: validate the immutable MatMul-ASERT schedule at construction so
        // an invalid parameter set aborts node startup instead of failing closed
        // (hardest target) at some future block. ValidateMatMulAsertParams is a pure
        // function of the params; the height argument is log context only.
        assert(!consensus.fMatMulPOW ||
               ValidateMatMulAsertParams(consensus, consensus.nMatMulAsertHeight));

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.emplace_back("shieldedv2dev.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        checkpointData = {
            {
                {0, consensus.hashGenesisBlock},
            }
        };

        m_assumeutxo_data.clear();

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 196);
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "qtcv2";
    }
};

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main()
{
    return std::make_unique<const CMainParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet()
{
    return std::make_unique<const CTestNetParams>();
}

std::unique_ptr<const CChainParams> CChainParams::TestNet4()
{
    return std::make_unique<const CTestNet4Params>();
}

std::unique_ptr<const CChainParams> CChainParams::ShieldedV2Dev()
{
    return std::make_unique<const CShieldedV2DevParams>();
}

std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto testnet4_msg = CChainParams::TestNet4()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest({})->MessageStart();
    const auto shieldedv2dev_msg = CChainParams::ShieldedV2Dev()->MessageStart();
    const auto signet_msg = CChainParams::SigNet({})->MessageStart();

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, testnet4_msg)) {
        return ChainType::TESTNET4;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    } else if (std::ranges::equal(message, shieldedv2dev_msg)) {
        return ChainType::SHIELDEDV2DEV;
    } else if (std::ranges::equal(message, signet_msg)) {
        return ChainType::SIGNET;
    }
    return std::nullopt;
}
