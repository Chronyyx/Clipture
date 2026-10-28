#!/usr/bin/env bash
# Finds CI's successful build of this commit for release.yml, waiting if CI
# is still running. Writes CI_RUN_ID to $GITHUB_ENV when that build can be
# published; leaves it empty when the release must build and test itself.
# Needs GH_TOKEN, GITHUB_REPOSITORY, GITHUB_SHA and actions: read.
set -uo pipefail
repo="$GITHUB_REPOSITORY"
sha="$GITHUB_SHA"
found=""
run_id=$(gh api "repos/$repo/actions/workflows/ci.yml/runs?head_sha=$sha&branch=main&event=push&per_page=1" --jq '.workflow_runs[0].id // empty')
if [ -z "$run_id" ]; then
  echo "CI has not run on $sha; this release builds and tests itself."
else
  if [ "$(gh api "repos/$repo/actions/runs/$run_id" --jq .status)" != completed ]; then
    echo "Waiting for CI run $run_id to finish"
    gh run watch "$run_id" -R "$repo" --interval 20 > /dev/null || true
  fi
  conclusion=$(gh api "repos/$repo/actions/runs/$run_id" --jq .conclusion)
  artifact=$(gh api "repos/$repo/actions/runs/$run_id/artifacts" --jq '.artifacts[] | select(.name == "release-build" and .expired == false) | .id')
  if [ "$conclusion" = success ] && [ -n "$artifact" ]; then
    echo "Using the build and tests of CI run $run_id"
    found="$run_id"
  else
    echo "CI run $run_id ended '$conclusion' without a usable build; this release builds and tests itself."
  fi
fi
echo "CI_RUN_ID=$found" >> "$GITHUB_ENV"
