#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""QTC regtest subsidy schedule checks.

Regtest follows Bitcoin's monetary policy at the regtest halving cadence
(CRegTestParams in src/kernel/chainparams.cpp):

* nInitialSubsidy        = 50 QTC   (src/consensus/params.h)
* nSubsidyHalvingInterval = 150 blocks

There is no tail emission or decay term: GetBlockSubsidy() right-shifts the
initial subsidy once per completed halving interval.  The treasury premine and
the empty-block subsidy penalty are inactive on regtest unless enabled through
-regtest* options, so an empty coinbase pays exactly the scheduled amount.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

COIN = 100_000_000
INITIAL_SUBSIDY = 50 * COIN
HALVING_INTERVAL = 150
# Two full halvings keep the test well inside the regtest mining budget.
CHAIN_HEIGHT = 2 * HALVING_INTERVAL


def expected_subsidy(height):
    return INITIAL_SUBSIDY >> (height // HALVING_INTERVAL)


class QTCSubsidyScheduleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[]]

    def coinbase_value(self, node, height):
        block = node.getblock(node.getblockhash(height), 2)
        coinbase = block["tx"][0]
        return int(sum(Decimal(out["value"]) for out in coinbase["vout"]) * COIN)

    def run_test(self):
        node = self.nodes[0]

        info = node.getblockchaininfo()
        assert_equal(info["chain"], "regtest")
        assert_equal(info["blocks"], 0)

        self.log.info("Genesis pays the initial subsidy at the regtest genesis difficulty")
        genesis_hash = node.getblockhash(0)
        stats = node.getblockstats(genesis_hash)
        assert_equal(stats["height"], 0)
        assert_equal(stats["subsidy"], INITIAL_SUBSIDY)

        header = node.getblockheader(genesis_hash)
        assert_equal(header["bits"], "207fffff")
        assert_equal(header["height"], 0)

        self.log.info(f"Mine {CHAIN_HEIGHT} blocks and check the schedule across two halvings")
        self.generate(node, CHAIN_HEIGHT)
        assert_equal(node.getblockcount(), CHAIN_HEIGHT)

        checkpoints = [
            (1, INITIAL_SUBSIDY),
            (HALVING_INTERVAL - 1, INITIAL_SUBSIDY),
            (HALVING_INTERVAL, INITIAL_SUBSIDY // 2),
            (HALVING_INTERVAL + 1, INITIAL_SUBSIDY // 2),
            (2 * HALVING_INTERVAL - 1, INITIAL_SUBSIDY // 2),
            (2 * HALVING_INTERVAL, INITIAL_SUBSIDY // 4),
        ]
        for height, subsidy in checkpoints:
            assert_equal(expected_subsidy(height), subsidy)
            stats = node.getblockstats(height)
            assert_equal(stats["height"], height)
            assert_equal(stats["subsidy"], subsidy)
            assert_equal(stats["totalfee"], 0)

        self.log.info("Every block subsidy in the chain matches the schedule")
        for height in range(CHAIN_HEIGHT + 1):
            assert_equal(node.getblockstats(height)["subsidy"], expected_subsidy(height))

        self.log.info("Empty coinbases pay the full scheduled subsidy (no premine, no penalty)")
        for height in (1, HALVING_INTERVAL, 2 * HALVING_INTERVAL):
            assert_equal(self.coinbase_value(node, height), expected_subsidy(height))


if __name__ == "__main__":
    QTCSubsidyScheduleTest(__file__).main()
