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

# Keep ordinary `git push` and IDE Sync usable, but make their destination
# deterministic. The pre-push hook independently verifies this exact public
# repository before scanning the proposed history range.
PUBLIC_REMOTE_HTTPS="https://github.com/v1simple/v1simple.git"
origin_url="$(git remote get-url origin 2>/dev/null || true)"
case "$origin_url" in
  "$PUBLIC_REMOTE_HTTPS")
    ;;
  *)
    echo "ERROR: origin is missing or is not the approved public repository." >&2
    echo "Fix origin deliberately, then run this setup again." >&2
    exit 1
    ;;
esac

git config --local push.default simple
git config --local push.followTags false
git config --local remote.pushDefault origin
git config --local branch.main.remote origin
git config --local branch.main.pushRemote origin
git config --local branch.main.merge refs/heads/main
git config --local --unset-all remote.origin.pushurl >/dev/null 2>&1 || true

echo "hooks installed (core.hooksPath = .githooks)"
echo "  commit-msg → enforces <type>(<scope>): summary"
echo "  pre-commit → blocks unsafe identity/content, including replay and capture data"
echo "  prepare-commit-msg → repeats privacy checks even with --no-verify"
echo "  reference-transaction → blocks unsafe refs even when --no-verify is used"
echo "  pre-push   → scans new history; blocks unsafe metadata, refs, force-push, and deletion"
echo "  identity   → fixed to v1simple <noreply@example.invalid> in this checkout"
echo "  publishing → ordinary push and IDE Sync use the verified pre-push gate"
echo ""
python3 scripts/check_commit_msg.py --selftest
python3 scripts/check_public_commit_metadata.py --identity-only
python3 scripts/check_public_commit_metadata.py --revision=--all
python3 scripts/check_public_snapshot_privacy.py --all-history
python3 scripts/check_local_privacy_setup.py
echo ""
echo "Normal git push and IDE Sync are ready after a clean full gate."
echo "Never use --no-verify or override the verified origin configuration."
