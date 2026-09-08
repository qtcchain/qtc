#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""C-002 SLH-DSA wallet/mempool boundary coverage.

This reproduces the observed post-C-002 failure mode with a low regtest
activation height: SLH-only P2MR spends must sign in the mode required by the
next block, mempool admission must verify with the same next-block flags, and
legacy-mode transactions created just before the boundary must not remain
relayable once the next block requires FIPS-205 SLH-DSA.
"""

from decimal import Decimal
import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


C002_HEIGHT = 110
SLH_OUTPUT_COUNT = 8
SLH_SIGNATURE_SIZES = {7856, 7857}


class WalletC002SLHMempoolBoundaryTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[f"-regtestshieldedc002activationheight={C002_HEIGHT}"]]
        self.rpc_timeout = 240

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def _assert_p2mr_address(self, address):
        assert address.startswith("qtcrt1z")

    def _find_vout_for_address(self, node, tx_hex, address):
        decoded = node.decoderawtransaction(tx_hex)
        for vout in decoded.get("vout", []):
            if vout.get("scriptPubKey", {}).get("address") == address:
                return vout["n"]
        raise AssertionError(f"address {address} not found in transaction")

    def _assert_slh_witness(self, node, tx_hex):
        decoded = node.decoderawtransaction(tx_hex)
        vins = decoded.get("vin", [])
        assert_equal(len(vins), 1)
        witness = vins[0].get("txinwitness", [])
        assert_equal(len(witness), 3)
        sig_size = len(bytes.fromhex(witness[0]))
        assert sig_size in SLH_SIGNATURE_SIZES, sig_size

    def _assert_mempool_allowed(self, node, tx_hex):
        accepted = node.testmempoolaccept([tx_hex])[0]
        if not accepted.get("allowed", False):
            raise AssertionError(f"expected mempool acceptance, got {accepted}")
        return accepted

    def _import_slh_forced_descriptor(self, node, wallet):
        """Import an SLH-spendable P2MR descriptor with an unreachable ML leaf."""
        descs = wallet.listdescriptors(True)["descriptors"]
        assert len(descs) >= 1
        ext = next((d for d in descs if not d.get("internal", False)), descs[0])["desc"]
        assert ext.startswith("mr(")

        ext_no_checksum = ext.split("#", 1)[0]
        inner = ext_no_checksum[len("mr("):-1]
        key_expr = inner.split(",pk_slh(", 1)[0]

        fixed_mldsa_hex = os.urandom(1312).hex()
        no_checksum = f"mr({fixed_mldsa_hex},pk_slh({key_expr}))"
        info = node.getdescriptorinfo(no_checksum)
        desc = f"{no_checksum}#{info['checksum']}"

        range_end = SLH_OUTPUT_COUNT - 1
        result = wallet.importdescriptors([{
            "desc": desc,
            "active": False,
            "timestamp": "now",
            "range": [0, range_end],
        }])[0]
        assert_equal(result["success"], True)
        addresses = node.deriveaddresses(desc, [0, range_end])
        for address in addresses:
            self._assert_p2mr_address(address)
        return desc, addresses

    def _build_signed_send(self, wallet, dest, input_txid, input_vout, amount):
        result = wallet.send(
            outputs=[{dest: amount}],
            options={
                "add_to_wallet": False,
                "add_inputs": False,
                "inputs": [{"txid": input_txid, "vout": input_vout}],
                "fee_rate": 1,
            },
        )
        assert_equal(result["complete"], True)
        assert "hex" in result
        return result["hex"]

    def _assert_incomplete_psbt_result(self, result):
        assert_equal(result["complete"], False)
        assert "hex" not in result

    def _descriptor_arg(self, desc):
        return [{"desc": desc, "range": [0, SLH_OUTPUT_COUNT - 1]}]

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="miner", descriptors=True)
        node.createwallet(wallet_name="slh", descriptors=True)
        node.createwallet(wallet_name="receiver", descriptors=True)
        miner = node.get_wallet_rpc("miner")
        slh = node.get_wallet_rpc("slh")
        receiver = node.get_wallet_rpc("receiver")

        mine_addr = miner.getnewaddress()
        self.log.info("Mine mature funds up to the pre-boundary setup height")
        self.generatetoaddress(node, C002_HEIGHT - 3, mine_addr, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), C002_HEIGHT - 3)

        self.log.info("Fund multiple confirmed SLH-only P2MR outputs before C-002")
        slh_desc, slh_addresses = self._import_slh_forced_descriptor(node, slh)
        fund = miner.send(
            outputs=[{address: Decimal("1.0")} for address in slh_addresses],
            options={"fee_rate": 1},
        )
        fund_txid = fund["txid"]
        self.generatetoaddress(node, 1, mine_addr, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), C002_HEIGHT - 2)

        fund_hex = miner.gettransaction(fund_txid)["hex"]
        slh_vouts = [self._find_vout_for_address(node, fund_hex, address) for address in slh_addresses]

        self.log.info("A legacy SLH transaction signed for block H-1 becomes stale at the C-002 boundary")
        stale_psbt_dest = receiver.getnewaddress(address_type="p2mr")
        stale_created = slh.walletcreatefundedpsbt(
            [{"txid": fund_txid, "vout": slh_vouts[0]}],
            [{stale_psbt_dest: Decimal("0.40")}],
            0,
            {"add_inputs": False, "fee_rate": 1},
        )
        stale_processed = slh.walletprocesspsbt(stale_created["psbt"])
        assert_equal(stale_processed["complete"], True)
        assert "hex" in stale_processed
        stale_psbt = stale_processed["psbt"]
        self._assert_slh_witness(node, stale_processed["hex"])
        self._assert_mempool_allowed(node, stale_processed["hex"])

        stale_dest = receiver.getnewaddress(address_type="p2mr")
        stale_hex = self._build_signed_send(
            slh,
            stale_dest,
            fund_txid,
            slh_vouts[1],
            Decimal("0.50"),
        )
        self._assert_slh_witness(node, stale_hex)
        self._assert_mempool_allowed(node, stale_hex)
        stale_txid = node.sendrawtransaction(stale_hex)
        assert stale_txid in node.getrawmempool()

        self.generateblock(node, mine_addr, [], sync_fun=self.no_op)
        assert_equal(node.getblockcount(), C002_HEIGHT - 1)
        assert stale_txid not in node.getrawmempool()

        stale_accept = node.testmempoolaccept([stale_hex])[0]
        assert_equal(stale_accept["allowed"], False)
        stale_reason = stale_accept.get("reject-reason", "")
        assert (
            "Invalid SLH-DSA signature" in stale_reason
            or "mandatory-script-verify-flag-failed" in stale_reason
            or "mempool-script-verify-flag-failed" in stale_reason
        ), stale_accept

        self.log.info("Stale finalized legacy SLH PSBTs are reported incomplete after C-002")
        self._assert_incomplete_psbt_result(node.finalizepsbt(stale_psbt))
        stale_analysis = node.analyzepsbt(stale_psbt)
        assert_equal(stale_analysis["inputs"][0]["is_final"], False)
        self._assert_incomplete_psbt_result(slh.walletprocesspsbt(psbt=stale_psbt, sign=False, finalize=False))
        self._assert_incomplete_psbt_result(
            node.descriptorprocesspsbt(
                stale_psbt,
                self._descriptor_arg(slh_desc),
                {"sighashtype": "ALL", "finalize": True},
            )
        )

        self.log.info("Direct wallet signing creates a FIPS-205 SLH spend for the activation block")
        direct_dest = receiver.getnewaddress(address_type="p2mr")
        direct_hex = self._build_signed_send(
            slh,
            direct_dest,
            fund_txid,
            slh_vouts[2],
            Decimal("0.50"),
        )
        self._assert_slh_witness(node, direct_hex)
        self._assert_mempool_allowed(node, direct_hex)
        direct_txid = node.sendrawtransaction(direct_hex)
        self.generatetoaddress(node, 1, mine_addr, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), C002_HEIGHT)
        assert_equal(receiver.gettransaction(direct_txid)["confirmations"], 1)

        self.log.info("finalizepsbt and analyzepsbt accept fresh post-C-002 FIPS-205 SLH PSBTs")
        finalize_dest = receiver.getnewaddress(address_type="p2mr")
        finalize_created = slh.walletcreatefundedpsbt(
            [{"txid": fund_txid, "vout": slh_vouts[3]}],
            [{finalize_dest: Decimal("0.40")}],
            0,
            {"add_inputs": False, "fee_rate": 1},
        )
        fips_partial = slh.walletprocesspsbt(finalize_created["psbt"], finalize=False)
        self._assert_incomplete_psbt_result(fips_partial)
        finalized_psbt = node.finalizepsbt(fips_partial["psbt"], False)
        assert_equal(finalized_psbt["complete"], True)
        assert "psbt" in finalized_psbt
        analysis = node.analyzepsbt(finalized_psbt["psbt"])
        assert_equal(analysis["inputs"][0]["is_final"], True)
        extracted = node.finalizepsbt(fips_partial["psbt"])
        assert_equal(extracted["complete"], True)
        assert "hex" in extracted
        self._assert_slh_witness(node, extracted["hex"])
        self._assert_mempool_allowed(node, extracted["hex"])
        finalize_txid = node.sendrawtransaction(extracted["hex"])
        self.generatetoaddress(node, 1, mine_addr, sync_fun=self.no_op)
        assert_equal(receiver.gettransaction(finalize_txid)["confirmations"], 1)

        self.log.info("walletprocesspsbt signs in FIPS-205 mode and passes mempool after activation")
        psbt_dest = receiver.getnewaddress(address_type="p2mr")
        created = slh.walletcreatefundedpsbt(
            [{"txid": fund_txid, "vout": slh_vouts[4]}],
            [{psbt_dest: Decimal("0.40")}],
            0,
            {"add_inputs": False, "fee_rate": 1},
        )
        processed = slh.walletprocesspsbt(created["psbt"])
        assert_equal(processed["complete"], True)
        assert "hex" in processed
        self._assert_slh_witness(node, processed["hex"])
        self._assert_mempool_allowed(node, processed["hex"])
        psbt_txid = node.sendrawtransaction(processed["hex"])
        self.generatetoaddress(node, 1, mine_addr, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), C002_HEIGHT + 2)
        assert_equal(receiver.gettransaction(psbt_txid)["confirmations"], 1)

        self.log.info("descriptorprocesspsbt signs and extracts fresh post-C-002 FIPS-205 SLH PSBTs")
        descriptor_dest = receiver.getnewaddress(address_type="p2mr")
        descriptor_created = slh.walletcreatefundedpsbt(
            [{"txid": fund_txid, "vout": slh_vouts[5]}],
            [{descriptor_dest: Decimal("0.40")}],
            0,
            {"add_inputs": False, "fee_rate": 1},
        )
        descriptor_processed = node.descriptorprocesspsbt(
            descriptor_created["psbt"],
            self._descriptor_arg(slh_desc),
            {"sighashtype": "ALL", "finalize": True},
        )
        assert_equal(descriptor_processed["complete"], True)
        assert "hex" in descriptor_processed
        self._assert_slh_witness(node, descriptor_processed["hex"])
        self._assert_mempool_allowed(node, descriptor_processed["hex"])
        descriptor_txid = node.sendrawtransaction(descriptor_processed["hex"])
        self.generatetoaddress(node, 1, mine_addr, sync_fun=self.no_op)
        assert_equal(node.getblockcount(), C002_HEIGHT + 3)
        assert_equal(receiver.gettransaction(descriptor_txid)["confirmations"], 1)


if __name__ == "__main__":
    WalletC002SLHMempoolBoundaryTest(__file__).main()
