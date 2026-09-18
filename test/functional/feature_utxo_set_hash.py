#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test UTXO set hash value calculation in gettxoutsetinfo."""

from test_framework.messages import (
    CBlock,
    COutPoint,
    from_hex,
)
from test_framework.crypto.muhash import MuHash3072
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet, MiniWalletMode

class UTXOSetHashTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=True, legacy=False)  # QTC: ADDRESS_P2MR MiniWallet signs via the node wallet

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def test_muhash_implementation(self):
        self.log.info("Test MuHash implementation consistency")

        node = self.nodes[0]
        wallet = MiniWallet(node, mode=MiniWalletMode.ADDRESS_P2MR)
        mocktime = node.getblockheader(node.getblockhash(0))['time'] + 1
        node.setmocktime(mocktime)

        # Generate 100 blocks and remove the first since we plan to spend its
        # coinbase. QTC connects the genesis block normally, so its coinbase
        # output is part of the UTXO set and must be hashed too.
        block_hashes = [node.getblockhash(0)] + self.generate(wallet, 1) + self.generate(node, 99)
        blocks = [(height, from_hex(CBlock(), node.getblock(block, False))) for height, block in enumerate(block_hashes)]
        blocks.pop(1)

        self.log.info("Test deterministic UTXO set hash results")
        # QTC: checked before the P2MR spend below, since its randomized ML-DSA
        # signature changes the witness commitment and thus the coinbase txid
        assert_equal(node.gettxoutsetinfo()['hash_serialized_3'], "575c374a4ee5b3247c61030c92ff085a023e8542549bbee506f3b8f6f2286643")
        assert_equal(node.gettxoutsetinfo("muhash")['muhash'], "27dcc8d40048e4cdcd5efe8c48a14b3c602241144f2ee8e00c5f4a987041ca7e")

        # Create a spending transaction and mine a block which includes it
        txid = wallet.send_self_transfer(from_node=node)['txid']
        tx_block = self.generateblock(node, output=wallet.get_address(), transactions=[txid])
        blocks.append((node.getblockcount(), from_hex(CBlock(), node.getblock(tx_block['hash'], False))))

        # Serialize the outputs that should be in the UTXO set and add them to
        # a MuHash object
        muhash = MuHash3072()

        for height, block in blocks:
            for tx in block.vtx:
                for n, tx_out in enumerate(tx.vout):
                    coinbase = 1 if not tx.vin[0].prevout.hash else 0

                    # Skip witness commitment
                    if (coinbase and n > 0):
                        continue

                    data = COutPoint(int(tx.rehash(), 16), n).serialize()
                    data += (height * 2 + coinbase).to_bytes(4, "little")
                    data += tx_out.serialize()

                    muhash.insert(data)

        finalized = muhash.digest()
        node_muhash = node.gettxoutsetinfo("muhash")['muhash']

        assert_equal(finalized[::-1].hex(), node_muhash)

    def run_test(self):
        self.test_muhash_implementation()


if __name__ == '__main__':
    UTXOSetHashTest(__file__).main()
