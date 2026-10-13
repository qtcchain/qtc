#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Wallets keep tracking a chain whose block timestamps trail the wall clock (#31).

After a stall, templates are clamped to MTP+12h, so a live chain can trail real time by days.
A wallet created on such a chain used to get a wall-clock birth time hours ahead of every block
time, and blockConnected() then skipped each new block until a manual rescanblockchain.

Node 0 mines with its clock ten days back; node 1 runs on the real clock and holds the wallets.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal

LAG = 10 * 24 * 3600


class WalletBirthtimeLaggingChainTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 2
        # The cached chain's last block carries the wall-clock time, which would be the chain's
        # maximum block time forever; start from genesis so every block is mined on the old clock.
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        miner, node = self.nodes
        old = int(time.time()) - LAG
        miner.setmocktime(old)
        miner.createwallet("m")
        self.generatetoaddress(miner, 10, miner.get_wallet_rpc("m").getnewaddress())
        tip_time = node.getblockheader(node.getbestblockhash())["time"]
        assert_greater_than_or_equal(old + 3600, tip_time)
        self.log.info(f"chain trails the real clock by {(int(time.time()) - tip_time) // 86400} days")

        self.log.info("A wallet created while the chain trails gets a chain-clamped birth time")
        node.createwallet("w")
        w = node.get_wallet_rpc("w")
        birth = w.getwalletinfo()["birthtime"]
        assert_greater_than_or_equal(birth, old)
        assert_greater_than_or_equal(old + 3600, birth)

        self.log.info("Blocks paying it are tracked without a rescan")
        addr = w.getnewaddress()
        self.generatetoaddress(miner, 3, addr)
        assert_equal(w.getwalletinfo()["txcount"], 3)
        assert_equal(w.getbalances()["mine"]["immature"], 150)

        self.log.info("Reloading the wallet keeps tracking")
        node.unloadwallet("w")
        node.loadwallet("w")
        w = node.get_wallet_rpc("w")
        self.generatetoaddress(miner, 2, addr)
        assert_equal(w.getwalletinfo()["txcount"], 5)

        self.log.info("A wall-clock birth time (explicit import timestamp) no longer skips trailing blocks")
        node.createwallet(wallet_name="watch", blank=True, disable_private_keys=True)
        watch = node.get_wallet_rpc("watch")
        pubkey = w.exportpqkey(addr, "ml-dsa-44")["pubkey"]
        desc = node.getdescriptorinfo(f"mr({pubkey})")["descriptor"]
        assert_equal(watch.importdescriptors([{"desc": desc, "timestamp": int(time.time())}])[0]["success"], True)
        assert_greater_than_or_equal(watch.getwalletinfo()["birthtime"], tip_time + 3600)
        self.generatetoaddress(miner, 2, addr)
        assert_equal(watch.getwalletinfo()["txcount"], 2)
        assert_equal(watch.getbalances()["mine"]["immature"], 100)


if __name__ == "__main__":
    WalletBirthtimeLaggingChainTest(__file__).main()
