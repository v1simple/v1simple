---
name: v1simple-engineering
description: Use for work that changes, validates, or reviews V1Simple behavior.
---

# V1Simple Engineering

1. Read `AGENTS.md`, inspect the working tree, and preserve unrelated changes.
2. Inspect the owning code, tests, build configuration, and applicable guards. Use bench or camera evidence for physical and visual behavior. State what remains unknown.
3. Plan and implement the smallest coherent change. Do not weaken a guard to obtain a pass.
4. Test the owning layer and add a focused regression test for a fixed defect when practical. Run broader checks in proportion to risk; use `./scripts/ci-test.sh` for the full gate.
5. For production or release changes, run the production build and artifact-validation checks used by the repository.
6. Review the final diff for behavior, scope, and privacy. Work on local `main` unless directed otherwise; do not push.
7. Report evidence used, checks run, and remaining risk.
