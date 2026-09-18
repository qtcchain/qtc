#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test MiniWallet."""
import random
import string

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)
from test_framework.wallet import (
    MiniWallet,
    MiniWalletMode,
    P2MR_SELF_TRANSFER_VSIZE,
)


class FeatureFrameworkMiniWalletTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)  # QTC: ADDRESS_P2MR MiniWallet signs via the node wallet

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.num_nodes = 1

    def test_tx_padding(self):
        """Verify that MiniWallet's transaction padding (`target_vsize` parameter)
           works accurately with all modes."""
        for mode_name, wallet in self.wallets:
            self.log.info(f"Test tx padding with MiniWallet mode {mode_name}...")
            utxo = wallet.get_utxo(mark_as_spent=False)
            for target_vsize in [250, 500, 1250, 2500, 5000, 12500, 25000, 50000, 1000000,
                                 248, 501, 1085, 3343, 5805, 12289, 25509, 55855,  999998]:
                if mode_name == "ADDRESS_P2MR" and target_vsize < P2MR_SELF_TRANSFER_VSIZE:
                    continue  # QTC: the PQ witness alone exceeds this target
                tx = wallet.create_self_transfer(utxo_to_spend=utxo, target_vsize=target_vsize)
                assert_equal(tx['tx'].get_vsize(), target_vsize)
                if mode_name == "ADDRESS_P2MR":
                    continue  # QTC: the node wallet cannot sign a spend of an unbroadcast parent
                child_tx = wallet.create_self_transfer_multi(utxos_to_spend=[tx["new_utxo"]], target_vsize=target_vsize)
                assert_equal(child_tx['tx'].get_vsize(), target_vsize)


    def test_wallet_tagging(self):
        """Verify that tagged wallet instances are able to send funds."""
        self.log.info("Test tagged wallet instances...")
        node = self.nodes[0]
        untagged_wallet = dict(self.wallets)["ADDRESS_P2MR"]  # QTC: only P2MR outputs are standard
        for i in range(10):
            tag = ''.join(random.choice(string.ascii_letters) for _ in range(20))
            self.log.debug(f"-> ({i}) tag name: {tag}")
            tagged_wallet = MiniWallet(node, tag_name=tag, mode=MiniWalletMode.ADDRESS_P2MR)
            # QTC: like send_to(), but re-signed after the outputs are edited and
            # funded well above the default fee of a P2MR spend (0.003117 QTC)
            amount = 1_000_000
            tx = untagged_wallet.create_self_transfer(fee_rate=0)["tx"]
            tx.vout[0].nValue -= amount + 1000
            tx.vout.append(CTxOut(amount, tagged_wallet.get_output_script()))
            untagged_wallet.sign_tx(tx)
            untagged_wallet.sendrawtransaction(from_node=node, tx_hex=tx.serialize().hex())
            tagged_wallet.rescan_utxos()
            tagged_wallet.send_self_transfer(from_node=node)
        self.generate(node, 1)  # clear mempool

    def run_test(self):
        node = self.nodes[0]
        self.wallets = [
            ("ADDRESS_OP_TRUE", MiniWallet(node, mode=MiniWalletMode.ADDRESS_OP_TRUE)),
            ("RAW_OP_TRUE",     MiniWallet(node, mode=MiniWalletMode.RAW_OP_TRUE)),
            ("RAW_P2PKH",        MiniWallet(node, mode=MiniWalletMode.RAW_P2PKH)),
            ("ADDRESS_P2MR",    MiniWallet(node, mode=MiniWalletMode.ADDRESS_P2MR)),
        ]
        for _, wallet in self.wallets:
            self.generate(wallet, 10)
        self.generate(wallet, COINBASE_MATURITY)

        self.test_tx_padding()
        self.test_wallet_tagging()


if __name__ == '__main__':
    FeatureFrameworkMiniWalletTest(__file__).main()
