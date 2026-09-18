#!/usr/bin/env python3
# Copyright (c) 2024 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that descriptor wallets rescan mempool transactions properly when importing."""

from test_framework.address import address_to_scriptpubkey
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import COIN, CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet, MiniWalletMode
from test_framework.wallet_util import test_address


def p2mr_send_to(wallet, node, scriptPubKey, amount, fee=1000):
    """QTC: MiniWallet.send_to() with a re-signed PQ witness (adding the
    destination output invalidates the ML-DSA signature)."""
    tx = wallet.create_self_transfer(fee_rate=0)["tx"]
    tx.vout[0].nValue -= amount + fee
    tx.vout.append(CTxOut(amount, scriptPubKey))
    wallet.sign_tx(tx)
    txid = wallet.sendrawtransaction(from_node=node, tx_hex=tx.serialize().hex())
    return {"txid": txid, "hex": tx.serialize().hex(), "tx": tx}

class WalletRescanUnconfirmed(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_sqlite()

    def run_test(self):
        self.log.info("Create wallets and mine initial chain")
        node = self.nodes[0]
        tester_wallet = MiniWallet(node, mode=MiniWalletMode.ADDRESS_P2MR)
        self.generate(tester_wallet, COINBASE_MATURITY + 1)  # QTC: the cached chain has no P2MR coins

        node.createwallet(wallet_name='w0', disable_private_keys=False)
        w0 = node.get_wallet_rpc('w0')

        self.log.info("Create a parent tx and mine it in a block that will later be disconnected")
        parent_address = w0.getnewaddress()
        tx_parent_to_reorg = p2mr_send_to(tester_wallet, node, address_to_scriptpubkey(parent_address), COIN)
        assert tx_parent_to_reorg["txid"] in node.getrawmempool()
        block_to_reorg = self.generate(tester_wallet, 1)[0]
        assert_equal(len(node.getrawmempool()), 0)
        node.syncwithvalidationinterfacequeue()
        assert_equal(w0.gettransaction(tx_parent_to_reorg["txid"])["confirmations"], 1)

        # Create an unconfirmed child transaction from the parent tx, sending all
        # the funds to an address that is not in w0/w1 (QTC: an unspendable P2WSH output
        # is not relay-standard, so sweep to the MiniWallet's P2MR address instead).
        # Importantly, no change output is created so the
        # transaction can't be recognized using its outputs. The wallet rescan needs to know the
        # inputs of the transaction to detect it, so the parent must be processed before the child.
        w0_utxos = w0.listunspent()

        self.log.info("Create a child tx and wait for it to propagate to all mempools")
        # The only UTXO available to spend is tx_parent_to_reorg.
        assert_equal(len(w0_utxos), 1)
        assert_equal(w0_utxos[0]["txid"], tx_parent_to_reorg["txid"])
        tx_child_unconfirmed_sweep = w0.sendall(recipients=[tester_wallet.get_address()], options={"locktime":0})
        assert tx_child_unconfirmed_sweep["txid"] in node.getrawmempool()
        node.syncwithvalidationinterfacequeue()

        self.log.info("Mock a reorg, causing parent to re-enter mempools after its child")
        node.invalidateblock(block_to_reorg)
        assert tx_parent_to_reorg["txid"] in node.getrawmempool()

        self.log.info("Import descriptor wallet on another node")
        # descriptor is ranged - label not allowed
        # QTC: ranged mr() descriptors cannot be imported watch-only (PQ public
        # keys are derived from the private seed), so import the private
        # descriptor into a wallet with private keys instead.
        descriptors_to_import = [{"desc": d["desc"], "timestamp": 0} for d in w0.listdescriptors(True)["descriptors"]]

        node.createwallet(wallet_name="w1", blank=True)
        w1 = node.get_wallet_rpc("w1")
        for res in w1.importdescriptors(descriptors_to_import):
            assert_equal(res["success"], True)

        self.log.info("Check that the importing node has properly rescanned mempool transactions")
        # Check that parent address is correctly determined as ismine
        test_address(w1, parent_address, ismine=True)
        # This would raise a JSONRPCError if the transactions were not identified as belonging to the wallet.
        assert_equal(w1.gettransaction(tx_parent_to_reorg["txid"])["confirmations"], 0)
        assert_equal(w1.gettransaction(tx_child_unconfirmed_sweep["txid"])["confirmations"], 0)

if __name__ == '__main__':
    WalletRescanUnconfirmed(__file__).main()
