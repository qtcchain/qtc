#!/usr/bin/env python3
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""MatMul invalid-block DoS mitigation smoke test.

A node flooded with blocks whose MatMul digest has been corrupted must reject
them cheaply, keep its tip, keep serving a clean block afterwards, and (on
regtest, soft-fail mode) never ban anyone.  Two rejection paths exist and both
are exercised with -test=matmulstrict:

* Header path.  The digest exceeds the nBits target, so CheckBlockHeader
  (MatMul phase 1, src/pow.cpp CheckMatMulProofOfWork_Phase1) rejects the
  block as ``high-hash`` before the header ever reaches the block index.

* Body path.  The digest stays under target, so the header is accepted, but it
  no longer equals the digest committed by the Freivalds product payload and
  ContextualCheckBlock rejects the block as ``invalid-product-payload``.  That
  result is BLOCK_MUTATED: the header is kept as a headers-only tip and is not
  cached as failed, so resubmitting the identical block yields the same reason
  (never ``duplicate-invalid``).

Before the product-committed digest (nMatMulProductDigestHeight = 0 on regtest)
the body path fell through to the O(n^3) transcript recomputation and surfaced
as ``high-hash``; the reject reason moved, the header layout did not.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# CBlockHeader serialization (src/primitives/block.h):
#   nVersion(4) hashPrevBlock(32) hashMerkleRoot(32) nTime(4) nBits(4)
#   nNonce64(8) matmul_digest(32) matmul_dim(2) seed_a(32) seed_b(32)
NBITS_OFFSET = 4 + 32 + 32 + 4
DIGEST_OFFSET = NBITS_OFFSET + 4 + 8
DIGEST_BYTES = 32
FLOOD_ROUNDS = 6


def read_le(block_hex, offset, size):
    return int.from_bytes(bytes.fromhex(block_hex[offset * 2:(offset + size) * 2]), "little")


def write_le(block_hex, offset, size, value):
    return (block_hex[:offset * 2]
            + value.to_bytes(size, "little").hex()
            + block_hex[(offset + size) * 2:])


def target_from_nbits(nbits):
    exponent = nbits >> 24
    mantissa = nbits & 0x007fffff
    if exponent <= 3:
        return mantissa >> (8 * (3 - exponent))
    return mantissa << (8 * (exponent - 3))


class QTCMatMulDosMitigationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-test=matmulstrict"],
            ["-test=matmulstrict"],
        ]

    def _candidate(self, node):
        return node.generateblock("raw(51)", [], False, called_by_framework=True)["hex"]

    def _over_target_candidate(self, node):
        """Digest pushed above the nBits target: fails MatMul phase 1 (header path)."""
        candidate = self._candidate(node)
        target = target_from_nbits(read_le(candidate, NBITS_OFFSET, 4))
        digest = read_le(candidate, DIGEST_OFFSET, DIGEST_BYTES)
        assert digest <= target  # the miner produced a valid header
        bad_digest = digest | (0xff << (8 * (DIGEST_BYTES - 1)))
        assert bad_digest > target
        return write_le(candidate, DIGEST_OFFSET, DIGEST_BYTES, bad_digest)

    def _mismatched_candidate(self, node):
        """Digest still under target but detached from the product payload (body path)."""
        while True:
            candidate = self._candidate(node)
            target = target_from_nbits(read_le(candidate, NBITS_OFFSET, 4))
            bad_digest = read_le(candidate, DIGEST_OFFSET, DIGEST_BYTES) ^ 0x1
            if bad_digest <= target:
                return write_le(candidate, DIGEST_OFFSET, DIGEST_BYTES, bad_digest)

    def run_test(self):
        node0, node1 = self.nodes

        self.generate(node0, 10)
        self.sync_blocks()
        tip_hash = node0.getbestblockhash()
        tip_height = node0.getblockcount()
        tips_before = len(node1.getchaintips())

        self.log.info("Header path: over-target digests are rejected as high-hash without touching the index")
        for _ in range(FLOOD_ROUNDS):
            assert_equal(node1.submitblock(self._over_target_candidate(node0)), "high-hash")
            assert_equal(node1.getbestblockhash(), tip_hash)
        assert_equal(len(node1.getchaintips()), tips_before)
        repeated = self._over_target_candidate(node0)
        assert_equal(node1.submitblock(repeated), "high-hash")
        assert_equal(node1.submitblock(repeated), "high-hash")
        assert_equal(len(node1.getchaintips()), tips_before)

        self.log.info("Body path: under-target digests that break the product commitment are rejected as invalid-product-payload")
        for _ in range(FLOOD_ROUNDS):
            assert_equal(node1.submitblock(self._mismatched_candidate(node0)), "invalid-product-payload")
            assert_equal(node1.getbestblockhash(), tip_hash)
        repeated = self._mismatched_candidate(node0)
        assert_equal(node1.submitblock(repeated), "invalid-product-payload")
        # BLOCK_MUTATED is not cached as a failure, so the same block is re-evaluated, not duplicate-invalid.
        assert_equal(node1.submitblock(repeated), "invalid-product-payload")
        for tip in node1.getchaintips():
            if tip["hash"] == tip_hash:
                assert_equal(tip["status"], "active")
                assert_equal(tip["height"], tip_height)
            else:
                assert_equal(tip["status"], "headers-only")
                assert_equal(tip["height"], tip_height + 1)

        self.log.info("The node still accepts and relays a clean block after the flood")
        assert_equal(node1.submitblock(self._candidate(node0)), None)
        assert_equal(node1.getblockcount(), tip_height + 1)
        self.sync_blocks()
        assert_equal(node0.getbestblockhash(), node1.getbestblockhash())

        self.log.info("Regtest stays in soft-fail mode: no bans, peers still connected")
        assert_equal(node0.listbanned(), [])
        assert_equal(node1.listbanned(), [])
        assert node0.getconnectioncount() > 0
        assert node1.getconnectioncount() > 0


if __name__ == "__main__":
    QTCMatMulDosMitigationTest(__file__).main()
