// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Security review M-11: getblocktemplate must refuse work when the node is not
// ready to produce a valid, propagatable block. These tests cover the pure
// readiness decision and the RPC wiring on a regtest node with no peers.

#include <interfaces/mining.h>
#include <rpc/mining.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

MiningTemplateReadinessPolicy MainnetLikePolicy()
{
    // Mirrors the mainnet defaults of -miningmin*/-miningmax* in rpc/mining.cpp.
    MiningTemplateReadinessPolicy policy;
    policy.enforce_connectivity = true;
    policy.min_outbound_peers = 3;
    policy.min_synced_outbound_peers = 2;
    policy.max_peer_sync_height_lag = 1;
    policy.enforce_header_lag = true;
    policy.max_header_lag = 3;
    return policy;
}

MiningTemplateReadinessObservation ReadyObservation()
{
    MiningTemplateReadinessObservation obs;
    obs.has_tip = true;
    obs.tip_height = 1000;
    obs.has_best_header = true;
    obs.best_header_height = 1000;
    obs.initial_block_download = false;
    obs.connected_peers = 8;
    obs.outbound_peers = 4;
    obs.peerman_available = true;
    obs.synced_outbound_peers = 3;
    return obs;
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

struct RpcFailure {
    int code;
    std::string message;
};

class ReadinessRpcSetup : public TestingSetup {
public:
    static TestOpts BuildOpts(std::vector<const char*> extra_args)
    {
        TestOpts opts;
        opts.extra_args = std::move(extra_args);
        opts.extra_args.push_back("-test=matmulstrict");
        return opts;
    }

    explicit ReadinessRpcSetup(std::vector<const char*> extra_args)
        : TestingSetup{ChainType::REGTEST, BuildOpts(std::move(extra_args))}
    {
        m_node.mining = interfaces::MakeMining(m_node);
    }

    static UniValue GBTParams()
    {
        UniValue rules{UniValue::VARR};
        rules.push_back("segwit");
        UniValue req{UniValue::VOBJ};
        req.pushKV("rules", std::move(rules));
        UniValue params{UniValue::VARR};
        params.push_back(std::move(req));
        return params;
    }

    //! Runs getblocktemplate; returns the JSON-RPC error if the call was refused.
    std::optional<RpcFailure> TryGetBlockTemplate(UniValue& result)
    {
        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = "getblocktemplate";
        request.params = GBTParams();
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        try {
            result = tableRPC.execute(request);
            return std::nullopt;
        } catch (const UniValue& obj_error) {
            return RpcFailure{
                obj_error.find_value("code").getInt<int>(),
                obj_error.find_value("message").get_str()};
        }
    }
};

//! Regtest defaults: every threshold is 0, so a lone regtest node may mine.
struct OpenRegtestSetup : public ReadinessRpcSetup {
    OpenRegtestSetup() : ReadinessRpcSetup({}) {}
};

//! Operator asked for a peer floor: the guard must bite even on regtest.
struct PeerFloorRegtestSetup : public ReadinessRpcSetup {
    PeerFloorRegtestSetup() : ReadinessRpcSetup({"-miningminoutboundpeers=1"}) {}
};

//! Synced-peer floor with an unreachable peer manager requirement.
struct SyncedPeerFloorRegtestSetup : public ReadinessRpcSetup {
    SyncedPeerFloorRegtestSetup()
        : ReadinessRpcSetup({"-miningminoutboundpeers=0", "-miningminsyncedoutboundpeers=1"}) {}
};

} // namespace

BOOST_AUTO_TEST_SUITE(mining_template_readiness_tests)

// ---------------------------------------------------------------------------
// Pure decision: passing cases
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ready_node_passes_mainnet_policy)
{
    const auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), ReadyObservation());
    BOOST_CHECK(!refusal.has_value());
}

BOOST_AUTO_TEST_CASE(header_lag_within_bound_passes)
{
    auto obs = ReadyObservation();
    obs.best_header_height = obs.tip_height + 3; // == max_header_lag
    BOOST_CHECK(!CheckMiningTemplateReadiness(MainnetLikePolicy(), obs).has_value());
}

