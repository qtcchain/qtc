#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the QTC v0.2.4 forced-upgrade protocol floor.

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
FLOOR_HEIGHT = 5


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
        self.extra_args = [[f"-v024floorheight={FLOOR_HEIGHT}", f"-minv024version={P2P_VERSION}"]]

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Below the floor height an old-protocol peer connects and stays connected")
        old = node.add_p2p_connection(OldProtocolPeer(OLD_VERSION))
        self.generate(node, FLOOR_HEIGHT - 2)
        old.sync_with_ping()
        assert old.is_connected
        assert_equal(node.getpeerinfo()[0]["version"], OLD_VERSION)

        self.log.info("Reaching the floor height closes the already-open old-protocol connection")
        self.generate(node, 2)
        assert_equal(node.getblockcount(), FLOOR_HEIGHT)
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


if __name__ == "__main__":
    ProtocolFloorTest(__file__).main()
