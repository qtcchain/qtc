// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QTC_MATMUL_TRANSCRIPT_H
#define QTC_MATMUL_TRANSCRIPT_H

#include <matmul/noise.h>
#include <matmul/matrix.h>

#include <crypto/sha256.h>
#include <span.h>
#include <uint256.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace matmul::transcript {

inline constexpr std::string_view COMPRESS_TAG{"matmul-compress-v1"};
/** Product-committed digest v4 (QTC O5): every b×b tile of C' is hashed in full
 *  (SHA-256 over b² LE32 elements, row-major), the tile hashes are hashed in
 *  row-major tile order into a root, and the digest is
 *  SHA256d(tag || sigma || root || dim_le32 || b_le32). The v3 linear
 *  compression was removable from the mining loop (H4): a miner could evaluate
 *  the compressed words in O(n³/b) without forming C'. */
inline constexpr std::string_view PRODUCT_DIGEST_TAG{"matmul-product-digest-v4"};

std::vector<field::Element> DeriveCompressionVector(const uint256& sigma, uint32_t b);
field::Element CompressBlock(const Matrix& block_bb, const std::vector<field::Element>& v);
field::Element CompressBlock(const ConstMatrixView& block_bb, const std::vector<field::Element>& v);

class TranscriptHasher {
public:
    TranscriptHasher(const uint256& sigma, uint32_t b);

    void AddIntermediate(uint32_t i, uint32_t j, uint32_t ell, const Matrix& block_bb);
    void AddIntermediate(uint32_t i, uint32_t j, uint32_t ell, const ConstMatrixView& block_bb);
    uint256 Finalize();

private:
    uint32_t m_b;
    CSHA256 m_hasher;
    std::vector<field::Element> m_compress_vec;
};

struct CanonicalResult {
    Matrix C_prime;
    uint256 transcript_hash;
};

CanonicalResult CanonicalMatMul(const Matrix& A_prime, const Matrix& B_prime, uint32_t b, const uint256& sigma);
std::vector<Matrix> PrecomputeCleanBlockProducts(const Matrix& A, const Matrix& B, uint32_t b);
uint256 ReplayCanonicalHashWithReusableCleanProducts(
    const Matrix& A,
    const Matrix& B,
    const std::vector<Matrix>& clean_block_products,
    const noise::NoisePair& noise,
    uint32_t b,
    const uint256& sigma);

/** Legacy transcript-scheme helper (pre product-digest heights on regtest only). */
uint256 HashMatrixWords(Span<const field::Element> words);
uint256 FinalizeTranscriptDigestFromWords(Span<const field::Element> words);

/** v4 product-committed digest primitives. */
uint256 HashProductTile(const ConstMatrixView& tile);
uint256 HashProductTile(Span<const field::Element> tile_row_major);
std::vector<uint256> ComputeProductTileHashes(const Matrix& C_prime, uint32_t b);
std::vector<uint256> ComputeProductTileHashesFromWords(Span<const field::Element> c_prime_words, uint32_t dim, uint32_t b);
uint256 HashProductTileHashes(Span<const uint256> tile_hashes);
uint256 FinalizeProductCommittedDigestFromHash(const uint256& tile_hash_root,
                                               const uint256& sigma,
                                               uint32_t dim,
                                               uint32_t b);
uint256 ComputeProductCommittedDigestFromTileHashes(Span<const uint256> tile_hashes,
                                                    const uint256& sigma,
                                                    uint32_t dim,
                                                    uint32_t b);
/** c_prime_words: the full C' (dim×dim elements, row-major). Validators rebuild
 *  the tile hashes from the carried C' payload in O(n²) and then use Freivalds
 *  to confirm A'B' == C'. */
uint256 ComputeProductCommittedDigestFromWords(Span<const field::Element> c_prime_words,
                                               const uint256& sigma,
                                               uint32_t dim,
                                               uint32_t b);
uint256 ComputeProductCommittedDigest(const Matrix& C_prime, uint32_t b, const uint256& sigma);
uint256 ComputeProductCommittedDigestFromPerturbed(const Matrix& A_prime,
                                                   const Matrix& B_prime,
                                                   uint32_t b,
                                                   const uint256& sigma);

} // namespace matmul::transcript

#endif // QTC_MATMUL_TRANSCRIPT_H
