#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Sign a transaction that mixes an HD P2MR input with an imported fixed-key mr(<hex>) input (#20).

The imported descriptor is public-key only; its private key lives in the wallet's HD
descriptor. The raw signing path must combine the two for every input, in either input
order, and the result must be accepted by the mempool.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than


class WalletP2MRMixedInputsTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("w")
        w = node.get_wallet_rpc("w")
        addr = w.getnewaddress()
        self.generatetoaddress(node, 110, addr)
        assert_greater_than(w.getbalance(), 100)

        self.log.info("Import the fixed-key form of one of the wallet's own ML-DSA-44 keys")
        pubkey = w.exportpqkey(addr, "ml-dsa-44")["pubkey"]
        desc = node.getdescriptorinfo(f"mr({pubkey})")["descriptor"]
        assert_equal(w.importdescriptors([{"desc": desc, "timestamp": "now"}])[0]["success"], True)
        fixed_addr = node.deriveaddresses(desc)[0]

        self.log.info("Fund the fixed-key output and pick an ordinary HD coin")
        fixed_txid = w.sendtoaddress(fixed_addr, 0.001)
        self.generatetoaddress(node, 1, addr)
        fixed_vout = [o["n"] for o in w.gettransaction(fixed_txid, True, True)["decoded"]["vout"]
                      if o["scriptPubKey"].get("address") == fixed_addr][0]
        hd = [u for u in w.listunspent() if u["txid"] != fixed_txid and u["amount"] >= 1][0]
        fixed_in = {"txid": fixed_txid, "vout": fixed_vout}
        hd_in = {"txid": hd["txid"], "vout": hd["vout"]}
        # Spend both coins minus a fee that stays under the mempool's default fee-rate cap.
        output_amount = Decimal("0.001") + hd["amount"] - Decimal("0.001")

        for order_name, inputs in (("fixed-key first", [fixed_in, hd_in]), ("HD first", [hd_in, fixed_in])):
            self.log.info(f"Sign the mixed transaction with the raw path, {order_name}")
            raw = node.createrawtransaction(inputs, {addr: output_amount})
            signed = w.signrawtransactionwithwallet(raw)
            assert_equal(signed.get("errors", []), [])
            assert_equal(signed["complete"], True)
            decoded = node.decoderawtransaction(signed["hex"])
            for vin in decoded["vin"]:
                assert_greater_than(len(vin["txinwitness"]), 0)
            assert_equal(node.testmempoolaccept([signed["hex"]])[0]["allowed"], True)

        self.log.info("fundrawtransaction with the fixed-key input seeded, then sign and broadcast")
        raw = node.createrawtransaction([fixed_in], {addr: 0.5})
        funded = w.fundrawtransaction(raw)["hex"]
        signed = w.signrawtransactionwithwallet(funded)
        assert_equal(signed["complete"], True)
        txid = node.sendrawtransaction(signed["hex"])
        self.generatetoaddress(node, 1, addr)
        assert_equal(w.gettransaction(txid)["confirmations"], 1)


if __name__ == "__main__":
    WalletP2MRMixedInputsTest(__file__).main()
