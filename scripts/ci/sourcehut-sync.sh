#!/usr/bin/env bash
set -euo pipefail

git fetch \
  --force \
  --prune \
  origin \
  '+refs/heads/*:refs/sync/github/*'

if ! git for-each-ref \
  --format='%(refname)' \
  refs/sync/github/ |
  grep -q .; then
  echo "ERROR: GitHub branch fetch produced no branches" >&2
  exit 1
fi

echo "GitHub branches to synchronize:"

git for-each-ref \
  --format='  %(refname)' \
  refs/sync/github/

set -euo pipefail

count="${SOURCEHUT_SYNC_COUNT:?SOURCEHUT_SYNC_COUNT is not set}"

if [ "$count" -lt 1 ]; then
  echo "ERROR: no SourceHut repositories configured" >&2
  exit 1
fi

for ((i = 0; i < count; i++)); do
  remote="sourcehut-$i"
  url="$(git remote get-url "$remote")"

  echo
  echo "============================================================"
  echo "Force-syncing branches to:"
  echo "  Remote: $remote"
  echo "  URL:    $url"
  echo "============================================================"

  git push \
    --force \
    --prune \
    "$remote" \
    'refs/sync/github/*:refs/heads/*'

  echo "Successfully synchronized $remote"
done

echo
echo "============================================================"
echo "SourceHut branch synchronization completed successfully."
echo "============================================================"
