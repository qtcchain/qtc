#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""v0.32.2 wallet RPC guards for the shielded sunset.

Regtest keeps the shielded sunset disabled by default so legacy shielded tests
can opt into the old flows. This test lowers the sunset/pool-credit gates to
the next block and verifies wallet RPCs refuse new shielded credits and
shielded-output appends before building transactions.
"""

from decimal import Decimal

from test_framework.shielded_utils import encrypt_and_unlock_wallet
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error


class WalletShieldedSunsetRpcGuardsTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-regtestshieldedpoolcreditdisableheight=1",
            "-regtestshieldedsunsetheight=1",
            "-regtestshieldedrecoveryexitactivationheight=1",
        ]]
        self.rpc_timeout = 1200

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="shielded", descriptors=True)
        wallet = encrypt_and_unlock_wallet(node, "shielded")
        zaddr = wallet.z_getnewaddress()

        self.log.info("Transparent-to-shielded RPCs fail before planning/building")
        assert_raises_rpc_error(-4, "z_planshieldfunds is disabled at height 1", wallet.z_planshieldfunds, Decimal("1.0"), zaddr)
        assert_raises_rpc_error(-4, "z_shieldfunds is disabled at height 1", wallet.z_shieldfunds, Decimal("1.0"), zaddr)
        assert_raises_rpc_error(-4, "z_shieldcoinbase is disabled at height 1", wallet.z_shieldcoinbase)
        assert_raises_rpc_error(-4, "z_fundpsbt is disabled at height 1", wallet.z_fundpsbt, Decimal("1.0"), zaddr)

        self.log.info("Private shielded-output append RPCs fail before building")
        assert_raises_rpc_error(-4, "z_sendtoaddress is disabled at height 1", wallet.z_sendtoaddress, zaddr, Decimal("0.1"))
        assert_raises_rpc_error(
            -4,
            "z_sendmany with shielded recipients is disabled at height 1",
            wallet.z_sendmany,
            [{"address": zaddr, "amount": Decimal("0.1")}],
        )
        assert_raises_rpc_error(-4, "z_mergenotes is disabled at height 1", wallet.z_mergenotes)
        assert_raises_rpc_error(-4, "z_recoverstrandednote is disabled at height 1", wallet.z_recoverstrandednote, "00" * 32)
        assert_raises_rpc_error(-4, "z_rotateaddress is disabled at height 1", wallet.z_rotateaddress, zaddr)
        assert_raises_rpc_error(-4, "z_revokeaddress is disabled at height 1", wallet.z_revokeaddress, zaddr)

        self.log.info("Bridge shield/control transaction builders fail before plan decoding")
        assert_raises_rpc_error(-4, "bridge_buildingressbatchtx is disabled at height 1", wallet.bridge_buildingressbatchtx, "00", [], [])
        assert_raises_rpc_error(-4, "bridge_buildegressbatchtx is disabled at height 1", wallet.bridge_buildegressbatchtx, "00", [], [], [])
        assert_raises_rpc_error(-4, "bridge_buildshieldtx is disabled at height 1", wallet.bridge_buildshieldtx, "00", "00" * 32, 0, Decimal("1.0"))
        assert_raises_rpc_error(-4, "bridge_submitshieldtx is disabled at height 1", wallet.bridge_submitshieldtx, "00", "00" * 32, 0, Decimal("1.0"))
        assert_raises_rpc_error(-4, "bridge_submitrebalancetx is disabled at height 1", wallet.bridge_submitrebalancetx, [], [])


if __name__ == '__main__':
    WalletShieldedSunsetRpcGuardsTest(__file__).main()
