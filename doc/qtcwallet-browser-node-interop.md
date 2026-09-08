# QTC Browser Wallet and Node Wallet Interop

QTC supports two wallet portability formats with different security goals:

- `.qtcwallet` wallet bundle: a plaintext JSON recovery file used to move a
  QTC browser wallet into a native `qtcd` descriptor wallet, or to export a
  single-seed native descriptor wallet for browser custody.
- `.bundle.qtc` native archive: an encrypted node-wallet backup produced by
  `backupwalletbundlearchive` for offline/operator recovery.

Do not treat these formats as interchangeable. A `.qtcwallet` file contains the
PQ master seed directly and must be handled like private-key material. A
`.bundle.qtc` archive is the preferred native backup format when exporting from
`qtcd`.

## Browser to Native Node

Use `restorewalletbundle` when the browser wallet exported a `.qtcwallet` or
`.qtcwallet.json` file and the destination should be a new native wallet:

```bash
qtc-cli restorewalletbundle "webwallet" "/secure/offline/qtc-wallet.qtcwallet.json" null true
```

For an encrypted restored wallet, pass the optional fifth argument:

```bash
qtc-cli restorewalletbundle \
  "webwallet" \
  "/secure/offline/qtc-wallet.qtcwallet.json" \
  null \
  true \
  "new native wallet passphrase"
```

Use `importwalletbundle` only when importing into an existing blank descriptor
wallet:

```bash
qtc-cli -rpcwallet="webwallet" importwalletbundle "/secure/offline/qtc-wallet.qtcwallet.json" true
```

If you need the manual fallback path advertised by the browser wallet, create a
blank descriptor wallet and pass the bundle's public descriptors plus its
`pq_master_seed` to `importdescriptors`:

```bash
qtc-cli createwallet "webwallet" false true "" false true

qtc-cli -rpcwallet="webwallet" importdescriptors \
  '[{"desc":"<receive descriptor>","timestamp":<birthday>,"active":true,"range":[0,100]},
    {"desc":"<change descriptor>","timestamp":<birthday>,"active":true,"internal":true,"range":[0,100]}]' \
  '[]' \
  '["<pq_master_seed hex>"]'
```

Both RPCs verify the bundle before installing keys:

- `format` must be `qtc-wallet-bundle`
- `version` must be `1`
- `network` must match the active chain
- `coin_type`, when present, must match the active chain
- `account`, when present, must be `0`
- `pq_master_seed` must be 32 bytes encoded as 64 hex characters
- `first_receive_address`, when present, must derive from that seed
- `descriptors`, when present, must match the seed, network, account, and
  receive/change branches

## Native Node to Browser

Native node wallets should be exported with the native archive RPC:

```bash
qtc-cli -rpcwallet="mywallet" \
  -stdinwalletpassphrase \
  -stdinbundlepassphrase \
  backupwalletbundlearchive /secure/offline/mywallet.bundle.qtc
```

Use `exportwalletbundle` only when you intentionally need a browser-compatible
plaintext `.qtcwallet` export:

```bash
qtc-cli -rpcwallet="mywallet" \
  exportwalletbundle "/secure/offline/mywallet.qtcwallet.json"
```

For encrypted wallets, either unlock the wallet first or pass the optional
wallet passphrase argument:

```bash
qtc-cli -rpcwallet="mywallet" \
  exportwalletbundle "/secure/offline/mywallet.qtcwallet.json" "wallet passphrase"
```

The export contains the PQ master seed in plaintext plus public receive/change
descriptors. It can spend funds. Prefer `backupwalletbundlearchive` for normal
native backups and use `exportwalletbundle` only for deliberate browser/node
interop or recovery handoff.

## WebAssembly Signing Core

The `src/libbitcoinpqc/wasm` target builds the same ML-DSA-44 and
SLH-DSA-SHAKE-128s signing core for browser and Node use. CI checks that
caller-supplied entropy produces byte-identical key material in native and WASM
builds. Website releases should consume the generated `qtc-pqc.js`,
`qtc-pqc.mjs`, and `qtc-pqc.wasm` artifacts rather than reimplementing PQ key
generation.
