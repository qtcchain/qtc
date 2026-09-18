#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test datacarrier functionality"""
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    CTxOut,
    MAX_OP_RETURN_RELAY,
)
from test_framework.script import (
    CScript,
    OP_RETURN,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import TestNode
from test_framework.util import assert_raises_rpc_error
from test_framework.wallet import MiniWallet, MiniWalletMode

from random import randbytes


class DataCarrierTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 4
        self.extra_args = [
            [],
            ["-datacarrier=0"],
            ["-datacarrier=1", f"-datacarriersize={MAX_OP_RETURN_RELAY - 1}"],
            ["-datacarrier=1", "-datacarriersize=2"],
        ]

    def skip_test_if_missing_module(self):
        # QTC relay policy only accepts P2MR and OP_RETURN outputs, so the
        # MiniWallet must spend real (wallet-signed) P2MR coins.
        self.skip_if_no_wallet()

    def test_null_data_transaction(self, node: TestNode, data, success: bool) -> None:
        tx = self.wallet.create_self_transfer(fee_rate=0)["tx"]
        data = [] if data is None else [data]
        tx.vout.append(CTxOut(nValue=0, scriptPubKey=CScript([OP_RETURN] + data)))
        tx.vout[0].nValue -= tx.get_vsize()  # simply pay 1sat/vbyte fee
        self.wallet.sign_tx(tx)  # P2MR witness carries a real signature; re-sign after editing outputs

        tx_hex = tx.serialize().hex()

        if success:
            self.wallet.sendrawtransaction(from_node=node, tx_hex=tx_hex)
            assert tx.rehash() in node.getrawmempool(True), f'{tx_hex} not in mempool'
        else:
            assert_raises_rpc_error(-26, "scriptpubkey", self.wallet.sendrawtransaction, from_node=node, tx_hex=tx_hex)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0], mode=MiniWalletMode.ADDRESS_P2MR)
        # The pre-mined chain has no P2MR coins; mine enough mature ones for every case.
        self.generate(self.wallet, 20 + COINBASE_MATURITY)

        # By default, 1000 bytes are used for data (+1 for OP_RETURN, +3 for OP_PUSHDATA2 and its length).
        default_size_data = randbytes(MAX_OP_RETURN_RELAY - 4)
        too_long_data = randbytes(MAX_OP_RETURN_RELAY - 3)
        small_data = randbytes(MAX_OP_RETURN_RELAY - 5)
        one_byte = randbytes(1)
        zero_bytes = randbytes(0)

        self.log.info("Testing null data transaction with default -datacarrier and -datacarriersize values.")
        self.test_null_data_transaction(node=self.nodes[0], data=default_size_data, success=True)

        self.log.info("Testing a null data transaction larger than allowed by the default -datacarriersize value.")
        self.test_null_data_transaction(node=self.nodes[0], data=too_long_data, success=False)

        self.log.info("Testing a null data transaction with -datacarrier=false.")
        self.test_null_data_transaction(node=self.nodes[1], data=default_size_data, success=False)

        self.log.info("Testing a null data transaction with a size larger than accepted by -datacarriersize.")
        self.test_null_data_transaction(node=self.nodes[2], data=default_size_data, success=False)

        self.log.info("Testing a null data transaction with a size smaller than accepted by -datacarriersize.")
        self.test_null_data_transaction(node=self.nodes[2], data=small_data, success=True)

        self.log.info("Testing a null data transaction with no data.")
        self.test_null_data_transaction(node=self.nodes[0], data=None, success=True)
        self.test_null_data_transaction(node=self.nodes[1], data=None, success=False)
        self.test_null_data_transaction(node=self.nodes[2], data=None, success=True)
        self.test_null_data_transaction(node=self.nodes[3], data=None, success=True)

        self.log.info("Testing a null data transaction with zero bytes of data.")
        self.test_null_data_transaction(node=self.nodes[0], data=zero_bytes, success=True)
        self.test_null_data_transaction(node=self.nodes[1], data=zero_bytes, success=False)
        self.test_null_data_transaction(node=self.nodes[2], data=zero_bytes, success=True)
        self.test_null_data_transaction(node=self.nodes[3], data=zero_bytes, success=True)

        self.log.info("Testing a null data transaction with one byte of data.")
        self.test_null_data_transaction(node=self.nodes[0], data=one_byte, success=True)
        self.test_null_data_transaction(node=self.nodes[1], data=one_byte, success=False)
        self.test_null_data_transaction(node=self.nodes[2], data=one_byte, success=True)
        self.test_null_data_transaction(node=self.nodes[3], data=one_byte, success=False)


if __name__ == '__main__':
    DataCarrierTest(__file__).main()
