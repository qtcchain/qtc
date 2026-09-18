#!/usr/bin/env python3
# Copyright (c) 2015-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test behavior of -maxuploadtarget.

* Verify that getdata requests for old blocks (>1week) are dropped
if uploadtarget has been reached.
* Verify that getdata requests for recent blocks are respected even
if uploadtarget has been reached.
* Verify that mempool requests lead to a disconnect if uploadtarget has been reached.
* Verify that the upload counters are reset after 24 hours.
"""
from collections import defaultdict
import time

from test_framework.messages import (
    CInv,
    MSG_BLOCK,
    msg_getdata,
    msg_mempool,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)
from test_framework.wallet import MiniWallet, MiniWalletMode


# QTC: MAX_BLOCK_SERIALIZED_SIZE is 24 MB, so the historical block serving
# buffer is 144 * 24 MB; the target must exceed that or the node never serves
# historical blocks at all.
MAX_BLOCK_SERIALIZED_SIZE = 24_000_000
UPLOAD_TARGET_MB = 4000


def mine_large_block(test_framework, mini_wallet, node):
    # QTC: 14 padded ~66 kvB P2MR self-transfers approach the 1 MB block size
    # (the generic util helper edits outputs after signing, which breaks P2MR)
    for _ in range(14):
        mini_wallet.send_self_transfer(from_node=node, target_vsize=66000)
    test_framework.generate(node, 1)


class TestP2PConn(P2PInterface):
    def __init__(self):
        super().__init__()
        self.block_receive_map = defaultdict(int)

    def on_inv(self, message):
        pass

    def on_block(self, message):
        message.block.calc_sha256()
        self.block_receive_map[message.block.sha256] += 1

class MaxUploadTest(BitcoinTestFramework):

    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)  # QTC: ADDRESS_P2MR MiniWallet signs via the node wallet

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            f"-maxuploadtarget={UPLOAD_TARGET_MB}M",
            "-datacarriersize=100000",
            "-dustrelayfee=0",  # QTC: MiniWallet padding outputs are below the P2MR dust threshold
        ]]
        self.supports_cli = False

    def assert_uploadtarget_state(self, *, target_reached, serve_historical_blocks):
        """Verify the node's current upload target state via the `getnettotals` RPC call."""
        uploadtarget = self.nodes[0].getnettotals()["uploadtarget"]
        assert_equal(uploadtarget["target_reached"], target_reached)
        assert_equal(uploadtarget["serve_historical_blocks"], serve_historical_blocks)

    def run_test(self):
        # Initially, neither historical blocks serving limit nor total limit are reached
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=True)

        # Before we connect anything, we first set the time on the node
        # to be in the past, otherwise things break because the CNode
        # time counters can't be reset backward after initialization
        old_time = int(time.time() - 2*60*60*24*7)
        self.nodes[0].setmocktime(old_time)

        # Generate some old blocks
        self.wallet = MiniWallet(self.nodes[0], mode=MiniWalletMode.ADDRESS_P2MR)
        self.generate(self.wallet, 130)

        # p2p_conns[0] will only request old blocks
        # p2p_conns[1] will only request new blocks
        # p2p_conns[2] will test resetting the counters
        p2p_conns = []

        for _ in range(3):
            # Don't use v2transport in this test (too slow with the unoptimized python ChaCha20 implementation)
            p2p_conns.append(self.nodes[0].add_p2p_connection(TestP2PConn(), supports_v2_p2p=False))

        # Now mine a big block
        mine_large_block(self, self.wallet, self.nodes[0])

        # Store the hash; we'll request this later
        big_old_block = int(self.nodes[0].getbestblockhash(), 16)

        # Advance to two days ago
        self.nodes[0].setmocktime(int(time.time()) - 2*60*60*24)

        # Mine one more block, so that the prior block looks old
        mine_large_block(self, self.wallet, self.nodes[0])

        # We'll be requesting this new block too
        big_new_block = self.nodes[0].getbestblockhash()
        big_new_block = int(big_new_block, 16)

        # p2p_conns[0] will test what happens if we just keep requesting the
        # the same big old block too many times (expect: disconnect)

        getdata_request = msg_getdata()
        getdata_request.inv.append(CInv(MSG_BLOCK, big_old_block))

        # QTC: the block is smaller on the wire than getblock 'size' reports,
        # so measure the bytes actually sent for one download
        bytes_sent_before = self.nodes[0].getnettotals()["totalbytessent"]
        p2p_conns[0].send_and_ping(getdata_request)
        assert_equal(p2p_conns[0].block_receive_map[big_old_block], 1)
        old_block_size = self.nodes[0].getnettotals()["totalbytessent"] - bytes_sent_before

        max_bytes_per_day = UPLOAD_TARGET_MB * 1024 *1024
        daily_buffer = 144 * MAX_BLOCK_SERIALIZED_SIZE
        max_bytes_available = max_bytes_per_day - daily_buffer
        success_count = max_bytes_available // old_block_size

        # 3456MB will be reserved for relaying new blocks, so expect this to
        # succeed for ~700 tries.
        for i in range(1, success_count):
            p2p_conns[0].send_and_ping(getdata_request)
            assert_equal(p2p_conns[0].block_receive_map[big_old_block], i+1)

        assert_equal(len(self.nodes[0].getpeerinfo()), 3)
        # At most a couple more tries should succeed (depending on how long
        # the test has been running so far).
        with self.nodes[0].assert_debug_log(expected_msgs=["historical block serving limit reached, disconnecting peer=0"]):
            for _ in range(3):
                p2p_conns[0].send_message(getdata_request)
            p2p_conns[0].wait_for_disconnect()
        assert_equal(len(self.nodes[0].getpeerinfo()), 2)
        self.log.info("Peer 0 disconnected after downloading old block too many times")

        # Historical blocks serving limit is reached by now, but total limit still isn't
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=False)

        # Requesting the current block on p2p_conns[1] should succeed indefinitely,
        # even when over the max upload target.
        # QTC: download enough copies to exhaust what is left of the (larger) target
        getdata_request.inv = [CInv(MSG_BLOCK, big_new_block)]
        new_block_count = self.nodes[0].getnettotals()["uploadtarget"]["bytes_left_in_cycle"] // old_block_size + 2
        for i in range(new_block_count):
            p2p_conns[1].send_and_ping(getdata_request)
            assert_equal(p2p_conns[1].block_receive_map[big_new_block], i+1)

        # Both historical blocks serving limit and total limit are reached
        self.assert_uploadtarget_state(target_reached=True, serve_historical_blocks=False)

        self.log.info("Peer 1 able to repeatedly download new block")

        # But if p2p_conns[1] tries for an old block, it gets disconnected too.
        getdata_request.inv = [CInv(MSG_BLOCK, big_old_block)]
        with self.nodes[0].assert_debug_log(expected_msgs=["historical block serving limit reached, disconnecting peer=1"]):
            p2p_conns[1].send_message(getdata_request)
            p2p_conns[1].wait_for_disconnect()
        assert_equal(len(self.nodes[0].getpeerinfo()), 1)

        self.log.info("Peer 1 disconnected after trying to download old block")

        self.log.info("Advancing system time on node to clear counters...")

        # If we advance the time by 24 hours, then the counters should reset,
        # and p2p_conns[2] should be able to retrieve the old block.
        self.nodes[0].setmocktime(int(time.time()))
        p2p_conns[2].sync_with_ping()
        p2p_conns[2].send_and_ping(getdata_request)
        assert_equal(p2p_conns[2].block_receive_map[big_old_block], 1)
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=True)

        self.log.info("Peer 2 able to download old block")

        self.nodes[0].disconnect_p2ps()

        self.log.info("Restarting node 0 with download permission, bloom filter support and 1MB maxuploadtarget")
        self.restart_node(0, ["-whitelist=download@127.0.0.1", "-peerbloomfilters", "-maxuploadtarget=1"])
        # Total limit isn't reached after restart, but 1 MB is too small to serve historical blocks
        self.assert_uploadtarget_state(target_reached=False, serve_historical_blocks=False)

        # Reconnect to self.nodes[0]
        peer = self.nodes[0].add_p2p_connection(TestP2PConn(), supports_v2_p2p=False)

        # Sending mempool message shouldn't disconnect peer, as total limit isn't reached yet
        peer.send_and_ping(msg_mempool())

        #retrieve 20 blocks which should be enough to break the 1MB limit
        getdata_request.inv = [CInv(MSG_BLOCK, big_new_block)]
        for i in range(20):
            peer.send_and_ping(getdata_request)
            assert_equal(peer.block_receive_map[big_new_block], i+1)

        # Total limit is exceeded
        self.assert_uploadtarget_state(target_reached=True, serve_historical_blocks=False)

        getdata_request.inv = [CInv(MSG_BLOCK, big_old_block)]
        peer.send_and_ping(getdata_request)

        self.log.info("Peer still connected after trying to download old block (download permission)")
        peer_info = self.nodes[0].getpeerinfo()
        assert_equal(len(peer_info), 1)  # node is still connected
        assert_equal(peer_info[0]['permissions'], ['download'])

        self.log.info("Peer gets disconnected for a mempool request after limit is reached")
        with self.nodes[0].assert_debug_log(expected_msgs=["mempool request with bandwidth limit reached, disconnecting peer=0"]):
            peer.send_message(msg_mempool())
            peer.wait_for_disconnect()

        self.log.info("Test passing an unparsable value to -maxuploadtarget throws an error")
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(extra_args=["-maxuploadtarget=abc"], expected_msg="Error: Unable to parse -maxuploadtarget: 'abc'")

if __name__ == '__main__':
    MaxUploadTest(__file__).main()
