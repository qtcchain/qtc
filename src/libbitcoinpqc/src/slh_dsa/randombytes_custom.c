#include <stddef.h>
#include <stdint.h>

/*
 * Entropy entry point for the SPHINCS+ sources, routed through the SLH-DSA
 * wrapper's seeded source (src/slh_dsa/utils.c).
 *
 * The SPHINCS+ code calls randombytes(); the Dilithium shim
 * (dilithium/ref/randombytes_custom.c) defines a function of that name too,
 * and whichever archive member the linker meets first used to win, so
 * SLH-DSA signing silently drew from the ML-DSA source and this wrapper's
 * hedging was dead code. The SPHINCS+ sources are therefore compiled with
 * -Drandombytes=slh_dsa_randombytes (see CMakeLists.txt) and this file
 * defines that symbol under its real name.
 */

#if defined(SPHINCSPLUS_VARIANT_SHAKE_A64)
#include "../../sphincsplus/shake-a64/randombytes.h"
#elif defined(SPHINCSPLUS_VARIANT_SHAKE_AVX2)
#include "../../sphincsplus/shake-avx2/randombytes.h"
#elif defined(SPHINCSPLUS_VARIANT_REF)
#include "../../sphincsplus/ref/randombytes.h"
#else
#error "SPHINCS+ variant not configured"
#endif

/* Forward declaration of our custom implementation from utils.c */
extern void custom_slh_randombytes_impl(uint8_t *out, size_t outlen);

void slh_dsa_randombytes(unsigned char *x, unsigned long long xlen)
{
    custom_slh_randombytes_impl((uint8_t*)x, (size_t)xlen);
}
