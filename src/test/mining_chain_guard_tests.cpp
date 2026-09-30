// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/mining_guard.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_AUTO_TEST_SUITE(mining_chain_guard_tests)

BOOST_AUTO_TEST_CASE(disabled_guard_does_not_pause_mining)
{
    node::MiningChainGuardOptions options;
    options.enabled = false;

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/100,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        std::vector<int>{90, 95, 100},
        options);

    BOOST_CHECK(status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "disabled");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "continue");
}

BOOST_AUTO_TEST_CASE(initial_block_download_keeps_mining_with_recovery_warning)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/100,
        /*initial_block_download=*/true,
        /*network_active=*/true,
        std::vector<int>{100, 100},
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "initial_block_download");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "mine_current_tip_and_catch_up");
}

BOOST_AUTO_TEST_CASE(insufficient_peer_consensus_keeps_mining_with_recovery_warning)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;
    options.min_peer_count = 2;

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/100,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        std::vector<int>{100},
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "insufficient_peer_consensus");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "add_outbound_peers");
}

BOOST_AUTO_TEST_CASE(default_guard_requires_three_peers)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;

    BOOST_CHECK_EQUAL(options.min_peer_count, 3);
    BOOST_CHECK_EQUAL(options.max_median_tip_gap, 2);
    BOOST_CHECK_EQUAL(options.stale_peer_seconds, 120);

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/100,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        std::vector<int>{100, 100},
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "insufficient_peer_consensus");
    BOOST_CHECK_EQUAL(status.min_peer_count, 3);
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "add_outbound_peers");
}

BOOST_AUTO_TEST_CASE(local_tip_ahead_of_peer_median_keeps_mining_to_propagate_tip)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;
    options.max_median_tip_gap = 6;

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/125,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        std::vector<int>{100, 101, 102, 103, 104},
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "local_tip_ahead_of_peer_median");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "propagate_tip");
}

BOOST_AUTO_TEST_CASE(local_tip_behind_peer_median_keeps_mining_with_recovery_warning)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;
    options.max_median_tip_gap = 6;

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/100,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        std::vector<int>{108, 109, 110},
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "local_tip_behind_peer_median");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "mine_current_tip_and_catch_up");
}

BOOST_AUTO_TEST_CASE(near_tip_peer_quorum_keeps_mining_with_recovery_warning)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;
    options.max_median_tip_gap = 4;

    BOOST_CHECK_EQUAL(options.min_near_tip_peers, 2);
    BOOST_CHECK_EQUAL(options.near_tip_window, 2);

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/100,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        std::vector<int>{100, 103, 103},
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "insufficient_near_tip_peers");
    BOOST_CHECK_EQUAL(status.near_tip_peers, 1);
    BOOST_CHECK_EQUAL(status.min_near_tip_peers, 2);
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "add_outbound_peers");
}

BOOST_AUTO_TEST_CASE(median_majority_close_to_tip_keeps_mining_enabled)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;
    options.max_median_tip_gap = 6;

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/120,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        std::vector<int>{118, 120, 120, 121, 110},
        options);

    BOOST_CHECK(status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "healthy");
    BOOST_CHECK_EQUAL(status.median_peer_tip, 120);
    BOOST_CHECK_EQUAL(status.near_tip_peers, 4);
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "continue");
}

BOOST_AUTO_TEST_CASE(stale_lagging_peers_are_filtered_out_before_median_check)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;
    options.max_median_tip_gap = 6;
    options.stale_peer_seconds = 30;

    const std::vector<node::MiningChainGuardPeerSample> peers{
        {120, 995, 995},
        {120, 995, 995},
        {120, 995, 995},
        {110, 900, 900},
        {110, 900, 900},
        {110, 900, 900},
        {110, 900, 900},
        {110, 900, 900},
    };

    const auto filtered = node::FilterMiningChainGuardPeerHeights(
        /*local_tip_height=*/120,
        /*now=*/1000,
        peers,
        options);

    BOOST_CHECK_EQUAL(filtered.size(), 3U);
    BOOST_CHECK_EQUAL(filtered[0], 120);
    BOOST_CHECK_EQUAL(filtered[1], 120);
    BOOST_CHECK_EQUAL(filtered[2], 120);

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/120,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        filtered,
        options);

    BOOST_CHECK(status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "healthy");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "continue");
}

