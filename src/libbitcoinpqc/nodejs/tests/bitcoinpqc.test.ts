import {
  Algorithm,
  PqcError,
  generateKeyPair,
  publicKeySize,
  secretKeySize,
  sign,
  signWithRandomness,
  signatureSize,
  verify,
  isNativeAddonLoaded,
} from "../src";

// Deterministic pseudo-random bytes for tests (not for real keys).
function testBytes(size: number, seed: number): Uint8Array {
  const bytes = new Uint8Array(size);
  let x = seed >>> 0;
  for (let i = 0; i < size; i++) {
    x = (x * 1664525 + 1013904223) >>> 0;
    bytes[i] = x >>> 24;
  }
  return bytes;
}

describe("Bitcoin PQC", () => {
  test("the native addon is loaded (the mock fallback must never pass as a real result)", () => {
    expect(isNativeAddonLoaded()).toBe(true);
  });

  describe("key sizes", () => {
    test("reports non-zero sizes for every algorithm", () => {
      for (const algo of [Algorithm.SECP256K1_SCHNORR, Algorithm.ML_DSA_44, Algorithm.SLH_DSA_SHAKE_128S]) {
        expect(publicKeySize(algo)).toBeGreaterThan(0);
        expect(secretKeySize(algo)).toBeGreaterThan(0);
        expect(signatureSize(algo)).toBeGreaterThan(0);
      }
      // FIPS 204 / FIPS 205 parameter sets used by QTC
      expect(publicKeySize(Algorithm.ML_DSA_44)).toBe(1312);
      expect(signatureSize(Algorithm.ML_DSA_44)).toBe(2420);
      expect(publicKeySize(Algorithm.SLH_DSA_SHAKE_128S)).toBe(32);
      expect(signatureSize(Algorithm.SLH_DSA_SHAKE_128S)).toBe(7856);
    });
  });

  describe.each([
    ["ML-DSA-44", Algorithm.ML_DSA_44],
    ["SLH-DSA-SHAKE-128s", Algorithm.SLH_DSA_SHAKE_128S],
  ])("%s", (_name, algorithm) => {
    const message = new TextEncoder().encode("Hello, QTC PQC!");

    test("generates a keypair, signs and verifies", () => {
      const keypair = generateKeyPair(algorithm, testBytes(128, 1));
      expect(keypair.publicKey.bytes.length).toBe(publicKeySize(algorithm));
      expect(keypair.secretKey.bytes.length).toBe(secretKeySize(algorithm));

      const signature = sign(keypair.secretKey, message);
      expect(signature.bytes.length).toBe(signatureSize(algorithm));

      expect(() => verify(keypair.publicKey, message, signature)).not.toThrow();
      expect(() => verify(keypair.publicKey, message, signature.bytes)).not.toThrow();

      const badMessage = new TextEncoder().encode("Bad message!");
      expect(() => verify(keypair.publicKey, badMessage, signature)).toThrow(PqcError);

      const tampered = new Uint8Array(signature.bytes);
      tampered[10] ^= 0x01;
      expect(() => verify(keypair.publicKey, message, tampered)).toThrow(PqcError);
    });

    test("signWithRandomness is deterministic for the same inputs and verifies", () => {
      const keypair = generateKeyPair(algorithm, testBytes(128, 2));
      const rnd = testBytes(128, 3);
      const a = signWithRandomness(keypair.secretKey, message, rnd);
      const b = signWithRandomness(keypair.secretKey, message, rnd);
      expect(Buffer.from(a.bytes).equals(Buffer.from(b.bytes))).toBe(true);
      expect(() => verify(keypair.publicKey, message, a)).not.toThrow();

      const other = signWithRandomness(keypair.secretKey, message, testBytes(128, 4));
      expect(() => verify(keypair.publicKey, message, other)).not.toThrow();
    });

    test("rejects randomness shorter than 128 bytes", () => {
      const keypair = generateKeyPair(algorithm, testBytes(128, 5));
      expect(() => signWithRandomness(keypair.secretKey, message, testBytes(64, 6))).toThrow(PqcError);
    });
  });

  describe("SLH-DSA FIPS 205 mode", () => {
    test("a FIPS 205 signature verifies in FIPS 205 mode", () => {
      const message = new TextEncoder().encode("context-wrapped message");
      const keypair = generateKeyPair(Algorithm.SLH_DSA_SHAKE_128S, testBytes(128, 7));
      const signature = signWithRandomness(keypair.secretKey, message, testBytes(128, 8), { slhdsaFips205: true });
      expect(() => verify(keypair.publicKey, message, signature, { slhdsaFips205: true })).not.toThrow();
    });
  });

  describe("error conditions", () => {
    test("throws on an invalid algorithm", () => {
      expect(() => generateKeyPair(99 as Algorithm, testBytes(128, 9))).toThrow(PqcError);
    });
    test("throws on too little key-generation randomness", () => {
      expect(() => generateKeyPair(Algorithm.ML_DSA_44, testBytes(16, 10))).toThrow(PqcError);
    });
  });
});