BOOST_AUTO_TEST_CASE(test_chain_without_thresholds_skips_connectivity_and_lag)
{
    // Regtest defaults: nothing enforced; a lone node in IBD with a header gap still gets work.
    MiningTemplateReadinessPolicy policy;
    policy.enforce_connectivity = false;
    policy.enforce_header_lag = false;

    auto obs = ReadyObservation();
    obs.connected_peers = 0;
    obs.outbound_peers = 0;
    obs.synced_outbound_peers = 0;
    obs.peerman_available = false;
    obs.initial_block_download = true;
    obs.best_header_height = obs.tip_height + 50;

    BOOST_CHECK(!CheckMiningTemplateReadiness(policy, obs).has_value());
}

BOOST_AUTO_TEST_CASE(zero_thresholds_on_mainnet_still_require_peers_and_sync)
{
    // -miningminoutboundpeers=0 etc. disable the counts, not the basic "connected and synced" rule.
    auto policy = MainnetLikePolicy();
    policy.min_outbound_peers = 0;
    policy.min_synced_outbound_peers = 0;
    policy.max_header_lag = 0;

    auto obs = ReadyObservation();
    obs.outbound_peers = 0;
    obs.synced_outbound_peers = 0;
    obs.best_header_height = obs.tip_height + 50;
    BOOST_CHECK(!CheckMiningTemplateReadiness(policy, obs).has_value());

    obs.connected_peers = 0;
    auto refusal = CheckMiningTemplateReadiness(policy, obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_NOT_CONNECTED);

    obs.connected_peers = 1;
    obs.initial_block_download = true;
    refusal = CheckMiningTemplateReadiness(policy, obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_IN_INITIAL_DOWNLOAD);
}

// ---------------------------------------------------------------------------
// Pure decision: refusing cases (fail closed)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(no_tip_refuses_even_when_nothing_else_is_enforced)
{
    MiningTemplateReadinessPolicy policy;
    policy.enforce_connectivity = false;
    policy.enforce_header_lag = false;

    MiningTemplateReadinessObservation obs; // has_tip == false
    const auto refusal = CheckMiningTemplateReadiness(policy, obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_IN_INITIAL_DOWNLOAD);
    BOOST_CHECK(Contains(refusal->message, "no active tip"));
}

BOOST_AUTO_TEST_CASE(no_peers_refuses)
{
    auto obs = ReadyObservation();
    obs.connected_peers = 0;
    obs.outbound_peers = 0;
    const auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_NOT_CONNECTED);
    BOOST_CHECK(Contains(refusal->message, "not connected"));
}

BOOST_AUTO_TEST_CASE(too_few_outbound_peers_refuses)
{
    auto obs = ReadyObservation();
    obs.outbound_peers = 2; // < 3
    const auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_NOT_CONNECTED);
    BOOST_CHECK(Contains(refusal->message, "has 2 outbound peers, requires at least 3 for getblocktemplate"));
    BOOST_CHECK(Contains(refusal->message, "-miningminoutboundpeers=0"));
}

BOOST_AUTO_TEST_CASE(initial_block_download_refuses)
{
    auto obs = ReadyObservation();
    obs.initial_block_download = true;
    const auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_IN_INITIAL_DOWNLOAD);
    BOOST_CHECK(Contains(refusal->message, "initial sync"));
}

BOOST_AUTO_TEST_CASE(too_few_synced_outbound_peers_refuses)
{
    auto obs = ReadyObservation();
    obs.synced_outbound_peers = 1; // < 2
    const auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_NOT_CONNECTED);
    BOOST_CHECK(Contains(refusal->message, "has 1 synced outbound peers, requires at least 2 within 1 blocks of active tip"));
    BOOST_CHECK(Contains(refusal->message, "-miningminsyncedoutboundpeers=0"));
}

BOOST_AUTO_TEST_CASE(missing_peer_manager_refuses_when_sync_floor_set)
{
    auto obs = ReadyObservation();
    obs.peerman_available = false;
    const auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_INTERNAL_ERROR);
    BOOST_CHECK(Contains(refusal->message, "Peer manager unavailable"));
}

