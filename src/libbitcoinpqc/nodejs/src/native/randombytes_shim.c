/**
 * Single randombytes provider for the Node.js addon.
 *
 * The vendored Dilithium and SPHINCS+ reference implementations both define a global
 * `randombytes` with incompatible prototypes. libbitcoinpqc's static archive gets away with
 * it through archive pull order; an addon that links every object directly does not. As in
 * the WASM build (wasm/wasm_randombytes.c), binding.gyp compiles the two cores as separate
 * static libraries with -Drandombytes=qtc_mldsa_randombytes / qtc_slhdsa_randombytes, and this
 * file provides both correctly typed functions, each routing to its own algorithm's
 * caller-supplied entropy state (src/ml_dsa/utils.c and src/slh_dsa/utils.c).
 */
#include <stddef.h>
#include <stdint.h>

extern void custom_randombytes_impl(uint8_t *out, size_t outlen);
extern void custom_slh_randombytes_impl(uint8_t *out, size_t outlen);

void qtc_mldsa_randombytes(uint8_t *out, size_t outlen)
{
    custom_randombytes_impl(out, outlen);
}

void qtc_slhdsa_randombytes(unsigned char *x, unsigned long long xlen)
{
    custom_slh_randombytes_impl((uint8_t *)x, (size_t)xlen);
}
