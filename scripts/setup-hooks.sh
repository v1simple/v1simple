#!/usr/bin/env bash
# Point git at the repo's tracked hooks. Run once per clone.
#
#   ./scripts/setup-hooks.sh
#
# Uses core.hooksPath so the hooks live in version control (.githooks/) rather
# than in .git/hooks, which is not tracked and is lost on every fresh clone.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

chmod +x .githooks/* 2>/dev/null || true
git config core.hooksPath .githooks

echo "hooks installed (core.hooksPath = .githooks)"
echo "  commit-msg → enforces <type>(<scope>): summary"
echo "  pre-commit → blocks unsafe identity/content, including replay and capture data"
echo "  pre-push   → scans new history; blocks unsafe metadata, refs, force-push, and deletion"
echo ""
python3 scripts/check_commit_msg.py --selftest
echo ""
echo "Note: hooks are guardrails, not enforcement — 'git push --no-verify'"
echo "bypasses them. Server-side protection of 'main' is the real control."
