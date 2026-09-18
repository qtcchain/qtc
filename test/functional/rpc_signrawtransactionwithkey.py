#!/usr/bin/env python3
# Copyright (c) 2015-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test transaction signing using the signrawtransactionwithkey RPC."""

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    COIN,
    CTxOut,
)
from test_framework.address import (
    address_to_scriptpubkey,
    p2a,
    script_to_p2sh,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)
from test_framework.script_util import (
    key_to_p2pk_script,
    key_to_p2pkh_script,
    script_to_p2wsh_script,
)
from test_framework.wallet import (
    getnewdestination,
    MiniWallet,
    MiniWalletMode,
)
from test_framework.wallet_util import (
    generate_keypair,
)

from decimal import (
    Decimal,
)

INPUTS = [
    # Valid pay-to-pubkey scripts
    {'txid': '9b907ef1e3c26fc71fe4a4b3580bc75264112f95050014157059c736f0202e71', 'vout': 0,
     'scriptPubKey': '76a91460baa0f494b38ce3c940dea67f3804dc52d1fb9488ac'},
    {'txid': '83a4f6a6b73660e13ee6cb3c6063fa3759c50c9b7521d0536022961898f4fb02', 'vout': 0,
     'scriptPubKey': '76a914669b857c03a5ed269d5d85a1ffac9ed5d663072788ac'},
]
OUTPUTS = {'mpLQjfK79b7CCV4VMJWEWAj5Mpx8Up5zxB': 0.1}

class SignRawTransactionWithKeyTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)  # QTC: ADDRESS_P2MR MiniWallet signs via the node wallet

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.num_nodes = 1
        # QTC: the funded scripts are not P2MR, so the node must accept non-standard outputs
        self.extra_args = [["-acceptnonstdtxn=1"]]

    def send_to_address(self, addr, amount, fee=1000):
        script_pub_key = address_to_scriptpubkey(addr)
        # QTC: like MiniWallet.send_to(), but re-sign after appending the output
        tx = self.wallet.create_self_transfer(fee_rate=0)["tx"]
        tx.vout[0].nValue -= (int(amount * COIN) + fee)
        tx.vout.append(CTxOut(int(amount * COIN), script_pub_key))
        self.wallet.sign_tx(tx)
        txid = self.wallet.sendrawtransaction(from_node=self.nodes[0], tx_hex=tx.serialize().hex())
        return txid, 1

    def assert_signing_completed_successfully(self, signed_tx):
        assert 'errors' not in signed_tx
        assert 'complete' in signed_tx
        assert_equal(signed_tx['complete'], True)

    def successful_signing_test(self):
        """Create and sign a valid raw transaction with one input.

        Expected results:

        1) The transaction has a complete set of signatures
        2) No script verification error occurred"""
        self.log.info("Test valid raw transaction with one input")
        privKeys = ['cUeKHd5orzT3mz8P9pxyREHfsWtVfgsfDjiZZBcjUBAaGk1BTj7N', 'cVKpPfVKSJxKqVpE9awvXNWuLHCa5j5tiE7K6zbUSptFpTEtiFrA']
        rawTx = self.nodes[0].createrawtransaction(INPUTS, OUTPUTS)
        rawTxSigned = self.nodes[0].signrawtransactionwithkey(rawTx, privKeys, INPUTS)

        self.assert_signing_completed_successfully(rawTxSigned)

    def witness_script_test(self):
        self.log.info("Test signing transaction to P2SH-P2WSH addresses without wallet")
        # QTC: createmultisig only builds PQ p2mr multisig, so the legacy
        # P2SH-P2WSH 1-of-1 multisig case is skipped.
        embedded_privkey, embedded_pubkey = generate_keypair(wif=True)
        assert_raises_rpc_error(-8, "Only address type 'p2mr' is supported", self.nodes[0].createmultisig, 1, [embedded_pubkey.hex()], "p2sh-segwit")

        # Now test with P2PKH and P2PK scripts as the witnessScript
        for tx_type in ['P2PKH', 'P2PK']:  # these tests are order-independent
            self.verify_txn_with_witness_script(tx_type)

    def keyless_signing_test(self):
        self.log.info("Test that keyless 'signing' of pay-to-anchor input succeeds")
        [txid, vout] = self.send_to_address(p2a(), 24.999)  # QTC: coinbase is 25
        spending_tx = self.nodes[0].createrawtransaction(
            [{"txid": txid, "vout": vout}],
            [{getnewdestination()[2]: Decimal("24.998")}])
        spending_tx_signed = self.nodes[0].signrawtransactionwithkey(spending_tx, [], [])
        self.assert_signing_completed_successfully(spending_tx_signed)
        assert self.nodes[0].testmempoolaccept([spending_tx_signed["hex"]])[0]["allowed"]
        # 'signing' a P2A prevout is a no-op, so signed and unsigned txs shouldn't differ
        assert_equal(spending_tx, spending_tx_signed["hex"])

    def verify_txn_with_witness_script(self, tx_type):
        self.log.info("Test with a {} script as the witnessScript".format(tx_type))
        embedded_privkey, embedded_pubkey = generate_keypair(wif=True)
        witness_script = {
            'P2PKH': key_to_p2pkh_script(embedded_pubkey).hex(),
            'P2PK': key_to_p2pk_script(embedded_pubkey).hex()
        }.get(tx_type, "Invalid tx_type")
        redeem_script = script_to_p2wsh_script(witness_script).hex()
        addr = script_to_p2sh(redeem_script)
        script_pub_key = address_to_scriptpubkey(addr).hex()
        # Fund that address
        [txid, vout] = self.send_to_address(addr, 10)
        # Now create and sign a transaction spending that output on node[0], which doesn't know the scripts or keys
        spending_tx = self.nodes[0].createrawtransaction([{'txid': txid, 'vout': vout}], {getnewdestination()[2]: Decimal("9.999")})
        spending_tx_signed = self.nodes[0].signrawtransactionwithkey(spending_tx, [embedded_privkey], [{'txid': txid, 'vout': vout, 'scriptPubKey': script_pub_key, 'redeemScript': redeem_script, 'witnessScript': witness_script, 'amount': 10}])
        self.assert_signing_completed_successfully(spending_tx_signed)
        self.nodes[0].sendrawtransaction(spending_tx_signed['hex'])

    def invalid_sighashtype_test(self):
        self.log.info("Test signing transaction with invalid sighashtype")
        tx = self.nodes[0].createrawtransaction(INPUTS, OUTPUTS)
        privkeys = [self.nodes[0].get_deterministic_priv_key().key]
        assert_raises_rpc_error(-8, "'all' is not a valid sighash parameter.", self.nodes[0].signrawtransactionwithkey, tx, privkeys, sighashtype="all")

    def invalid_private_key_and_tx(self):
        self.log.info("Test signing transaction with an invalid private key")
        tx = self.nodes[0].createrawtransaction(INPUTS, OUTPUTS)
        privkeys = ["123"]
        assert_raises_rpc_error(-5, "Invalid private key", self.nodes[0].signrawtransactionwithkey, tx, privkeys)
        self.log.info("Test signing transaction with an invalid tx hex")
        privkeys = [self.nodes[0].get_deterministic_priv_key().key]
        assert_raises_rpc_error(-22, "TX decode failed. Make sure the tx has at least one input.", self.nodes[0].signrawtransactionwithkey, tx + "00", privkeys)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0], mode=MiniWalletMode.ADDRESS_P2MR)
        self.generate(self.wallet, COINBASE_MATURITY + 25)  # QTC: the cached chain has no P2MR coins
        self.successful_signing_test()
        self.witness_script_test()
        self.keyless_signing_test()
        self.invalid_sighashtype_test()
        self.invalid_private_key_and_tx()


if __name__ == '__main__':
    SignRawTransactionWithKeyTest(__file__).main()
