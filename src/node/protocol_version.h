// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_PROTOCOL_VERSION_H
#define BITCOIN_NODE_PROTOCOL_VERSION_H

/**
 * network protocol versioning
 */

// QTC v0.2.4: 800003 (forced-upgrade floor at mainnet height 4100, see V024_PROTOCOL_FLOOR_HEIGHT).
// 800002 = v0.2.0 to v0.2.3; 800001 = v0.1.0 to v0.1.3 (v1 chain). The protocol version advances only when a
// release changes P2P behaviour; the peer floor below follows doc/upgrade-policy.md.
static const int PROTOCOL_VERSION = 800003;

//! initial proto version, to be increased after version/verack negotiation
static const int INIT_PROTO_VERSION = 209;

//! disconnect from peers older than this proto version.
//! QTC v2 (mainnet reset, v0.2.0): the v2 network has its own message start, so no 800001 peer can ever
//! be a v2 peer; the floor starts at the current protocol. Future floors follow doc/upgrade-policy.md §3.
static const int MIN_PEER_PROTO_VERSION = 800002;

//! Minimum protocol version required for SMILE v2 shielded transactions.
//! Peers below this version are disconnected after SMILE_V2_ENFORCEMENT_HEIGHT
//! to prevent chain splits from nodes that reject valid SMILE v2 transactions.
static const int MIN_SMILE_V2_PROTOCOL_VERSION = 800001;

//! Chain height at which SMILE v2 protocol version enforcement activates.
//! Before this height, old-version peers are tolerated for IBD compatibility.
static const int SMILE_V2_ENFORCEMENT_HEIGHT = 51000;

//! QTC v0.2.4 forced-upgrade floor (owner decision, 12 October 2026; recorded exception to
//! doc/upgrade-policy.md section 3). Once the chain tip has reached V024_PROTOCOL_FLOOR_HEIGHT, peers
//! below this protocol version are disconnected: at the version handshake, and from SendMessages for
//! connections that were already open. Block validity is unchanged; this is a network rule only.
static const int MIN_V024_PROTOCOL_VERSION = 800003;

//! Mainnet height of the v0.2.4 floor: 4100, about 27 October 2026 at the October 2026 block spacing and
//! more than 2 000 blocks after the v0.2.4 tag (tip 1898 on 12 October). The same height applies on test
//! networks. Test-only override: -v024floorheight.
static const int V024_PROTOCOL_FLOOR_HEIGHT = 4100;

//! BIP 0031, pong message, is enabled for all versions AFTER this one
static const int BIP0031_VERSION = 60000;

//! "sendheaders" command and announcing blocks with headers starts with this version
static const int SENDHEADERS_VERSION = 70012;

//! "feefilter" tells peers to filter invs to you by fee starts with this version
static const int FEEFILTER_VERSION = 70013;

//! short-id-based block download starts with this version
static const int SHORT_IDS_BLOCKS_VERSION = 70014;

//! not banning for invalid compact blocks starts with this version
static const int INVALID_CB_NO_BAN_VERSION = 70015;

//! "wtxidrelay" command for wtxid-based relay starts with this version
static const int WTXID_RELAY_VERSION = 70016;

#endif // BITCOIN_NODE_PROTOCOL_VERSION_H
