{
  "variables": {
    "pqc_common_cflags": [ "-Wno-sign-compare", "-Wno-unused-variable", "-Wno-implicit-function-declaration", "-O3" ]
  },
  "target_defaults": {
    "cflags": [ "<@(pqc_common_cflags)" ],
    "conditions": [
      ["OS=='mac'", {
        "xcode_settings": { "OTHER_CFLAGS": [ "<@(pqc_common_cflags)" ], "GCC_SYMBOLS_PRIVATE_EXTERN": "YES" }
      }]
    ]
  },
  "targets": [
    {
      "target_name": "pqc_mldsa",
      "type": "static_library",
      "sources": [
        "src/c_sources/ml_dsa/keygen.c",
        "src/c_sources/ml_dsa/sign.c",
        "src/c_sources/ml_dsa/verify.c",
        "src/c_sources/ml_dsa/utils.c",
        "src/c_sources/dilithium_ref/sign.c",
        "src/c_sources/dilithium_ref/packing.c",
        "src/c_sources/dilithium_ref/polyvec.c",
        "src/c_sources/dilithium_ref/poly.c",
        "src/c_sources/dilithium_ref/ntt.c",
        "src/c_sources/dilithium_ref/reduce.c",
        "src/c_sources/dilithium_ref/rounding.c",
        "src/c_sources/dilithium_ref/fips202.c",
        "src/c_sources/dilithium_ref/symmetric-shake.c"
      ],
      "include_dirs": [ "src/c_sources/include", "src/c_sources/dilithium_ref" ],
      "defines": [ "DILITHIUM_MODE=2", "CUSTOM_RANDOMBYTES=1", "randombytes=qtc_mldsa_randombytes" ]
    },
    {
      "target_name": "pqc_slhdsa",
      "type": "static_library",
      "sources": [
        "src/c_sources/slh_dsa/keygen.c",
        "src/c_sources/slh_dsa/sign.c",
        "src/c_sources/slh_dsa/verify.c",
        "src/c_sources/slh_dsa/utils.c",
        "src/c_sources/sphincsplus_ref/address.c",
        "src/c_sources/sphincsplus_ref/fors.c",
        "src/c_sources/sphincsplus_ref/hash_shake.c",
        "src/c_sources/sphincsplus_ref/merkle.c",
        "src/c_sources/sphincsplus_ref/sign.c",
        "src/c_sources/sphincsplus_ref/thash_shake_simple.c",
        "src/c_sources/sphincsplus_ref/utils.c",
        "src/c_sources/sphincsplus_ref/utilsx1.c",
        "src/c_sources/sphincsplus_ref/wots.c",
        "src/c_sources/sphincsplus_ref/wotsx1.c",
        "src/c_sources/sphincsplus_ref/fips202.c"
      ],
      "include_dirs": [ "src/c_sources/include", "src/c_sources/sphincsplus_ref" ],
      "defines": [ "PARAMS=sphincs-shake-128s", "CRYPTO_ALGNAME=\"SPHINCS+-shake-128s\"", "CUSTOM_RANDOMBYTES=1", "SPHINCSPLUS_VARIANT_REF=1", "randombytes=qtc_slhdsa_randombytes" ]
    },
    {
      "target_name": "bitcoinpqc",
      "sources": [
        "src/native/bitcoinpqc_addon.cc",
        "src/native/randombytes_shim.c",
        "src/c_sources/bitcoinpqc.c"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "src/c_sources/include"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")",
        "pqc_mldsa",
        "pqc_slhdsa"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS" ],
      "conditions": [
        ["OS=='win'", { "msvs_settings": { "VCCLCompilerTool": { "ExceptionHandling": 1 } } }],
        ["OS=='mac'", { "xcode_settings": { "GCC_ENABLE_CPP_EXCEPTIONS": "YES" } }]
      ]
    }
  ]
}
