#!/usr/bin/env bash
# Apply the QTC repository rulesets so that nothing reaches `main` except through a pull request.
#
# GitHub only allows branch rulesets on public repositories or paid plans, so run this in the same
# minute the repository is made public:
#
#   gh repo edit qtcchain/qtc --visibility public --accept-visibility-change-consequences
#   contrib/devtools/github-protect-main.sh
#
# Idempotent: re-running updates the existing rulesets in place. Requires `gh` authenticated as an admin.
set -euo pipefail
REPO="${QTC_GITHUB_REPO:-qtcchain/qtc}"

# Required status checks: exact check names of jobs that run on pull_request. Add the readiness matrix
# entries once their names are stable; the fuzz build is excluded until it is green on main.
REQUIRED_CHECKS='[{"context":"centos native container"},{"context":"Build and verify qtc-pqc.wasm"}]'

main_ruleset=$(cat <<JSON
{
  "name": "protect-main",
  "target": "branch",
  "enforcement": "active",
  "bypass_actors": [],
  "conditions": { "ref_name": { "include": ["~DEFAULT_BRANCH"], "exclude": [] } },
  "rules": [
    { "type": "deletion" },
    { "type": "non_fast_forward" },
    { "type": "required_linear_history" },
    { "type": "pull_request",
      "parameters": {
        "required_approving_review_count": 0,
        "dismiss_stale_reviews_on_push": true,
        "require_code_owner_review": false,
        "require_last_push_approval": false,
        "required_review_thread_resolution": true,
        "allowed_merge_methods": ["squash", "rebase", "merge"] } },
    { "type": "required_status_checks",
      "parameters": {
        "strict_required_status_checks_policy": true,
        "do_not_enforce_on_create": false,
        "required_status_checks": ${REQUIRED_CHECKS} } }
  ]
}
JSON
)

tag_ruleset=$(cat <<'JSON'
{
  "name": "protect-release-tags",
  "target": "tag",
  "enforcement": "active",
  "bypass_actors": [],
  "conditions": { "ref_name": { "include": ["refs/tags/v*"], "exclude": [] } },
  "rules": [ { "type": "deletion" }, { "type": "update" }, { "type": "non_fast_forward" } ]
}
JSON
)

apply() {
  local name="$1" body="$2"
  local id
  id=$(gh api "repos/${REPO}/rulesets" --jq ".[] | select(.name==\"${name}\") | .id" 2>/dev/null || true)
  if [ -n "${id}" ]; then
    gh api -X PUT "repos/${REPO}/rulesets/${id}" --input - <<<"${body}" >/dev/null && echo "updated ruleset ${name} (${id})"
  else
    gh api -X POST "repos/${REPO}/rulesets" --input - <<<"${body}" >/dev/null && echo "created ruleset ${name}"
  fi
}

apply protect-main "${main_ruleset}"
apply protect-release-tags "${tag_ruleset}"

echo "--- verification"
gh api "repos/${REPO}/rulesets" --jq '.[] | "\(.name): \(.enforcement) target=\(.target)"'
gh api "repos/${REPO}/rules/branches/main" --jq '.[] | .type' | sort | tr '\n' ' '; echo
echo "Direct pushes to main (including by admins) are now rejected; changes must arrive by pull request."