BOOST_AUTO_TEST_CASE(recently_active_lagging_peers_still_count_for_fork_safety)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;
    options.max_median_tip_gap = 6;
    options.stale_peer_seconds = 30;

    const std::vector<node::MiningChainGuardPeerSample> peers{
        {120, 995, 995},
        {120, 995, 995},
        {120, 995, 995},
        {110, 995, 995},
        {110, 995, 995},
        {110, 995, 995},
        {110, 995, 995},
        {110, 995, 995},
    };

    const auto filtered = node::FilterMiningChainGuardPeerHeights(
        /*local_tip_height=*/120,
        /*now=*/1000,
        peers,
        options);

    BOOST_CHECK_EQUAL(filtered.size(), peers.size());

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/120,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        filtered,
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "local_tip_ahead_of_peer_median");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "propagate_tip");
}

BOOST_AUTO_TEST_CASE(peer_height_prefers_sync_then_common_then_starting_height)
{
    BOOST_CHECK_EQUAL(node::ResolveMiningChainGuardPeerHeight(120, 110, 5), 120);
    BOOST_CHECK_EQUAL(node::ResolveMiningChainGuardPeerHeight(-1, 110, 5), 110);
    BOOST_CHECK_EQUAL(node::ResolveMiningChainGuardPeerHeight(-1, -1, 5), 5);
    BOOST_CHECK_EQUAL(node::ResolveMiningChainGuardPeerHeight(-1, -1, 0), 0);
    BOOST_CHECK_EQUAL(node::ResolveMiningChainGuardPeerHeight(-1, -1, -1), -1);
}

BOOST_AUTO_TEST_CASE(genesis_peers_with_only_starting_height_form_consensus)
{
    // Mainnet launch, 2026-09-30: local tip is genesis and three outbound
    // peers are connected, but no header has been exchanged yet, so
    // nSyncHeight and nCommonHeight are -1 for all of them. Their handshake
    // starting height (0) is the tip, and must read as consensus rather than
    // "insufficient_peer_consensus" (which made the supervisor churn peers).
    node::MiningChainGuardOptions options;
    options.enabled = true;
    BOOST_CHECK_EQUAL(options.min_peer_count, 3);

    std::vector<node::MiningChainGuardPeerSample> peers;
    for (int i = 0; i < 3; ++i) {
        node::MiningChainGuardPeerSample sample;
        sample.height = node::ResolveMiningChainGuardPeerHeight(
            /*sync_height=*/-1, /*common_height=*/-1, /*starting_height=*/0);
        peers.push_back(sample); // no block time or announcement yet either
    }

    const auto filtered = node::FilterMiningChainGuardPeerHeights(
        /*local_tip_height=*/0,
        /*now=*/1000,
        peers,
        options);
    BOOST_CHECK_EQUAL(filtered.size(), 3U);

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/0,
        /*initial_block_download=*/false,
        /*network_active=*/true,
        filtered,
        options);

    BOOST_CHECK(status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "healthy");
    BOOST_CHECK_EQUAL(status.peer_count, 3);
    BOOST_CHECK_EQUAL(status.median_peer_tip, 0);
    BOOST_CHECK_EQUAL(status.near_tip_peers, 3);
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "continue");
}

BOOST_AUTO_TEST_CASE(genesis_peers_without_any_height_signal_still_report_insufficient_consensus)
{
    // A peer that has not even completed the version handshake contributes nothing.
    node::MiningChainGuardOptions options;
    options.enabled = true;

    std::vector<node::MiningChainGuardPeerSample> peers(3);
    for (auto& sample : peers) {
        sample.height = node::ResolveMiningChainGuardPeerHeight(-1, -1, -1);
    }

    const auto filtered = node::FilterMiningChainGuardPeerHeights(0, 1000, peers, options);
    BOOST_CHECK(filtered.empty());

    const auto status = node::EvaluateMiningChainGuard(0, false, true, filtered, options);
    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "insufficient_peer_consensus");
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "add_outbound_peers");
}

BOOST_AUTO_TEST_CASE(network_inactive_keeps_mining_with_recovery_warning)
{
    node::MiningChainGuardOptions options;
    options.enabled = true;

    const auto status = node::EvaluateMiningChainGuard(
        /*local_tip_height=*/100,
        /*initial_block_download=*/false,
        /*network_active=*/false,
        std::vector<int>{100, 100, 100},
        options);

    BOOST_CHECK(!status.healthy);
    BOOST_CHECK_EQUAL(status.reason, "network_inactive");
    BOOST_CHECK(!node::ShouldPauseMiningByChainGuard(status));
    BOOST_CHECK_EQUAL(node::GetMiningChainGuardRecommendedAction(status), "mine_current_tip_and_enable_network");
}

BOOST_AUTO_TEST_SUITE_END()