BOOST_AUTO_TEST_CASE(validated_tip_behind_best_header_refuses)
{
    auto obs = ReadyObservation();
    obs.best_header_height = obs.tip_height + 4; // > 3
    const auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_IN_INITIAL_DOWNLOAD);
    BOOST_CHECK(Contains(refusal->message, "validated tip is 4 blocks behind best header (4 > 3)"));
    BOOST_CHECK(Contains(refusal->message, "-miningmaxheaderlag=0"));
}

BOOST_AUTO_TEST_CASE(header_lag_enforced_independently_of_connectivity)
{
    // Test chain with only -miningmaxheaderlag set: connectivity is skipped, lag is not.
    MiningTemplateReadinessPolicy policy;
    policy.enforce_connectivity = false;
    policy.enforce_header_lag = true;
    policy.max_header_lag = 2;

    auto obs = ReadyObservation();
    obs.connected_peers = 0;
    obs.outbound_peers = 0;
    obs.best_header_height = obs.tip_height + 3;
    const auto refusal = CheckMiningTemplateReadiness(policy, obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK_EQUAL(refusal->code, RPC_CLIENT_IN_INITIAL_DOWNLOAD);
    BOOST_CHECK(Contains(refusal->message, "3 blocks behind best header (3 > 2)"));
}

BOOST_AUTO_TEST_CASE(refusals_are_ordered_connectivity_before_sync_before_lag)
{
    // A node that is wrong in every way reports the first, most actionable reason.
    auto obs = ReadyObservation();
    obs.connected_peers = 0;
    obs.outbound_peers = 0;
    obs.initial_block_download = true;
    obs.synced_outbound_peers = 0;
    obs.best_header_height = obs.tip_height + 100;

    auto refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK(Contains(refusal->message, "not connected"));

    obs.connected_peers = 1;
    obs.outbound_peers = 1;
    refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK(Contains(refusal->message, "outbound peers, requires at least"));

    obs.outbound_peers = 3;
    refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK(Contains(refusal->message, "initial sync"));

    obs.initial_block_download = false;
    refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK(Contains(refusal->message, "synced outbound peers"));

    obs.synced_outbound_peers = 2;
    refusal = CheckMiningTemplateReadiness(MainnetLikePolicy(), obs);
    BOOST_REQUIRE(refusal.has_value());
    BOOST_CHECK(Contains(refusal->message, "behind best header"));

    obs.best_header_height = obs.tip_height;
    BOOST_CHECK(!CheckMiningTemplateReadiness(MainnetLikePolicy(), obs).has_value());
}

// ---------------------------------------------------------------------------
// RPC wiring: getblocktemplate on a peerless regtest node
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(getblocktemplate_serves_lone_regtest_node_by_default, OpenRegtestSetup)
{
    UniValue result;
    const auto failure = TryGetBlockTemplate(result);
    const std::string why = failure ? failure->message : std::string{};
    BOOST_REQUIRE_MESSAGE(!failure.has_value(), why);
    BOOST_CHECK(result.isObject());
    BOOST_CHECK(!result.find_value("previousblockhash").isNull());
}

BOOST_FIXTURE_TEST_CASE(getblocktemplate_refuses_when_outbound_peer_floor_unmet, PeerFloorRegtestSetup)
{
    UniValue result;
    const auto failure = TryGetBlockTemplate(result);
    BOOST_REQUIRE(failure.has_value());
    BOOST_CHECK_EQUAL(failure->code, RPC_CLIENT_NOT_CONNECTED);
    BOOST_CHECK_MESSAGE(Contains(failure->message, "not connected"), failure->message);
}

BOOST_FIXTURE_TEST_CASE(getblocktemplate_refuses_when_synced_peer_floor_unmet, SyncedPeerFloorRegtestSetup)
{
    UniValue result;
    const auto failure = TryGetBlockTemplate(result);
    BOOST_REQUIRE(failure.has_value());
    // With no peers at all the "not connected" rule fires first; either way the node fails closed.
    BOOST_CHECK_EQUAL(failure->code, RPC_CLIENT_NOT_CONNECTED);
}

BOOST_AUTO_TEST_SUITE_END()
