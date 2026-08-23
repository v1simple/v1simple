# V1Simple Engineering

Keep V1Simple simple, in code and docs.

- This public repository is the complete source of engineering context.
- Process for the sake of process is NOT accepted. Every plan, artifact,
  abstraction, check, or report must directly deliver a requested product
  outcome or prove a necessary safety property; otherwise stop before building it.
- Run `./scripts/check_local_privacy_setup.py` at the start of every task and
  again before any commit or handoff. Stop if it fails; do not bypass, weaken,
  or treat `./scripts/ci-test.sh` as a substitute for this machine-local check.
- Ground claims first in the owning code, tests, and build output; use recorded bench and camera observations for physical and visual behavior. Mark unknowns; documentation does not override behavior.
- Keep private context, paths, data, and excerpts out of public files, logs, commits, and artifacts.
- Make the smallest coherent change and inspect the final diff for unrelated work or privacy leaks.
- Work on local `main` unless asked otherwise. Do not push.
- Never use `--no-verify` for a real commit or push, change the tracked hook
  path, or override the verified `origin` destination.
- Run proportionate checks while working and the full gate before handoff. Release changes also require production-build and artifact checks.
