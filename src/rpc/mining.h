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
