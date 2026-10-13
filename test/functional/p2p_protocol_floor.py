#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the QTC forced-upgrade protocol floors (v0.2.4 at 4100, v0.2.5 at 6000 on mainnet).

Once the chain tip reaches the floor height, peers below the v0.2.4 protocol
version are disconnected: connections that were already open are closed from
SendMessages, and new ones are refused at the version handshake. Below the
floor height old-protocol peers are tolerated. The floor height and version
are overridden here with the test-only -v024floorheight / -minv024version.
"""

from test_framework.p2p import P2PInterface, P2P_VERSION
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

OLD_VERSION = 800002
BLOCKS_TO_FLOOR = 5  # the floor is placed this many blocks above the chain the framework starts with


class OldProtocolPeer(P2PInterface):
    def __init__(self, version):
        super().__init__()
        self.version = version

    def peer_connect_send_version(self, services):
        super().peer_connect_send_version(services)
        self.on_connection_send_msg.nVersion = self.version


class ProtocolFloorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        # The framework starts from a cached chain, so place the floor relative to its height.
        floor_height = node.getblockcount() + BLOCKS_TO_FLOOR
        self.restart_node(0, extra_args=[f"-v024floorheight={floor_height}", f"-minv024version={P2P_VERSION}"])

        self.log.info("Below the floor height an old-protocol peer connects and stays connected")
        old = node.add_p2p_connection(OldProtocolPeer(OLD_VERSION))
        self.generate(node, BLOCKS_TO_FLOOR - 2)
        old.sync_with_ping()
        assert old.is_connected
        assert_equal(node.getpeerinfo()[0]["version"], OLD_VERSION)

        self.log.info("Reaching the floor height closes the already-open old-protocol connection")
        self.generate(node, 2)
        assert_equal(node.getblockcount(), floor_height)
        old.wait_for_disconnect()

        self.log.info("A new old-protocol peer is refused at the handshake")
        refused = node.add_p2p_connection(OldProtocolPeer(OLD_VERSION), wait_for_verack=False)
        refused.wait_for_disconnect()
        assert_equal(len(node.getpeerinfo()), 0)

        self.log.info("A current-protocol peer connects normally")
        current = node.add_p2p_connection(P2PInterface())
        current.sync_with_ping()
        assert current.is_connected
        assert_equal(node.getpeerinfo()[0]["version"], P2P_VERSION)

        self.log.info("The v0.2.5 floor stacks on top: a v0.2.4-protocol peer is cut off at its own height")
        node.disconnect_p2ps()
        floor_height = node.getblockcount() + BLOCKS_TO_FLOOR
        self.restart_node(0, extra_args=[f"-v025floorheight={floor_height}", f"-minv025version={P2P_VERSION}"])
        v024_peer = node.add_p2p_connection(OldProtocolPeer(P2P_VERSION - 1))
        self.generate(node, BLOCKS_TO_FLOOR - 1)
        v024_peer.sync_with_ping()
        assert v024_peer.is_connected
        self.generate(node, 1)
        assert_equal(node.getblockcount(), floor_height)
        v024_peer.wait_for_disconnect()
        refused = node.add_p2p_connection(OldProtocolPeer(P2P_VERSION - 1), wait_for_verack=False)
        refused.wait_for_disconnect()
        current = node.add_p2p_connection(P2PInterface())
        current.sync_with_ping()
        assert_equal(node.getpeerinfo()[0]["version"], P2P_VERSION)


if __name__ == "__main__":
    ProtocolFloorTest(__file__).main()
