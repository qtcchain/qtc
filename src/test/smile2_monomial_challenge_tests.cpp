// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// f3 soundness residual (see redteam/F3_MSIS_SOUNDNESS_REDUCTION.md): the SMILE2 CT proof's
// special-soundness extractor relies on every monomial Fiat-Shamir challenge c = +/-X^k being a
// UNIT in R_q = Z_q[X]/(X^128+1) -- so that from two accepting transcripts on distinct challenges a
// genuine short witness (or a short M-SIS collision) extracts, and "guess the challenge" never
// substitutes for "break M-SIS". This reproduces that load-bearing check directly.

#include <shielded/smile2/ct_proof.h>
#include <shielded/smile2/ntt.h>
#include <shielded/smile2/params.h>
#include <shielded/smile2/poly.h>

#include <boost/test/unit_test.hpp>

using namespace smile2;

BOOST_AUTO_TEST_SUITE(smile2_monomial_challenge_tests)

// Every one of the 256 monomial challenges (+/-X^k, k in [0,128)) inverts: c * c^{-1} == 1.
BOOST_AUTO_TEST_CASE(all_monomial_challenges_are_units)
{
    size_t checked = 0;
    for (size_t k = 0; k < POLY_DEGREE; ++k) {
        for (const int64_t sign : {int64_t{1}, mod_q(-1)}) {
            SmilePoly c{};
            c.coeffs[k] = sign;
            const SmilePoly inv = InvertMonomialChallenge(c);
            SmilePoly prod = NttMul(c, inv);
            prod.Reduce();
            // prod must be the multiplicative identity 1 (constant term 1, all others 0).
            BOOST_CHECK_EQUAL(mod_q(prod.coeffs[0]), int64_t{1});
            for (size_t i = 1; i < POLY_DEGREE; ++i) {
                BOOST_CHECK_EQUAL(mod_q(prod.coeffs[i]), int64_t{0});
            }
            ++checked;
        }
    }
    BOOST_CHECK_EQUAL(checked, 2 * POLY_DEGREE); // all 256 monomials exercised
}

// The inverse of a monomial is itself a monomial (single nonzero coefficient): X^k inverts to a
// scalar times X^{d-k}, never a dense element -- the structural fact behind the small-norm response.
BOOST_AUTO_TEST_CASE(monomial_inverse_is_a_monomial)
{
    for (size_t k = 0; k < POLY_DEGREE; ++k) {
        SmilePoly c{};
        c.coeffs[k] = 1;
        SmilePoly inv = InvertMonomialChallenge(c);
        inv.Reduce();
        size_t nonzero = 0;
        for (size_t i = 0; i < POLY_DEGREE; ++i) {
            if (mod_q(inv.coeffs[i]) != 0) ++nonzero;
        }
        BOOST_CHECK_EQUAL(nonzero, size_t{1});
    }
}

BOOST_AUTO_TEST_SUITE_END()
