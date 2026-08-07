---
name: v1simple-engineering
description: Use for work that changes, validates, or reviews V1Simple behavior.
---

# V1Simple Engineering

1. Read `AGENTS.md`, inspect the working tree, and preserve unrelated changes.
2. Run `./scripts/check_local_privacy_setup.py` before inspecting or changing
   repository content. Stop if it fails; do not bypass or weaken the check.
3. Inspect the owning code, tests, build configuration, and applicable guards. Use bench or camera evidence for physical and visual behavior. State what remains unknown.
4. Plan and implement the smallest coherent change. Do not weaken a guard to obtain a pass.
5. Test the owning layer and add a focused regression test for a fixed defect when practical. Run broader checks in proportion to risk; use `./scripts/ci-test.sh` for the full gate. The full gate does not replace the machine-local privacy check.
6. For production or release changes, run the production build and artifact-validation checks used by the repository.
7. Run `./scripts/check_local_privacy_setup.py` again immediately before any
   commit or handoff, then review the final diff for behavior, scope, and
   privacy. Work on local `main` unless directed otherwise; do not push.
8. Report evidence used, checks run, and remaining risk.
