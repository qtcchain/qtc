#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test fastprune mode."""
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal
)
from test_framework.wallet import MiniWallet, MiniWalletMode


class FeatureFastpruneTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)  # QTC: ADDRESS_P2MR MiniWallet signs via the node wallet

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-fastprune"]]

    def run_test(self):
        self.log.info("ensure that large blocks don't crash or freeze in -fastprune")
        wallet = MiniWallet(self.nodes[0], mode=MiniWalletMode.ADDRESS_P2MR)
        self.generate(wallet, COINBASE_MATURITY + 1)  # QTC: the cached chain has no P2MR coins
        # QTC: P2MR rejects an annex at consensus, so grow the tx past the
        # 64 KiB fastprune block file size with padding outputs instead
        tx = wallet.create_self_transfer(target_vsize=70000)['tx']
        height = self.nodes[0].getblockcount()
        self.generateblock(self.nodes[0], output="raw(55)", transactions=[tx.serialize().hex()])
        assert_equal(self.nodes[0].getblockcount(), height + 1)


if __name__ == '__main__':
    FeatureFastpruneTest(__file__).main()
