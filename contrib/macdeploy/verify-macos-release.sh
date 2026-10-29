#!/usr/bin/env bash
# Copyright (c) 2026 The QTC developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Verify the Apple code signature, notarization and (optionally) the SHA-256 of a QTC macOS release artifact.
# macOS only. Nothing from the artifact is ever executed: DMGs are attached read-only and archives are unpacked
# into a private temporary directory that is removed on exit. Works with the stock /bin/bash 3.2.
#
# Usage: verify-macos-release.sh [--sums SHA256SUMS] [--team-id ID] [--offline] <artifact>
#
#   <artifact>   qtc-<ver>-<arch>-apple-darwin.dmg
#                qtc-<ver>-<arch>-apple-darwin-signed.zip | .tar.gz
#                or a directory that already holds an unpacked qtc-<ver>/ tree
#   --sums       the release's SHA256SUMS (verify its PGP signature first: contrib/verify-binaries/verify.py, or
#                `gpg --verify SHA256SUMS.asc SHA256SUMS`). The artifact's SHA-256 must be listed in it.
#   --team-id    expected Apple Team ID of the signer (default: the QTC release signer, 9FRYT84V8P)
#   --offline    skip the checks that ask Apple's servers (notarization requirement); everything else still runs
#
# What is checked, and why these checks and not `spctl -t exec` or `stapler validate` on a binary:
#   * `spctl -a -t exec` assesses application BUNDLES. For a bare command-line Mach-O such as qtcd it answers
#     "rejected (the code is valid but does not seem to be an app)" for every such file, Apple's own /bin/ls included,
#     so it cannot be used as a pass/fail test here. The same goes for `stapler validate` on a bare binary: Apple cannot
#     staple a ticket to one. The ticket is stapled to the .dmg instead, which is what is assessed below.
#   * For each Mach-O: `codesign --verify --strict` (signature intact, nothing modified since signing), a Developer ID
#     Application chain, the expected Team ID, hardened runtime, a secure timestamp, and the `notarized` code
#     requirement (Apple's online notarization lookup by code-directory hash).
#   * For a .dmg: image checksum, its own signature, stapled ticket, and Gatekeeper's `open` assessment.
# Exit status: 0 all checks passed, 1 at least one failed, 2 usage or environment error.

export LC_ALL=C
set -u

TEAM_ID=9FRYT84V8P
SUMS=
OFFLINE=0
usage() { awk '/^# Usage:/{p=1} /^# What is checked/{p=0} p{sub(/^# ?/,""); print}' "$0"; }
die() { echo "error: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    --sums)    [ $# -ge 2 ] || die "--sums needs a file"; SUMS=$2; shift ;;
    --team-id) [ $# -ge 2 ] || die "--team-id needs a value"; TEAM_ID=$2; shift ;;
    --offline) OFFLINE=1 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) die "unknown option $1" ;;
    *) break ;;
  esac
  shift
