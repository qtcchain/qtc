#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The OP_RETURN cap rises at nOpReturnRaiseHeight (testnet3: 83 -> 1,004 bytes at height 2290).

Below the raise height a transaction whose OP_RETURN script exceeds the legacy cap is rejected by the
mempool and by block validation; from the raise height the chain's final cap applies. Regtest carries
the final cap from genesis, so the raise is placed with -opreturnraiseheight.
"""

from test_framework.messages import tx_from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

LEGACY_PAYLOAD = 80     # 1 + 2 + 80 = 83-byte script, the legacy cap
RAISED_PAYLOAD = 101    # the QTC-EVM anchor
FINAL_PAYLOAD = 1000    # 1 + 3 + 1000 = 1,004-byte script, the final cap
OVER_PAYLOAD = 1001


class OpReturnRaiseTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def opreturn_tx(self, w, payload_len):
        """A signed transaction paying one OP_RETURN of payload_len bytes and change."""
        raw = self.nodes[0].createrawtransaction([], [{"data": "ab" * payload_len}])
        funded = w.fundrawtransaction(raw)["hex"]
        return w.signrawtransactionwithwallet(funded)["hex"]

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("w")
        w = node.get_wallet_rpc("w")
        addr = w.getnewaddress()
        self.generatetoaddress(node, 110, addr)

        raise_height = node.getblockcount() + 3
        self.log.info(f"Raise height {raise_height}; restarting with -opreturnraiseheight")
        self.restart_node(0, extra_args=[f"-opreturnraiseheight={raise_height}"])
        w = node.get_wallet_rpc("w")

        self.log.info("Below the raise height: the legacy 83-byte cap is consensus")
        small = self.opreturn_tx(w, LEGACY_PAYLOAD)
        assert_equal(node.testmempoolaccept([small])[0]["allowed"], True)
        big = self.opreturn_tx(w, RAISED_PAYLOAD)
        res = node.testmempoolaccept([big])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "bad-txns-opreturn-size")
        assert_raises_rpc_error(-25, "bad-txns-opreturn-size", node.generateblock, addr, [big])

        self.log.info("Mine up to the raise height")
        self.generatetoaddress(node, raise_height - 1 - node.getblockcount(), addr)
        assert_equal(node.getblockcount() + 1, raise_height)

        self.log.info("From the raise height: 1,004-byte scripts are accepted and mined, 1,005 rejected")
        res = node.testmempoolaccept([big])[0]
        assert_equal(res["allowed"], True)
        txid = node.sendrawtransaction(big)
        final = self.opreturn_tx(w, FINAL_PAYLOAD)
        assert_equal(node.testmempoolaccept([final])[0]["allowed"], True)
        over = self.opreturn_tx(w, OVER_PAYLOAD)
        res = node.testmempoolaccept([over])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "bad-txns-opreturn-size")
        self.generatetoaddress(node, 1, addr)
        assert_equal(node.getrawtransaction(txid, True)["confirmations"], 1)
        assert_equal(len(tx_from_hex(big).vout), 2)


if __name__ == "__main__":
    OpReturnRaiseTest(__file__).main()
