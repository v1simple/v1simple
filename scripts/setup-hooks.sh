#!/usr/bin/env bash
# Install the repository's fail-closed local privacy boundary. Run once per
# clone and again after moving or repairing a checkout.
#
#   ./scripts/setup-hooks.sh
#
# Uses core.hooksPath so the hooks live in version control (.githooks/) rather
# than in .git/hooks, which is not tracked and is lost on every fresh clone.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

chmod +x .githooks/* 2>/dev/null || true
git config --local core.hooksPath .githooks

# Never inherit or guess a personal identity in this public repository. The
# author.* and committer.* overrides also take precedence over a user's global
# settings; environment/command-line overrides are independently rejected by
# the hooks after Git resolves them.
git config --local user.useConfigOnly true
git config --local user.name v1simple
git config --local user.email noreply@example.invalid
git config --local author.name v1simple
git config --local author.email noreply@example.invalid
git config --local committer.name v1simple
git config --local committer.email noreply@example.invalid

# Fetching stays normal, but ordinary `git push`, IDE Sync, and Publish actions
# cannot contact GitHub. A reviewed publication must name the GitHub URL
# explicitly, which still invokes the tracked pre-push history scanner.
git config --local push.default nothing
if git remote get-url origin >/dev/null 2>&1; then
  git config --local remote.origin.pushurl /dev/null
fi

echo "hooks installed (core.hooksPath = .githooks)"
echo "  commit-msg → enforces <type>(<scope>): summary"
echo "  pre-commit → blocks unsafe identity/content, including replay and capture data"
echo "  prepare-commit-msg → repeats privacy checks even with --no-verify"
echo "  reference-transaction → blocks unsafe refs even when --no-verify is used"
echo "  pre-push   → scans new history; blocks unsafe metadata, refs, force-push, and deletion"
echo "  identity   → fixed to v1simple <noreply@example.invalid> in this checkout"
echo "  publishing → ordinary pushes and IDE Sync are disabled"
echo ""
python3 scripts/check_commit_msg.py --selftest
python3 scripts/check_public_commit_metadata.py --identity-only
python3 scripts/check_public_commit_metadata.py --revision=--all
python3 scripts/check_public_snapshot_privacy.py --all-history
python3 scripts/check_local_privacy_setup.py
echo ""
echo "To publish after an explicit review, run the full gate and use the GitHub URL"
echo "directly. Never change origin's push URL or use IDE Sync for this repository."
