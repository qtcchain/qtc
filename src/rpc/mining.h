// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_MINING_H
#define BITCOIN_RPC_MINING_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

/** Default max iterations to try in RPC generatetodescriptor, generatetoaddress, and generateblock. */
static const uint64_t DEFAULT_MAX_TRIES{100000000};

/**
 * Thresholds getblocktemplate applies before handing out work (security review M-11).
 * Derived from -miningminoutboundpeers, -miningminsyncedoutboundpeers,
 * -miningmaxpeersyncheightlag and -miningmaxheaderlag; on test chains the
 * connectivity and header-lag groups are only enforced when the operator set a
 * non-zero threshold, on mainnet they are always enforced.
 *
 * Only outbound peers count towards the peer floors. An outbound connection is
 * one this node chose to open (addrman selection or an operator addnode), so an
 * attacker cannot satisfy the floor simply by connecting to the miner; inbound
 * connections are unauthenticated and cheap to sybil. Operators running a fleet
 * should addnode each other on both sides, so every fleet peer is outbound
 * (manual) for both nodes and counts. The mainnet default floor is 2: the
 * built-in launch mesh has three public hosts, and a miner that is itself one
 * of them can only reach the other two.
 */
struct MiningTemplateReadinessPolicy {
    bool enforce_connectivity{true};
    int64_t min_outbound_peers{0};
    int64_t min_synced_outbound_peers{0};
    int64_t max_peer_sync_height_lag{0};
    bool enforce_header_lag{true};
    int64_t max_header_lag{0};
};

/** Node state sampled (under cs_main) for one readiness decision. */
struct MiningTemplateReadinessObservation {
    bool has_tip{false};
    int tip_height{-1};
    bool has_best_header{false};
    int best_header_height{-1};
    bool initial_block_download{false};
    size_t connected_peers{0};
    size_t outbound_peers{0};
    bool peerman_available{false};
    size_t synced_outbound_peers{0};
};

/** Where the height used for the synced-outbound rule came from. */
enum class MiningPeerHeightSource {
    NONE,            //!< no height signal is known for the peer
    SYNC_HEIGHT,     //!< net_processing best known block (the peer announced it to us)
    RELAYED,         //!< best header we announced to the peer, or block it fetched from us
    STARTING_HEIGHT, //!< version-handshake starting height (nothing exchanged yet)
};

/** Per-peer inputs to ClassifyMiningPeerSync; -1 means "unknown" for each. */
struct MiningPeerSyncSignals {
    int sync_height{-1};     //!< net_processing nSyncHeight (peer's announcements to us)
    int relayed_height{-1};  //!< max of best header we sent and best block the peer fetched
    int starting_height{-1}; //!< version-handshake starting height
};

/** Per-peer result of ClassifyMiningPeerSync. */
struct MiningPeerSyncStatus {
    MiningPeerHeightSource source{MiningPeerHeightSource::NONE};
    int sync_lag{-1};
    bool counts_as_synced{false};
};

/**
 * Decide whether one outbound peer counts towards -miningminsyncedoutboundpeers.
 *
 * Two strong signals are combined, and the higher one is used (issue #15):
 *
 *  - sync_height: the peer's best known block from net_processing (-1 until
 *    the peer has sent us a header, inv or block). A node never announces a
 *    block back to the peer it received it from, so for a well-connected
 *    miner that usually hears of a block first and relays it to its outbound
 *    peers, this height goes stale while the topology is perfectly healthy.
 *  - relayed_height: the best header we announced to the peer, or the best
 *    block it fetched from us. That is direct evidence the peer holds our tip
 *    (it has the data, even if it has not finished validating it).
 *
 * At genesis, and for any peer that connected while the network was idle,
 * neither signal exists even though the peer is exactly at our tip. In that
 * case only, fall back to the starting height the peer reported in its version
 * handshake. A known sync or relayed height always wins over the starting
 * height, so a peer that is really lagging cannot be talked back in via the
 * handshake. The peer counts as synced when the chosen height is within
 * max_peer_sync_height_lag blocks of the active tip.
 */
MiningPeerSyncStatus ClassifyMiningPeerSync(
    int active_tip_height,
    int64_t max_peer_sync_height_lag,
    const MiningPeerSyncSignals& signals);

/** Convenience overload without a relayed height (announcement-only view). */
inline MiningPeerSyncStatus ClassifyMiningPeerSync(
    int active_tip_height,
    int64_t max_peer_sync_height_lag,
    int sync_height,
    int starting_height)
{
    MiningPeerSyncSignals signals;
    signals.sync_height = sync_height;
    signals.starting_height = starting_height;
    return ClassifyMiningPeerSync(active_tip_height, max_peer_sync_height_lag, signals);
}

/** Why a template was refused: an RPCErrorCode and the message reported to the caller. */
struct MiningTemplateRefusal {
    int code;
    std::string message;
};

/**
 * Pure readiness decision for getblocktemplate. Returns std::nullopt when the
 * node may hand out a template, otherwise the refusal to report. Fails closed:
 * no tip, no peers, too few (synced) outbound peers, initial block download and
 * a validated tip too far behind the best known header all refuse.
 */
std::optional<MiningTemplateRefusal> CheckMiningTemplateReadiness(
    const MiningTemplateReadinessPolicy& policy,
    const MiningTemplateReadinessObservation& observation);

#endif // BITCOIN_RPC_MINING_H
