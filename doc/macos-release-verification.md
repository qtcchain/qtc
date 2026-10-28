# Verifying a macOS release

Every QTC macOS release ships as a disk image (`qtc-<version>-<arch>-apple-darwin.dmg`) and as the same signed tree in
`-signed.zip` / `-signed.tar.gz` archives. The executables inside (`qtcd`, `qtc-cli`, `qtc-qt`, `qtc-wallet`, ...) are
signed with an Apple Developer ID Application certificate (team `9FRYT84V8P`) with the hardened runtime and a secure
timestamp, and submitted to Apple's notary service. The notarization ticket is stapled to the `.dmg`. No `.pkg`
installer is published.

Two independent things back a download:

1. **The PGP-signed `SHA256SUMS`** says which bytes the QTC release key published. This is what proves the file is the
   one the project released. Check it first (see [Install a release](/README.md#install-a-release)).
2. **Apple's code signature and notarization** say the executables inside were not modified after signing and were
   scanned by Apple. This is what macOS itself checks.

## One command

```bash
contrib/macdeploy/verify-macos-release.sh --sums SHA256SUMS qtc-0.2.5-arm64-apple-darwin.dmg
```

It accepts the `.dmg`, either archive, or an unpacked directory. It never runs anything from the artifact: the image is
attached read-only and archives are unpacked into a temporary directory that is removed on exit. Exit status is 0 when
every check passes, 1 when one fails, 2 for a usage error. `--offline` skips the checks that need Apple's servers.

For the `.dmg` it checks the image checksum, its signature, that it is signed by the expected Developer ID team, that
the notarization ticket is stapled, and that Gatekeeper accepts it. For every Mach-O file inside it checks that the
signature verifies, the signer, the team ID, the hardened runtime, the secure timestamp and Apple's notarization record
for that exact code.

The checks it runs by hand, if you prefer:

```bash
shasum -a 256 qtc-0.2.5-arm64-apple-darwin.dmg                    # compare with the signed SHA256SUMS
codesign --verify --strict --verbose=4 /Applications/qtc-0.2.5/bin/qtcd
codesign -dvvv /Applications/qtc-0.2.5/bin/qtcd                    # Authority=Developer ID Application ... TeamIdentifier=9FRYT84V8P
codesign --verify --strict --test-requirement='=notarized' /Applications/qtc-0.2.5/bin/qtcd   # needs network
xcrun stapler validate qtc-0.2.5-arm64-apple-darwin.dmg
spctl -a -vv -t open --context context:primary-signature qtc-0.2.5-arm64-apple-darwin.dmg
```

## Checks that are expected to fail on a bare executable

The release contains command-line executables, not an application bundle. Three common checks therefore do not apply,
and a "fail" from them says nothing about the signature:

| Command | What it prints for `qtcd` | Why |
| --- | --- | --- |
| `spctl -a -vv -t exec <file>` | `rejected (the code is valid but does not seem to be an app)` | `spctl` assesses application bundles. It prints the same line for Apple's own `/bin/ls`. The text says the code is valid. |
| `xcrun stapler validate <file>` | `does not have a ticket stapled to it` | Apple cannot staple a ticket to a bare executable. The ticket is stapled to the `.dmg`. |
| `syspolicy_check distribution <file>` | `Notary Ticket Missing` | Same reason. Run it on the `.dmg` instead: it passes. |

Because there is no stapled ticket on the executable itself, macOS can look the notarization record up online the
first time it assesses a quarantined copy (a copy that came from a downloaded image or archive). Open the files for the
first time while the Mac has a network connection.

## If `codesign --verify` fails

```text
qtcd: invalid signature (code or signature have been modified)
```

means the bytes of that file differ from the bytes that were signed. A correctly downloaded and unpacked release does
not do this. Typical causes are a file changed after it was unpacked (a "strip" or "relink" step, a security product or
sync tool that rewrites executables) or a download that was not byte-identical.

1. Re-download the `.dmg`, confirm its SHA-256 against the PGP-signed `SHA256SUMS`, and run the script on the `.dmg`
   itself, before copying anything out of it.
2. Copy with `cp -R` or `ditto`, or unpack with `tar -xzf`, `unzip` or `ditto -x -k`. All of them keep the signature.
3. If the script passes on the image but fails on your installed copy, compare `shasum -a 256` of the installed file with
   the one in the image. A difference means the installed copy was modified; reinstall it from the image.
4. Do not run files that fail. Send the full output of the script, the macOS version and how the files were installed to
   `security@qtc.dev`.

Do not turn Gatekeeper off, and do not remove the quarantine attribute to get around a failed check.
