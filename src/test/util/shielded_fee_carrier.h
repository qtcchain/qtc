// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_SHIELDED_FEE_CARRIER_H
#define BITCOIN_TEST_UTIL_SHIELDED_FEE_CARRIER_H

#include <addresstype.h>
#include <coins.h>
#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/translation.h>

#include <boost/test/unit_test.hpp>

#include <map>

namespace test::shielded {

static constexpr CAmount DEFAULT_SHIELDED_FEE_CARRIER_FEE{40'000};

inline void ReSignCoinbaseSpend(TestChain100Setup& setup,
                                CMutableTransaction& tx,
                                const CTransactionRef& funding_tx)
{
    FillableSigningProvider keystore;
    BOOST_REQUIRE(keystore.AddKey(setup.coinbaseKey));
    BOOST_REQUIRE_GT(funding_tx->vout.size(), 0U);

    std::map<COutPoint, Coin> input_coins;
    input_coins.emplace(COutPoint{funding_tx->GetHash(), 0},
                        Coin{funding_tx->vout[0], /*nHeight=*/0, /*fCoinBase=*/true});

    std::map<int, bilingual_str> input_errors;
    BOOST_REQUIRE(SignTransaction(tx, &keystore, input_coins, SIGHASH_ALL, input_errors));
}

inline void AttachCoinbaseFeeCarrier(TestChain100Setup& setup,
                                     CMutableTransaction& tx,
                                     const CTransactionRef& funding_tx,
                                     CAmount fee = DEFAULT_SHIELDED_FEE_CARRIER_FEE)
{
    BOOST_REQUIRE_GT(funding_tx->vout.size(), 0U);
    BOOST_REQUIRE_GT(funding_tx->vout[0].nValue, fee);

    tx.vin = {CTxIn{COutPoint{funding_tx->GetHash(), 0}}};
    tx.vout = {CTxOut{funding_tx->vout[0].nValue - fee,
                      GetScriptForDestination(WitnessV2P2MR(uint256::ONE))}};

    ReSignCoinbaseSpend(setup, tx, funding_tx);
}

} // namespace test::shielded

#endif // BITCOIN_TEST_UTIL_SHIELDED_FEE_CARRIER_H