done
[ $# -eq 1 ] || { usage >&2; exit 2; }
ART=$1
[ "$(uname -s)" = Darwin ] || die "this script needs macOS (codesign, hdiutil, stapler)"
for t in codesign hdiutil file shasum xcrun; do command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"; done
[ -e "$ART" ] || die "no such file or directory: $ART"
[ -z "$SUMS" ] || [ -f "$SUMS" ] || die "no such file: $SUMS"

W=$(mktemp -d "${TMPDIR:-/tmp}/qtc-verify.XXXXXX") || die "cannot create a temporary directory"
MNT=
cleanup() {
  [ -z "$MNT" ] || hdiutil detach -quiet "$MNT" >/dev/null 2>&1 || hdiutil detach -quiet -force "$MNT" >/dev/null 2>&1
  rm -rf "$W"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS  $*"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $*"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP  $*"; }
# check <label> <cmd...>: run quietly, keep the first lines of stderr/stdout for the report on failure
check() {
  local label=$1 out; shift
  if out=$("$@" 2>&1); then ok "$label"; else bad "$label"; printf '%s\n' "$out" | sed -n '1,4s/^/          /p'; fi
}

echo "artifact: $ART"

# ---- 1. SHA-256 against the (separately verified) SHA256SUMS ----
if [ -n "$SUMS" ] && [ -f "$ART" ]; then
  echo "SHA-256"
  base=$(basename "$ART")
  have=$(shasum -a 256 "$ART" | awk '{print $1}')
  # SHA256SUMS names each file under its build directory ("apple-darwin-signed/qtc-...dmg"): match on the basename.
  want=$(awk -v b="$base" '{n=$2; sub(/^\*/,"",n); sub(/^.*\//,"",n); if (n==b) print $1}' "$SUMS")
  if [ -z "$want" ]; then bad "$base is not listed in $SUMS"
  elif [ "$want" = "$have" ]; then ok "sha256 $have matches the list"
  else bad "sha256 mismatch: file $have, list $want"; fi
elif [ -n "$SUMS" ]; then
  echo "SHA-256"; skip "--sums given, but $ART is a directory"
fi

# ---- 2. get a tree of Mach-O files to inspect (never executed) ----
TREE=
if [ -d "$ART" ]; then
  TREE=$ART
else
  case "$ART" in
    *.dmg)
      echo "Disk image"
      check "image checksum (hdiutil verify)" hdiutil verify -quiet "$ART"
      check "image code signature (codesign --verify --strict)" codesign --verify --strict "$ART"
      sig=$(codesign -dvvv "$ART" 2>&1)
      case "$sig" in *"Authority=Developer ID Application"*"TeamIdentifier=$TEAM_ID"*) ok "signed by Developer ID Application, team $TEAM_ID" ;;
        *) bad "image is not signed by a Developer ID Application identity of team $TEAM_ID"; printf '%s\n' "$sig" | grep -E 'Authority|TeamIdentifier|Signature' | sed 's/^/          /' ;; esac
      check "notarization ticket stapled (stapler validate)" xcrun stapler validate "$ART"
      if [ "$OFFLINE" = 1 ]; then skip "Gatekeeper open assessment (needs Apple's servers)"
      else check "Gatekeeper accepts the image (spctl -t open)" spctl -a -t open --context context:primary-signature "$ART"; fi
      MNT="$W/mnt"; mkdir -p "$MNT"
      if hdiutil attach -quiet -readonly -nobrowse -noautoopen -mountpoint "$MNT" "$ART" >"$W/attach.out" 2>&1; then TREE=$MNT
      else MNT=; bad "cannot attach the image read-only (is it already mounted? eject it and retry)"; sed -n '1,3s/^/          /p' "$W/attach.out"; fi ;;
    *.zip)
      mkdir -p "$W/x"
      if command -v ditto >/dev/null 2>&1 && ditto -x -k "$ART" "$W/x" 2>/dev/null; then TREE=$W/x; else bad "cannot unpack $ART"; fi ;;
    *.tar.gz|*.tgz)
      mkdir -p "$W/x"
      if tar -C "$W/x" -xzf "$ART" 2>/dev/null; then TREE=$W/x; else bad "cannot unpack $ART"; fi ;;
    *) die "unsupported artifact type (want .dmg, .zip, .tar.gz or a directory): $ART" ;;
  esac
fi

# ---- 3. every Mach-O in the tree ----
if [ -n "$TREE" ]; then
  echo "Executables in $(basename "$ART")"
  n=0
  while IFS= read -r -d '' f; do
    file -b "$f" | grep -q 'Mach-O' || continue
    n=$((n+1)); rel=${f#"$TREE"/}
    info=$(codesign -dvvv "$f" 2>&1)
    if ! codesign --verify --strict "$f" >"$W/v.out" 2>&1; then
      bad "$rel: signature does not verify"; sed -n '1,3s/^/          /p' "$W/v.out"; continue
    fi
    problems=
    case "$info" in *"Authority=Developer ID Application"*) ;; *) problems="$problems not-Developer-ID;" ;; esac
    case "$info" in *"TeamIdentifier=$TEAM_ID"*) ;; *) problems="$problems team-id-is-not-$TEAM_ID;" ;; esac
    case "$info" in *"flags=0x"*"(runtime)"*) ;; *) problems="$problems no-hardened-runtime;" ;; esac
    case "$info" in *"Timestamp="*) ;; *) problems="$problems no-secure-timestamp;" ;; esac
    if [ "$OFFLINE" = 0 ] && ! codesign --verify --strict --test-requirement='=notarized' "$f" >/dev/null 2>&1; then
      problems="$problems not-notarized-or-Apple-unreachable;"
    fi
    if [ -n "$problems" ]; then bad "$rel:$problems"; else ok "$rel  $(printf '%s\n' "$info" | sed -n 's/^CDHash=/cdhash /p')"; fi
  done < <(find "$TREE" -type f -print0)
  [ "$n" -gt 0 ] || bad "no Mach-O executables found in $ART"
  [ "$OFFLINE" = 0 ] || skip "notarization lookup for each executable (--offline)"
fi

echo
echo "result: $PASS passed, $FAIL failed, $SKIP skipped"
if [ "$FAIL" -gt 0 ]; then
  echo "At least one check failed. Re-download the artifact, confirm its SHA-256 against the PGP-signed SHA256SUMS,"
  echo "and do not run files that fail. Report the output above to security@qtc.dev."
  exit 1
fi
exit 0
