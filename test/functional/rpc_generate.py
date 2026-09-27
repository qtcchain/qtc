#!/usr/bin/env python3
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test generate* RPCs."""

from concurrent.futures import ThreadPoolExecutor

from test_framework.authproxy import JSONRPCException
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.segwit_addr import decode_segwit_address, encode_segwit_address
from test_framework.test_framework import BitcoinTestFramework
from test_framework.wallet import MiniWallet, MiniWalletMode
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)


class RPCGenerateTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)  # QTC: ADDRESS_P2MR MiniWallet signs via the node wallet

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        self.test_generatetoaddress()
        self.test_generate()
        self.test_generateblock()

    def test_generatetoaddress(self):
        node = self.nodes[0]
        # QTC: coinbases pay to witness v2 P2MR, so mine to the MiniWallet's P2MR address.
        address = MiniWallet(node, mode=MiniWalletMode.ADDRESS_P2MR).get_address()
        block_hash = self.generatetoaddress(node, 1, address)[0]
        block = node.getblock(block_hash, 2)
        assert_equal(block['tx'][0]['vout'][0]['scriptPubKey']['address'], address)
        # QTC D8: regtest requires the MatMul product payload from genesis.
        assert_greater_than(block['matrix_c_words'], 0)

        # The same P2MR witness program under the mainnet HRP is not a regtest address.
        witver, witprog = decode_segwit_address('qtcrt', address)
        assert_equal(witver, 2)
        mainnet_address = encode_segwit_address('qtc', witver, witprog)
        assert mainnet_address.startswith('qtc1z')
        assert_raises_rpc_error(-5, "Invalid address", self.generatetoaddress, node, 1, mainnet_address)

    def test_generateblock(self):
        node = self.nodes[0]
        miniwallet = MiniWallet(node, mode=MiniWalletMode.ADDRESS_P2MR)
        self.generate(miniwallet, COINBASE_MATURITY + 15)  # QTC: the cached chain has no P2MR coins

        def derive_one(descriptor: str) -> str:
            return node.deriveaddresses(node.getdescriptorinfo(descriptor)["descriptor"])[0]

        self.log.info('Mine an empty block to address and return the hex')
        address = miniwallet.get_address()
        generated_block = self.generateblock(node, output=address, transactions=[], submit=False)
        node.submitblock(hexdata=generated_block['hex'])
        assert_equal(generated_block['hash'], node.getbestblockhash())

        self.log.info('Generate an empty block to address')
        hash = self.generateblock(node, output=address, transactions=[])['hash']
        block = node.getblock(blockhash=hash, verbose=2)
        assert_equal(len(block['tx']), 1)
        assert_equal(block['tx'][0]['vout'][0]['scriptPubKey']['address'], address)

        self.log.info('Generate an empty block to a descriptor')
        hash = self.generateblock(node, 'addr(' + address + ')', [])['hash']
        block = node.getblock(blockhash=hash, verbosity=2)
        assert_equal(len(block['tx']), 1)
        assert_equal(block['tx'][0]['vout'][0]['scriptPubKey']['address'], address)

        self.log.info('Generate an empty block to a combo descriptor with compressed pubkey')
        combo_key = '0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798'
        combo_descriptor = node.getdescriptorinfo('combo(' + combo_key + ')')['descriptor']
        combo_addresses = {derive_one(f'pkh({combo_key})')}
        for descriptor in (f'wpkh({combo_key})', f'sh(wpkh({combo_key}))'):
            try:
                combo_addresses.add(derive_one(descriptor))
            except JSONRPCException:
                pass
        hash = self.generateblock(node, combo_descriptor, [])['hash']
        block = node.getblock(hash, 2)
        assert_equal(len(block['tx']), 1)
        assert block['tx'][0]['vout'][0]['scriptPubKey']['address'] in combo_addresses

        self.log.info('Generate an empty block to a combo descriptor with uncompressed pubkey')
        combo_key = '0408ef68c46d20596cc3f6ddf7c8794f71913add807f1dc55949fa805d764d191c0b7ce6894c126fce0babc6663042f3dde9b0cf76467ea315514e5a6731149c67'
        combo_descriptor = node.getdescriptorinfo('combo(' + combo_key + ')')['descriptor']
        combo_addresses = {derive_one(f'pkh({combo_key})')}
        for descriptor in (f'wpkh({combo_key})', f'sh(wpkh({combo_key}))'):
            try:
                combo_addresses.add(derive_one(descriptor))
            except JSONRPCException:
                pass
        hash = self.generateblock(node, combo_descriptor, [])['hash']
        block = node.getblock(hash, 2)
        assert_equal(len(block['tx']), 1)
        assert block['tx'][0]['vout'][0]['scriptPubKey']['address'] in combo_addresses

        # Generate some extra mempool transactions to verify they don't get mined
        for _ in range(10):
            miniwallet.send_self_transfer(from_node=node)

        self.log.info('Generate block with txid')
        txid = miniwallet.send_self_transfer(from_node=node)['txid']
        hash = self.generateblock(node, address, [txid])['hash']
        block = node.getblock(hash, 1)
        assert_equal(len(block['tx']), 2)
        assert_equal(block['tx'][1], txid)

        self.log.info('Generate block with raw tx')
        rawtx = miniwallet.create_self_transfer()['hex']
        hash = self.generateblock(node, address, [rawtx])['hash']

        block = node.getblock(hash, 1)
        assert_equal(len(block['tx']), 2)
        txid = block['tx'][1]
        assert_equal(node.getrawtransaction(txid=txid, verbose=False, blockhash=hash), rawtx)

        # Ensure that generateblock can be called concurrently by many threads.
        # QTC: MatMul solving is slow enough for concurrent callers to race. The
        # node rejects a block whose parent is no longer the tip with an explicit
        # stale-tip error, so tolerate exactly that error. A block that passes
        # the check just before a racing block connects is still stored as a
        # same-height sibling, so not every returned block extends the chain.
        self.log.info('Generate blocks in parallel')
        height_before = node.getblockcount()

        def generate_50_blocks(n):
            hashes = []
            for _ in range(50):
                try:
                    hashes.append(n.generateblock(output=address, transactions=[])['hash'])
                except JSONRPCException as e:
                    assert_equal(e.error['code'], -32603)
                    assert_equal(e.error['message'], 'chain tip changed during mining; mined block is stale')
            return hashes

        rpcs = [node.cli for _ in range(6)]
        with ThreadPoolExecutor(max_workers=len(rpcs)) as threads:
            mined = [h for hashes in threads.map(generate_50_blocks, rpcs) for h in hashes]
        for block_hash in mined:
            assert_greater_than(node.getblockheader(block_hash)['height'], height_before)
        assert_greater_than(node.getblockcount(), height_before)
        assert node.getblockcount() <= height_before + len(mined)

        self.log.info('Fail to generate block with out of order txs')
        txid1 = miniwallet.send_self_transfer(from_node=node)['txid']
        utxo1 = miniwallet.get_utxo(txid=txid1)
        rawtx2 = miniwallet.create_self_transfer(utxo_to_spend=utxo1)['hex']
        # QTC: on MatMul chains generateblock skips the pre-mining TestBlockValidity
        # (the MatMul seeds are only derived while solving), so mine without
        # submitting and check that the node rejects the solved block.
        tip_before = node.getbestblockhash()
        out_of_order_block = self.generateblock(node, address, [rawtx2, txid1], submit=False)['hex']
        assert_equal(node.submitblock(out_of_order_block), 'bad-txns-inputs-missingorspent')
        assert_equal(node.getbestblockhash(), tip_before)

        self.log.info('Fail to generate block with txid not in mempool')
        missing_txid = '0000000000000000000000000000000000000000000000000000000000000000'
        assert_raises_rpc_error(-5, 'Transaction ' + missing_txid + ' not in mempool.', self.generateblock, node, address, [missing_txid])

        self.log.info('Fail to generate block with invalid raw tx')
        invalid_raw_tx = '0000'
        assert_raises_rpc_error(-22, 'Transaction decode failed for ' + invalid_raw_tx, self.generateblock, node, address, [invalid_raw_tx])

        self.log.info('Fail to generate block with invalid address/descriptor')
        assert_raises_rpc_error(-5, 'Invalid address or descriptor', self.generateblock, node, '1234', [])

        self.log.info('Fail to generate block with a ranged descriptor')
        ranged_descriptor = 'pkh(tpubD6NzVbkrYhZ4XgiXtGrdW5XDAPFCL9h7we1vwNCpn8tGbBcgfVYjXyhWo4E1xkh56hjod1RhGjxbaTLV3X4FyWuejifB9jusQ46QzG87VKp/0/*)'
        assert_raises_rpc_error(-8, 'Ranged descriptor not accepted. Maybe pass through deriveaddresses first?', self.generateblock, node, ranged_descriptor, [])

        self.log.info('Fail to generate block with a descriptor missing a private key')
        child_descriptor = 'pkh(tpubD6NzVbkrYhZ4XgiXtGrdW5XDAPFCL9h7we1vwNCpn8tGbBcgfVYjXyhWo4E1xkh56hjod1RhGjxbaTLV3X4FyWuejifB9jusQ46QzG87VKp/0\'/0)'
        assert_raises_rpc_error(-5, 'Cannot derive script without private keys', self.generateblock, node, child_descriptor, [])

    def test_generate(self):
        message = (
            "generate\n\n"
            "has been replaced by the -generate "
            "cli option. Refer to -help for more information.\n"
        )

        self.log.info("Test rpc generate raises with message to use cli option")
        assert_raises_rpc_error(-32601, message, self.nodes[0].rpc.generate)

        self.log.info("Test rpc generate help prints message to use cli option")
        assert_equal(message, self.nodes[0].help("generate"))

        self.log.info("Test rpc generate is a hidden command not discoverable in general help")
        assert message not in self.nodes[0].help()


if __name__ == "__main__":
    RPCGenerateTest(__file__).main()
