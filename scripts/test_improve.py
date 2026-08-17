#!/usr/bin/env python3
"""Regression tests for the fail-closed Phase-B improvement controller."""

from __future__ import annotations

import hashlib
import json
import math
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]

import improve  # noqa: E402


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_error(callable_object, error_type: type[BaseException], text: str) -> None:
    try:
        callable_object()
    except error_type as exc:
        assert_true(text in str(exc), f"unexpected error: {exc}")
    else:
        raise AssertionError(f"expected {error_type.__name__} containing {text!r}")


def test_counterbalanced_schedule() -> None:
    for runs in (5, 6, 7, 10):
        schedule = improve.counterbalanced_schedule(runs)
        assert_true(len(schedule) == 2 * runs, "schedule length changed")
        assert_true(schedule.count("baseline") == runs, "baseline arm is unbalanced")
        assert_true(schedule.count("candidate") == runs, "candidate arm is unbalanced")
        assert_true(schedule[0] == "baseline", "experiment must establish base first")
        assert_true(schedule[-1] == "candidate", "accepted candidate must remain installed")
        assert_true(
            max(len(list(group)) for _, group in __import__("itertools").groupby(schedule)) <= 2,
            "schedule permits a long time-confounded arm block",
        )
    expect_error(lambda: improve.counterbalanced_schedule(4), improve.InvalidInput, "at least 5")


def test_improvement_rule() -> None:
    lower = improve.decide_improvement([100, 101, 99, 100, 102], [90, 91, 92, 93, 94], "lower_better", 5)
    assert_true(lower["accepted"] is True and lower["separation_gap"] == 5, "clear lower result rejected")
    higher = improve.decide_improvement([10, 11, 9, 10, 12], [20, 21, 22, 19, 20], "higher_better", 5)
    assert_true(higher["accepted"] is True and higher["separation_gap"] == 7, "clear higher result rejected")
    overlap = improve.decide_improvement([100, 101, 99, 100, 102], [90, 91, 100, 93, 94], "lower_better", 5)
    assert_true(overlap["accepted"] is False, "one overlapping candidate outlier was accepted")
    equal = improve.decide_improvement([100] * 5, [100] * 5, "lower_better", 5)
    assert_true(equal["accepted"] is False and equal["separation_gap"] == 0, "equality was accepted")
    expect_error(
        lambda: improve.decide_improvement([1] * 5, [2] * 4, "lower_better", 5),
        improve.InvalidInput,
        "exactly N",
    )
    expect_error(
        lambda: improve.decide_improvement([1] * 5, [2, 2, 2, 2, math.inf], "lower_better", 5),
        improve.InvalidInput,
        "finite",
    )
    expect_error(
        lambda: improve.decide_improvement([1] * 5, [2, 2, 2, 2, True], "lower_better", 5),
        improve.InvalidInput,
        "non-boolean",
    )


def test_candidate_scope() -> None:
    improve.validate_candidate_paths(["src/display_frequency.cpp", "test/test_display.cpp"])
    improve.validate_candidate_paths(["src/modules/display/display_pipeline_module.cpp"])
    for paths, expected in (
        (["README.md", "src/display_frequency.cpp"], "display implementation allowlist"),
        (["test/test_display.cpp"], "eligible display implementation"),
        (["src/perf_metrics.cpp"], "display implementation allowlist"),
        (["src/main_loop_wiring.cpp"], "display implementation allowlist"),
        (["src/modules/display/display_orchestration_module.cpp"], "display implementation allowlist"),
        (["include/psram_alloc_compat.h"], "display implementation allowlist"),
        (["include/esp_timer.h"], "display implementation allowlist"),
        (["src/../tools/bench_score.py"], "unsafe"),
    ):
        expect_error(lambda paths=paths: improve.validate_candidate_paths(paths), improve.InvalidInput, expected)


def test_serial_port_and_live_plan_validation() -> None:
    assert_true(
        improve.validate_serial_port("/dev/cu.fixture", require_exists=False) == "/dev/cu.fixture",
        "canonical serial port changed",
    )
    for bad in ("cu.fixture", "/tmp/cu.fixture", "/dev/disk1", "/dev/cu.bad port", "--upload-port"):
        expect_error(
            lambda bad=bad: improve.validate_serial_port(bad, require_exists=False),
            improve.InvalidInput,
            "serial port",
    )
    with tempfile.TemporaryDirectory() as temp:
        session = (Path(temp) / "sessions" / "fixture").resolve()
        plan = {
            "schema_version": improve.SCHEMA_VERSION,
            "kind": "improve_plan",
            "simulated": False,
            "session_dir": str(session),
            "source_root": str(improve.ROOT),
            "source_snapshot": {"head": "a" * 40, "status": "", "branch": "main"},
            "base_sha": "a" * 40,
            "candidate_sha": "b" * 40,
            "candidate_branch": "candidate",
            "evaluation_branch": "improve/fixture/candidate",
            "base_worktree": str(session / "worktrees" / "base"),
            "candidate_worktree": str(session / "worktrees" / "candidate"),
            "changed_paths": ["src/display_frequency.cpp"],
            "runs_per_arm": improve.MIN_RUNS,
            "schedule": improve.counterbalanced_schedule(improve.MIN_RUNS),
            "target": {
                "suite": "replay",
                "metric": "disp_pipe_p95_us",
                "direction": "lower_better",
                "catalog_sha256": "c" * 64,
            },
            "board_id": improve.DEFAULT_BOARD_ID,
            "env": improve.DEFAULT_ENV,
            "port": "/dev/cu.fixture",
            "bench_contract": {
                "all_suites": True,
                "camera": True,
                "duration_seconds": improve.RUN_DURATION_SECONDS,
                "replay_duration_seconds": improve.REPLAY_DURATION_SECONDS,
                "profile": improve.PROFILE,
                "segment": improve.SEGMENT,
                "blink_profile": improve.BLINK_PROFILE,
                "baseline_comparison": False,
                "upload_during_bench": False,
            },
            "post_flash_settle_seconds": improve.DEFAULT_SETTLE_SECONDS,
            "controller_components": {
                "scripts/improve.py": "d" * 64,
                "scripts/improve_git_dryrun.py": "e" * 64,
            },
            "controller_sha256": improve.sha256_bytes(
                improve.canonical_bytes(
                    {
                        "scripts/improve.py": "d" * 64,
                        "scripts/improve_git_dryrun.py": "e" * 64,
                    }
                )
            ),
            "dry_run_report_sha256": "f" * 64,
        }
        improve.validate_live_plan(plan, session, require_port=False)
        historical_base = dict(plan)
        historical_base["base_sha"] = "9" * 40
        expect_error(
            lambda: improve.validate_live_plan(historical_base, session, require_port=False),
            improve.InvalidInput,
            "clean invoking HEAD",
        )
        display_target = dict(plan)
        display_target["target"] = {**plan["target"], "suite": "display"}
        expect_error(
            lambda: improve.validate_live_plan(display_target, session, require_port=False),
            improve.InvalidInput,
            "frozen Phase-B contract",
        )
        escaped = dict(plan)
        escaped["candidate_worktree"] = str(Path(temp) / "elsewhere")
        expect_error(
            lambda: improve.validate_live_plan(escaped, session, require_port=False),
            improve.InvalidInput,
            "escapes",
        )
        weakened = dict(plan)
        weakened["bench_contract"] = {**plan["bench_contract"], "camera": False}
        expect_error(
            lambda: improve.validate_live_plan(weakened, session, require_port=False),
            improve.InvalidInput,
            "bench contract",
        )
        session.mkdir(parents=True)
        outside = Path(temp) / "outside-worktrees"
        outside.mkdir()
        (session / "worktrees").symlink_to(outside, target_is_directory=True)
        expect_error(
            lambda: improve.validate_live_plan(plan, session, require_port=False),
            improve.InvalidInput,
            "must not traverse a symlink",
        )


def test_ambient_git_and_platformio_overrides_fail_closed() -> None:
    ambient_unsafe = {
        key: value
        for key, value in os.environ.items()
        if key.startswith(("GIT_", "PLATFORMIO_", "BENCH_", "BASH_FUNC_"))
        or key in improve.UNSAFE_PRODUCT_ENVIRONMENT
        or key in improve.UNSAFE_SHELL_ENVIRONMENT
    }
    for key in ambient_unsafe:
        os.environ.pop(key, None)
    try:
        for key, value, expected in (
            ("GIT_CONFIG_PARAMETERS", "'core.hooksPath'='/dev/null'", "Git repository selectors"),
            ("BASH_ENV", "/tmp/untrusted-bash-env", "child shell controls"),
            ("BASH_FUNC_exit%%", "() { :; }", "child shell controls"),
            ("SHELLOPTS", "xtrace", "child shell controls"),
            ("PS4", "untrusted-trace-prefix", "child shell controls"),
            ("ENV", "/tmp/untrusted-shell-env", "child shell controls"),
            ("PLATFORMIO_UPLOAD_FLAGS", "--erase-all", "PlatformIO/build overrides"),
            ("PLATFORMIO_BUILD_FLAGS", "-D V1SIMPLE_INJECTED=1", "PlatformIO/build overrides"),
            ("PIO_CMD", "/tmp/untrusted-pio", "PlatformIO/build overrides"),
            ("BENCH_BOARD_ID", "../../outside", "bench/camera overrides"),
        ):
            os.environ[key] = value
            try:
                expect_error(improve.validate_git_environment, improve.InvalidInput, expected)
                sanitized = improve.sanitized_product_environment()
                assert_true(key not in sanitized, f"unsafe child environment survived: {key}")
                assert_true(
                    key not in improve.sanitized_git_environment(),
                    f"unsafe Git child environment survived: {key}",
                )
            finally:
                os.environ.pop(key, None)
    finally:
        os.environ.update(ambient_unsafe)

    prior_trace = os.environ.get("GIT_TRACE")
    os.environ["GIT_TRACE"] = "/tmp/phase-b-must-not-write-trace"
    try:
        assert_true(
            "GIT_TRACE" not in improve.sanitized_git_environment(),
            "ambient Git trace output survived sanitization",
        )
        assert_true(
            "GIT_TRACE" not in improve.sanitized_product_environment(),
            "ambient Git trace output survived product sanitization",
        )
    finally:
        if prior_trace is None:
            os.environ.pop("GIT_TRACE", None)
        else:
            os.environ["GIT_TRACE"] = prior_trace


def test_sanitized_git_environment_cannot_bypass_a_tracked_bash_hook() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        subprocess.run(["git", "init", "-q", "-b", "main", str(root)], check=True)
        subprocess.run(["git", "config", "user.name", "Phase B Test"], cwd=root, check=True)
        subprocess.run(
            ["git", "config", "user.email", "phase-b@example.invalid"], cwd=root, check=True
        )
        hooks = root / ".githooks"
        hooks.mkdir()
        reference_hook = hooks / "reference-transaction"
        reference_hook.write_text("#!/usr/bin/env bash\nexit 99\n", encoding="utf-8")
        reference_hook.chmod(0o755)
        tracked = root / "tracked.txt"
        tracked.write_text("base\n", encoding="utf-8")
        subprocess.run(
            ["git", "add", "--", ".githooks/reference-transaction", "tracked.txt"],
            cwd=root,
            check=True,
        )
        subprocess.run(
            [
                "git",
                "-c",
                "core.hooksPath=/dev/null",
                "commit",
                "-q",
                "-m",
                "base",
            ],
            cwd=root,
            check=True,
        )
        subprocess.run(["git", "config", "core.hooksPath", ".githooks"], cwd=root, check=True)
        bash_env = root / "bash-env"
        bash_env.write_text("exit 0\n", encoding="utf-8")
        for key, value in (
            ("BASH_ENV", str(bash_env)),
            ("BASH_FUNC_exit%%", "() { :; }"),
        ):
            prior = os.environ.get(key)
            os.environ[key] = value
            try:
                improve.assert_repository_hook_contract(root)
                expect_error(
                    lambda: improve.run_capture(
                        ["git", "commit", "--allow-empty", "-m", "must-be-blocked"],
                        cwd=root,
                    ),
                    improve.InvalidInput,
                    "command failed",
                )
            finally:
                if prior is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = prior


def test_repository_fsmonitor_cannot_hide_a_dirty_worktree() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        subprocess.run(["git", "init", "-q", "-b", "main", str(root)], check=True)
        subprocess.run(["git", "config", "user.name", "Phase B Test"], cwd=root, check=True)
        subprocess.run(
            ["git", "config", "user.email", "phase-b@example.invalid"], cwd=root, check=True
        )
        tracked = root / "tracked.txt"
        tracked.write_text("base\n", encoding="utf-8")
        subprocess.run(["git", "add", "--", "tracked.txt"], cwd=root, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "base"], cwd=root, check=True)
        fsmonitor = root / "false-clean-fsmonitor"
        fsmonitor.write_text("#!/bin/sh\nprintf 'token\\n'\n", encoding="utf-8")
        fsmonitor.chmod(0o755)
        subprocess.run(
            ["git", "config", "core.fsmonitor", str(fsmonitor)], cwd=root, check=True
        )
        subprocess.run(
            ["git", "status", "--porcelain=v1", "-uall"],
            cwd=root,
            check=True,
            stdout=subprocess.DEVNULL,
        )
        tracked.write_text("modified\n", encoding="utf-8")
        controlled = improve.controlled_git_argv(
            ["git", "status", "--porcelain=v1", "-uall"]
        )
        assert_true(
            controlled[1:3] == ["-c", "core.fsmonitor=false"],
            "controller Git command did not disable the repository fsmonitor",
        )
        assert_true(
            "tracked.txt" in improve.source_snapshot(root)["status"],
            "repository fsmonitor hid a dirty tracked file from the controller",
        )


def test_checkout_filter_cannot_hide_changed_tracked_bytes() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        source = root / "source"
        subprocess.run(["git", "init", "-q", "-b", "main", str(source)], check=True)
        subprocess.run(["git", "config", "user.name", "Phase B Test"], cwd=source, check=True)
        subprocess.run(
            ["git", "config", "user.email", "phase-b@example.invalid"],
            cwd=source,
            check=True,
        )
        tracked = source / "src" / "display_arrow.cpp"
        tracked.parent.mkdir()
        tracked.write_text("CANDIDATE\n", encoding="utf-8")
        subprocess.run(["git", "add", "--", "src/display_arrow.cpp"], cwd=source, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "candidate"], cwd=source, check=True)
        commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source, text=True
        ).strip()

        filter_script = root / "filter.py"
        filter_script.write_text(
            """import sys
data = sys.stdin.buffer.read()
marker = b'INJECTED_BY_SMUDGE\\n'
if sys.argv[1] == 'smudge':
    sys.stdout.buffer.write(data + marker)
else:
    sys.stdout.buffer.write(data.replace(marker, b''))
""",
            encoding="utf-8",
        )
        command = f"{sys.executable} {filter_script}"
        subprocess.run(
            ["git", "config", "filter.phaseb.smudge", f"{command} smudge"],
            cwd=source,
            check=True,
        )
        subprocess.run(
            ["git", "config", "filter.phaseb.clean", f"{command} clean"],
            cwd=source,
            check=True,
        )
        info = source / ".git" / "info" / "attributes"
        info.write_text("src/display_arrow.cpp filter=phaseb\n", encoding="utf-8")
        evaluation = root / "evaluation"
        subprocess.run(
            ["git", "worktree", "add", "-q", "--detach", str(evaluation), commit],
            cwd=source,
            check=True,
        )
        evaluated = evaluation / "src" / "display_arrow.cpp"
        assert_true(
            "INJECTED_BY_SMUDGE" in evaluated.read_text(encoding="utf-8"),
            "fixture did not alter checkout bytes through the common attributes filter",
        )
        assert_true(
            subprocess.check_output(
                ["git", "status", "--porcelain=v1", "-uall"],
                cwd=evaluation,
                text=True,
            ).strip()
            == "",
            "fixture did not reproduce Git's false-clean filtered worktree",
        )
        expect_error(
            lambda: improve.verify_worktree_matches_commit(evaluation, commit),
            improve.GateFailure,
            "tracked raw bytes differ",
        )


def test_host_wide_state_paths_ignore_ambient_home() -> None:
    with tempfile.TemporaryDirectory() as temp:
        environment = os.environ.copy()
        environment["HOME"] = temp
        code = (
            "import json,sys;"
            f"sys.path.insert(0,{str(ROOT / 'scripts')!r});"
            f"sys.path.insert(0,{str(ROOT / 'scripts' / 'bench')!r});"
            "import improve,run_window;"
            "print(json.dumps([str(improve.MANAGED_V1_RADIO_LEASE_PATH),"
            "str(improve.ACCOUNT_HOME),str(run_window.V1_RADIO_LEASE_PATH)]))"
        )
        output = subprocess.check_output(
            [sys.executable, "-c", code],
            cwd=ROOT,
            env=environment,
            text=True,
        )
        lease_path, account_home, window_lease = json.loads(output)
        expected_state = improve.ACCOUNT_HOME / ".local" / "state" / "v1simple"
        assert_true(Path(account_home) == improve.ACCOUNT_HOME, "account home followed ambient HOME")
        assert_true(
            Path(lease_path) == expected_state / "managed-v1-radio.lock",
            "controller rig lease followed ambient HOME",
        )
        assert_true(Path(window_lease) == Path(lease_path), "bench and controller lease paths differ")


def test_campaign_radio_lease_is_exclusive_and_inheritable() -> None:
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "managed-v1.lock"
        lease = improve.CampaignRadioLease(path)
        descriptor = lease.acquire()
        pass_fds, environment = lease.child_contract()
        assert_true(pass_fds == (descriptor,), "campaign descriptor was not inherited")
        assert_true(
            environment[improve.MANAGED_V1_RADIO_LEASE_FD_ENV] == str(descriptor),
            "campaign descriptor environment changed",
        )
        contender = os.open(path, os.O_RDWR)
        try:
            try:
                fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                pass
            else:
                raise AssertionError("campaign radio lease did not exclude another owner")
        finally:
            os.close(contender)
        lease.close()


def test_host_wide_leases_reject_an_intermediate_state_symlink() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        account_home = root / "account"
        account_home.mkdir()
        state_a = root / "state-a"
        state_b = root / "state-b"
        for target in (state_a, state_b):
            (target / "state" / "v1simple").mkdir(parents=True)
        local_link = account_home / ".local"
        local_link.symlink_to(state_a, target_is_directory=True)
        controller_path = account_home / ".local" / "state" / "v1simple" / "controller.lock"
        radio_path = account_home / ".local" / "state" / "v1simple" / "radio.lock"
        original_home = improve.ACCOUNT_HOME
        improve.ACCOUNT_HOME = account_home
        try:
            for take_lease in (
                lambda: improve.GlobalLease(controller_path).__enter__(),
                lambda: improve.CampaignRadioLease(radio_path).acquire(),
            ):
                expect_error(take_lease, improve.InvalidInput, "without symlinks")
            local_link.unlink()
            local_link.symlink_to(state_b, target_is_directory=True)
            for take_lease in (
                lambda: improve.GlobalLease(controller_path).__enter__(),
                lambda: improve.CampaignRadioLease(radio_path).acquire(),
            ):
                expect_error(take_lease, improve.InvalidInput, "without symlinks")
            assert_true(
                not (state_a / "state" / "v1simple" / "controller.lock").exists()
                and not (state_b / "state" / "v1simple" / "controller.lock").exists(),
                "a controller lock was created through the substituted state symlink",
            )
            assert_true(
                not (state_a / "state" / "v1simple" / "radio.lock").exists()
                and not (state_b / "state" / "v1simple" / "radio.lock").exists(),
                "a radio lock was created through the substituted state symlink",
            )
        finally:
            improve.ACCOUNT_HOME = original_home


def test_campaign_radio_lease_remains_owned_by_an_inherited_child() -> None:
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "managed-v1.lock"
        lease = improve.CampaignRadioLease(path)
        descriptor = lease.acquire()
        ready_read, ready_write = os.pipe()
        release_read, release_write = os.pipe()
        child = subprocess.Popen(
            [
                sys.executable,
                "-c",
                (
                    "import os,sys; "
                    "os.write(int(sys.argv[1]), b'1'); "
                    "os.read(int(sys.argv[2]), 1)"
                ),
                str(ready_write),
                str(release_read),
            ],
            pass_fds=(descriptor, ready_write, release_read),
        )
        os.close(ready_write)
        os.close(release_read)
        try:
            assert_true(os.read(ready_read, 1) == b"1", "lease child did not start")
            lease.close()
            contender = os.open(path, os.O_RDWR)
            try:
                try:
                    fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    pass
                else:
                    raise AssertionError("parent close unlocked an inherited campaign lease")
                os.write(release_write, b"1")
                child.wait(timeout=5)
                fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finally:
                os.close(contender)
        finally:
            os.close(ready_read)
            os.close(release_write)
            if child.poll() is None:
                child.kill()
                child.wait()


def test_controller_leases_reject_hardlinked_lock_files() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        original = root / "protected-evidence"
        original.write_text("must stay intact\n", encoding="utf-8")
        controller_lock = root / "controller.lock"
        radio_lock = root / "radio.lock"
        os.link(original, controller_lock)
        os.link(original, radio_lock)

        def take_controller_lock() -> None:
            with improve.GlobalLease(controller_lock):
                pass

        expect_error(take_controller_lock, improve.InvalidInput, "ownership is invalid")
        radio = improve.CampaignRadioLease(radio_lock)
        expect_error(radio.acquire, improve.InvalidInput, "ownership is invalid")
        assert_true(
            original.read_text(encoding="utf-8") == "must stay intact\n",
            "hardlinked lease validation modified the protected file",
        )


def test_evaluation_revert_refuses_a_switched_submitted_branch() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        source = root / "source"
        subprocess.run(["git", "init", "-q", "-b", "main", str(source)], check=True)
        subprocess.run(["git", "config", "user.name", "Phase B Test"], cwd=source, check=True)
        subprocess.run(
            ["git", "config", "user.email", "phase-b@example.invalid"], cwd=source, check=True
        )
        (source / "src").mkdir()
        (source / "src" / "candidate.cpp").write_text("int value = 1;\n", encoding="utf-8")
        subprocess.run(["git", "add", "--", "src/candidate.cpp"], cwd=source, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "base"], cwd=source, check=True)
        base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
        subprocess.run(["git", "switch", "-q", "-c", "submitted"], cwd=source, check=True)
        (source / "src" / "candidate.cpp").write_text("int value = 2;\n", encoding="utf-8")
        subprocess.run(["git", "add", "--", "src/candidate.cpp"], cwd=source, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "candidate"], cwd=source, check=True)
        candidate = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source, text=True
        ).strip()
        subprocess.run(["git", "switch", "-q", "main"], cwd=source, check=True)
        session = root / "session"
        store = improve.FileEvidenceStore(session)
        evaluation = session / "worktrees" / "candidate"
        evaluation.parent.mkdir(parents=True)
        subprocess.run(
            [
                "git",
                "worktree",
                "add",
                "-q",
                "-b",
                "improve/test/evaluation",
                str(evaluation),
                candidate,
            ],
            cwd=source,
            check=True,
        )
        plan = {
            "session_dir": str(session),
            "base_worktree": str(session / "worktrees" / "base"),
            "candidate_worktree": str(evaluation),
            "base_sha": base,
            "candidate_sha": candidate,
            "candidate_branch": "submitted",
            "evaluation_branch": "improve/test/evaluation",
        }
        original_root = improve.ROOT
        improve.ROOT = source
        try:
            adapter = improve.LiveAdapter(
                plan,
                store,
                improve.CommandRunner(improve.StopController()),
                improve.StopController(),
            )
            assert_true(
                adapter._assert_worktree_owned("candidate") == candidate,
                "owned evaluation branch was rejected",
            )
            subprocess.run(["git", "switch", "-q", "submitted"], cwd=evaluation, check=True)
            expect_error(
                adapter.finalize_evaluation,
                improve.GateFailure,
                "evaluation ownership",
            )
            submitted = subprocess.check_output(
                ["git", "rev-parse", "refs/heads/submitted"], cwd=source, text=True
            ).strip()
            assert_true(submitted == candidate, "submitted candidate branch was mutated")
        finally:
            improve.ROOT = original_root


def test_evaluation_revert_recovers_exact_staged_base_after_hook_abort() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        source = root / "source"
        subprocess.run(["git", "init", "-q", "-b", "main", str(source)], check=True)
        subprocess.run(["git", "config", "user.name", "Phase B Test"], cwd=source, check=True)
        subprocess.run(
            ["git", "config", "user.email", "phase-b@example.invalid"], cwd=source, check=True
        )
        subprocess.run(
            ["git", "config", "core.hooksPath", ".githooks"], cwd=source, check=True
        )
        fail_once = root / "fail-reference-transaction-once"
        hooks = source / ".githooks"
        hooks.mkdir()
        reference_hook = hooks / "reference-transaction"
        reference_hook.write_text(
            "#!/bin/sh\n"
            f'if [ -f "{fail_once}" ]; then rm -f "{fail_once}"; exit 1; fi\n'
            "exit 0\n",
            encoding="utf-8",
        )
        reference_hook.chmod(0o755)
        candidate_file = source / "src" / "display_frequency.cpp"
        candidate_file.parent.mkdir()
        candidate_file.write_text("int value = 1;\n", encoding="utf-8")
        subprocess.run(
            ["git", "add", "--", ".githooks/reference-transaction", "src/display_frequency.cpp"],
            cwd=source,
            check=True,
        )
        subprocess.run(["git", "commit", "-q", "-m", "base"], cwd=source, check=True)
        base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
        subprocess.run(["git", "switch", "-q", "-c", "submitted"], cwd=source, check=True)
        candidate_file.write_text("int value = 2;\n", encoding="utf-8")
        subprocess.run(["git", "add", "--", "src/display_frequency.cpp"], cwd=source, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "candidate"], cwd=source, check=True)
        candidate = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source, text=True
        ).strip()
        subprocess.run(["git", "switch", "-q", "main"], cwd=source, check=True)

        session = root / "session"
        store = improve.FileEvidenceStore(session)
        evaluation = session / "worktrees" / "candidate"
        evaluation.parent.mkdir(parents=True)
        subprocess.run(
            [
                "git",
                "worktree",
                "add",
                "-q",
                "-b",
                "improve/test/evaluation",
                str(evaluation),
                candidate,
            ],
            cwd=source,
            check=True,
        )
        plan = {
            "session_dir": str(session),
            "base_worktree": str(session / "worktrees" / "base"),
            "candidate_worktree": str(evaluation),
            "base_sha": base,
            "candidate_sha": candidate,
            "candidate_branch": "submitted",
            "evaluation_branch": "improve/test/evaluation",
        }
        original_root = improve.ROOT
        improve.ROOT = source
        try:
            adapter = improve.LiveAdapter(
                plan,
                store,
                improve.CommandRunner(improve.StopController()),
                improve.StopController(),
            )
            fail_once.write_text("fail once\n", encoding="utf-8")
            expect_error(
                adapter.finalize_evaluation,
                improve.GateFailure,
                "command failed",
            )
            result = adapter.finalize_evaluation()
            assert_true(
                result.get("revert_commit") not in {base, candidate},
                "retry did not create a distinct revert commit",
            )
            submitted = subprocess.check_output(
                ["git", "rev-parse", "refs/heads/submitted"], cwd=source, text=True
            ).strip()
            assert_true(submitted == candidate, "retry mutated the submitted candidate ref")
            assert_true(
                subprocess.check_output(
                    ["git", "diff", "--name-only", base, str(result["revert_commit"]), "--"],
                    cwd=evaluation,
                    text=True,
                ).strip()
                == "",
                "retry revert tree differs from the pinned base",
            )
            logs = sorted((session / "logs").glob("revert-evaluation-branch-attempt-*.log"))
            assert_true(len(logs) == 2, "revert retries did not retain two immutable logs")
        finally:
            improve.ROOT = original_root


def build_evaluation_repository(root: Path) -> dict:
    """Disposable source repo plus a controller-owned evaluation worktree.

    The candidate both edits a tracked file and adds a new one, so a correct
    finalization has to delete the added path again rather than merely revert
    the edit.
    """
    source = root / "source"
    subprocess.run(["git", "init", "-q", "-b", "main", str(source)], check=True)
    subprocess.run(["git", "config", "user.name", "Phase B Test"], cwd=source, check=True)
    subprocess.run(
        ["git", "config", "user.email", "phase-b@example.invalid"], cwd=source, check=True
    )
    subprocess.run(["git", "config", "core.hooksPath", ".githooks"], cwd=source, check=True)
    hooks = source / ".githooks"
    hooks.mkdir()
    reference_hook = hooks / "reference-transaction"
    # Mirror the repository gate's fail-closed rule for ref targets: only commit
    # and tag objects are scannable, so anything else (notably the AUTO_MERGE
    # tree that `git revert` writes) must abort the transaction.
    reference_hook.write_text(
        "#!/bin/sh\n"
        'case "${1:-}" in\n'
        "  preparing|prepared) ;;\n"
        "  *) exit 0 ;;\n"
        "esac\n"
        "while read -r old new ref; do\n"
        '  [ -n "${ref:-}" ] || continue\n'
        '  case "${new:-}" in ref:*) continue ;; esac\n'
        '  [ -n "$(printf %s "$new" | tr -d 0)" ] || continue\n'
        '  kind=$(git cat-file -t "$new" 2>/dev/null) || exit 1\n'
        '  case "$kind" in\n'
        "    commit|tag) ;;\n"
        '    *) echo "blocked non-commit ref target: $kind" >&2; exit 1 ;;\n'
        "  esac\n"
        "done\n"
        "exit 0\n",
        encoding="utf-8",
    )
    reference_hook.chmod(0o755)
    (source / "src").mkdir()
    (source / "src" / "display_indicators.cpp").write_text("int value = 1;\n", encoding="utf-8")
    subprocess.run(
        ["git", "add", "--", ".githooks/reference-transaction", "src/display_indicators.cpp"],
        cwd=source,
        check=True,
    )
    subprocess.run(["git", "commit", "-q", "-m", "base"], cwd=source, check=True)
    base = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()

    subprocess.run(["git", "switch", "-q", "-c", "submitted"], cwd=source, check=True)
    (source / "src" / "display_indicators.cpp").write_text("int value = 2;\n", encoding="utf-8")
    (source / "test" / "test_added").mkdir(parents=True)
    (source / "test" / "test_added" / "added.cpp").write_text("int added = 1;\n", encoding="utf-8")
    subprocess.run(
        ["git", "add", "--", "src/display_indicators.cpp", "test/test_added/added.cpp"],
        cwd=source,
        check=True,
    )
    subprocess.run(
        ["git", "commit", "-q", "-m", "perf(display): candidate"], cwd=source, check=True
    )
    candidate = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=source, text=True
    ).strip()
    subprocess.run(["git", "switch", "-q", "main"], cwd=source, check=True)

    session = root / "session"
    # FileEvidenceStore owns session creation, so build it before the worktree.
    store = improve.FileEvidenceStore(session)
    evaluation = session / "worktrees" / "candidate"
    evaluation.parent.mkdir(parents=True)
    subprocess.run(
        [
            "git",
            "worktree",
            "add",
            "-q",
            "-b",
            "improve/test/evaluation",
            str(evaluation),
            candidate,
        ],
        cwd=source,
        check=True,
    )
    return {
        "source": source,
        "session": session,
        "store": store,
        "evaluation": evaluation,
        "base": base,
        "candidate": candidate,
        "plan": {
            "session_dir": str(session),
            "base_worktree": str(session / "worktrees" / "base"),
            "candidate_worktree": str(evaluation),
            "base_sha": base,
            "candidate_sha": candidate,
            "candidate_branch": "submitted",
            "evaluation_branch": "improve/test/evaluation",
        },
    }


def logged_git_subcommands(session: Path) -> list[str]:
    """Return the git subcommand of every immutable controller command log."""
    subcommands = []
    for log in sorted((session / "logs").glob("*.log")):
        first = log.read_text(encoding="utf-8").splitlines()[0]
        tokens = first.removeprefix("$ ").split()
        # Skip `git` and any leading `-c key=value` pairs.
        index = 1
        while index < len(tokens) and tokens[index] == "-c":
            index += 2
        if tokens and tokens[0].endswith("git") and index < len(tokens):
            subcommands.append(tokens[index])
    return subcommands


def test_clean_finalization_commits_base_tree_without_git_revert() -> None:
    with tempfile.TemporaryDirectory() as temp:
        fixture = build_evaluation_repository(Path(temp))
        source = fixture["source"]
        evaluation = fixture["evaluation"]
        base = fixture["base"]
        candidate = fixture["candidate"]
        store = fixture["store"]
        original_root = improve.ROOT
        improve.ROOT = source
        try:
            adapter = improve.LiveAdapter(
                fixture["plan"],
                store,
                improve.CommandRunner(improve.StopController()),
                improve.StopController(),
            )
            result = adapter.finalize_evaluation()
            revert_commit = str(result["revert_commit"])

            subcommands = logged_git_subcommands(fixture["session"])
            assert_true(
                "revert" not in subcommands,
                f"clean finalization still invoked git revert: {subcommands}",
            )
            assert_true(
                "commit" in subcommands and "read-tree" in subcommands,
                f"clean finalization did not stage and commit the base tree: {subcommands}",
            )
            assert_true(
                subprocess.run(
                    ["git", "rev-parse", "--verify", "-q", "AUTO_MERGE"],
                    cwd=evaluation,
                    capture_output=True,
                ).returncode
                != 0,
                "finalization left an AUTO_MERGE pseudo-ref behind",
            )

            parents = subprocess.check_output(
                ["git", "rev-list", "--parents", "-n", "1", revert_commit],
                cwd=evaluation,
                text=True,
            ).split()
            assert_true(
                parents == [revert_commit, candidate],
                "revert commit does not have the candidate as its sole parent",
            )
            assert_true(
                subprocess.check_output(
                    ["git", "rev-parse", f"{revert_commit}^{{tree}}"], cwd=evaluation, text=True
                ).strip()
                == subprocess.check_output(
                    ["git", "rev-parse", f"{base}^{{tree}}"], cwd=evaluation, text=True
                ).strip(),
                "revert commit tree does not equal the pinned base tree",
            )
            assert_true(
                subprocess.check_output(
                    ["git", "status", "--porcelain=v1", "-uall"], cwd=evaluation, text=True
                ).strip()
                == "",
                "finalization left the evaluation worktree dirty",
            )
            submitted = subprocess.check_output(
                ["git", "rev-parse", "refs/heads/submitted"], cwd=source, text=True
            ).strip()
            assert_true(submitted == candidate, "submitted candidate branch was mutated")
        finally:
            improve.ROOT = original_root


class RealFinalizeAdapter(improve.FakeAdapter):
    """Simulated measurements with a real-Git evaluation finalization."""

    def __init__(self, baseline_values, candidate_values, live) -> None:
        super().__init__(baseline_values, candidate_values)
        self._live = live
        self.fail_first_finalize = False
        self.finalize_calls = 0

    def finalize_evaluation(self) -> dict:
        self.operations.append("finalize_evaluation")
        self.finalize_calls += 1
        if self.fail_first_finalize and self.finalize_calls == 1:
            raise improve.GateFailure(
                "simulated transient reference-gate abort during finalization"
            )
        return self._live.finalize_evaluation()


def run_overlapping_experiment(temp: str, *, fail_first_finalize: bool) -> dict:
    fixture = build_evaluation_repository(Path(temp))
    store = fixture["store"]
    original_root = improve.ROOT
    improve.ROOT = fixture["source"]
    try:
        live = improve.LiveAdapter(
            fixture["plan"],
            store,
            improve.CommandRunner(improve.StopController()),
            improve.StopController(),
        )
        # Candidate is mostly faster but its worst sample exceeds the best
        # baseline sample, so the strict envelope rule must reject it.
        adapter = RealFinalizeAdapter([100] * 5, [90, 90, 90, 90, 110], live)
        adapter.fail_first_finalize = fail_first_finalize
        decision = improve.execute_experiment(improve.dry_plan(), adapter, store)
        return {"decision": decision, "store": store, "adapter": adapter, "fixture": fixture}
    finally:
        improve.ROOT = original_root


def assert_complete_no_improvement(outcome: dict, label: str) -> None:
    decision = outcome["decision"]
    store = outcome["store"]
    assert_true(
        decision["result"] == "REJECTED_NO_IMPROVEMENT",
        f"{label}: expected REJECTED_NO_IMPROVEMENT, got {decision['result']}",
    )
    analysis = decision["analysis"]
    assert_true(bool(analysis), f"{label}: terminal decision omitted the stored analysis")
    assert_true(
        analysis == store.state["analysis"],
        f"{label}: published analysis differs from the recorded analysis",
    )
    assert_true(
        analysis["accepted"] is False
        and analysis["separation_gap"] < 0
        and analysis["baseline"]["count"] == 5
        and analysis["candidate"]["count"] == 5,
        f"{label}: analysis is not the complete five-versus-five envelope result",
    )
    assert_true(
        store.decision_path.is_file(),
        f"{label}: terminal decision was not published",
    )
    revert_commit = subprocess.check_output(
        ["git", "rev-parse", "refs/heads/improve/test/evaluation"],
        cwd=outcome["fixture"]["source"],
        text=True,
    ).strip()
    assert_true(
        revert_commit != outcome["fixture"]["candidate"],
        f"{label}: evaluation branch was not finalized before publication",
    )
    submitted = subprocess.check_output(
        ["git", "rev-parse", "refs/heads/submitted"],
        cwd=outcome["fixture"]["source"],
        text=True,
    ).strip()
    assert_true(
        submitted == outcome["fixture"]["candidate"],
        f"{label}: submitted candidate branch was mutated",
    )


def test_overlapping_measurements_reject_no_improvement_after_real_cleanup() -> None:
    with tempfile.TemporaryDirectory() as temp:
        outcome = run_overlapping_experiment(temp, fail_first_finalize=False)
        assert_complete_no_improvement(outcome, "clean cleanup")
        assert_true(
            "revert" not in logged_git_subcommands(outcome["fixture"]["session"]),
            "clean rejection cleanup invoked git revert",
        )


def test_recovered_cleanup_preserves_the_no_improvement_outcome() -> None:
    # Reproduces the observed defect: the first finalization attempt aborts,
    # the controller's own retry succeeds, and the already-measured experimental
    # outcome must survive that transient internal step.
    with tempfile.TemporaryDirectory() as temp:
        outcome = run_overlapping_experiment(temp, fail_first_finalize=True)
        assert_true(
            outcome["adapter"].finalize_calls == 2,
            "internal cleanup retry did not run",
        )
        assert_complete_no_improvement(outcome, "recovered cleanup")


class AlwaysFailFinalizeAdapter(improve.FakeAdapter):
    def __init__(self) -> None:
        super().__init__([100] * 5, [90, 90, 90, 90, 110])
        self.fail_revert = True

    def finalize_evaluation(self) -> dict:
        self.operations.append("finalize_evaluation")
        if self.fail_revert:
            raise improve.GateFailure("simulated unresolved evaluation cleanup failure")
        return {"message": "simulated evaluation branch reverted to base tree"}


def test_unresolved_cleanup_after_analysis_blocks_and_retains_analysis() -> None:
    plan = improve.dry_plan()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "session"
        store = improve.FileEvidenceStore(root)
        adapter = AlwaysFailFinalizeAdapter()
        first = improve.execute_experiment(plan, adapter, store)
        assert_true(
            first["result"] == "CLEANUP_FAILED",
            "genuine unresolved cleanup failure was downgraded to a terminal rejection",
        )
        assert_true(
            store.state["status"] == "CLEANUP_FAILED",
            "unresolved cleanup failure was marked terminal",
        )
        assert_true(
            store.state["evaluation_cleanup_required"] is True,
            "evaluation-branch cleanup obligation was cleared",
        )
        assert_true(
            not store.decision_path.exists(),
            "unresolved cleanup published a misleading terminal decision",
        )
        assert_true(
            bool(first["analysis"]) and first["analysis"] == store.state["analysis"],
            "tolerant post-analysis failure decision dropped the stored analysis",
        )
        failures = sorted((root / "cleanup_failures").glob("attempt-*.json"))
        assert_true(len(failures) == 1, "unresolved cleanup evidence was not preserved")

        adapter.fail_revert = False
        reopened = improve.FileEvidenceStore.open(root)
        recovered = improve.recover_experiment(plan, adapter, reopened)
        assert_true(
            recovered["result"] in improve.TERMINAL_STATES,
            "cleanup retry did not close the session",
        )
        assert_true(
            bool(recovered["analysis"]),
            "recovery decision dropped the already-recorded analysis",
        )
        assert_true(
            reopened.state["evaluation_cleanup_required"] is False,
            "evaluation-branch cleanup obligation survived success",
        )


def test_effective_worktree_hook_override_is_rejected() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        source = root / "source"
        subprocess.run(["git", "init", "-q", "-b", "main", str(source)], check=True)
        subprocess.run(["git", "config", "user.name", "Phase B Test"], cwd=source, check=True)
        subprocess.run(
            ["git", "config", "user.email", "phase-b@example.invalid"], cwd=source, check=True
        )
        hooks = source / ".githooks"
        hooks.mkdir()
        reference_hook = hooks / "reference-transaction"
        reference_hook.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        reference_hook.chmod(0o755)
        (source / "src").mkdir()
        (source / "src" / "display_frequency.cpp").write_text(
            "int value = 1;\n", encoding="utf-8"
        )
        subprocess.run(["git", "add", "--", ".githooks", "src"], cwd=source, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "base"], cwd=source, check=True)
        subprocess.run(
            ["git", "config", "core.hooksPath", ".githooks"], cwd=source, check=True
        )
        evaluation = root / "evaluation"
        subprocess.run(
            ["git", "worktree", "add", "-q", "-b", "evaluation", str(evaluation), "HEAD"],
            cwd=source,
            check=True,
        )
        git_dir = subprocess.check_output(
            ["git", "rev-parse", "--absolute-git-dir"], cwd=evaluation, text=True
        ).strip()
        override = root / "override.config"
        subprocess.run(
            ["git", "config", "--file", str(override), "core.hooksPath", "/dev/null"],
            check=True,
        )
        subprocess.run(
            [
                "git",
                "config",
                "--add",
                f"includeIf.gitdir:{git_dir}.path",
                str(override),
            ],
            cwd=source,
            check=True,
        )
        assert_true(
            subprocess.check_output(
                ["git", "config", "--get", "core.hooksPath"], cwd=evaluation, text=True
            ).strip()
            == "/dev/null",
            "fixture did not create an effective worktree hook override",
        )
        expect_error(
            lambda: improve.assert_repository_hook_contract(evaluation),
            improve.GateFailure,
            "privacy-hook contract",
        )


def test_target_policy() -> None:
    policy, digest = improve.resolve_target_policy(
        ROOT / "tools" / "hardware_metric_catalog.json", "replay", "disp_pipe_p95_us"
    )
    assert_true(policy.direction == "lower_better", "target direction did not come from catalog")
    assert_true(policy.score_level == "hard", "hard optimization target changed")
    assert_true(improve.valid_digest(digest), "catalog digest is invalid")
    expect_error(
        lambda: improve.resolve_target_policy(
            ROOT / "tools" / "hardware_metric_catalog.json", "replay", "notify_to_display_max_ms"
        ),
        improve.InvalidInput,
        "Informational".lower(),
    )


def write_metric_fixture(
    root: Path,
    policy: improve.MetricPolicy,
    value: object,
    *,
    unit: str | None = None,
) -> Path:
    manifest = dict(improve.EXPECTED_TRACK["replay"])
    manifest.update(
        {
            "schema_version": 1,
            "run_id": "fixture-run",
            "timestamp_utc": "2026-01-01T00:00:00Z",
            "git_sha": "a" * 40,
            "git_ref": "fixture",
            "board_id": "release",
            "env": "waveshare-349",
            "result": "NO_BASELINE",
            "base_result": "PASS",
            "metrics_file": "metrics.ndjson",
            "scoring_file": "scoring.json",
            "tracks": [improve.PROFILE],
        }
    )
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    record = {
        "schema_version": 1,
        "run_id": "fixture-run",
        "git_sha": "a" * 40,
        "run_kind": "real_fw_soak",
        "suite_or_profile": improve.PROFILE,
        "metric": policy.metric,
        "sample": 0,
        "value": value,
        "unit": unit if unit is not None else policy.unit,
        "tags": {},
    }
    (root / "metrics.ndjson").write_text(json.dumps(record) + "\n", encoding="utf-8")
    scoring_value = value
    (root / "scoring.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "result": "PASS",
                "metrics": [
                    {
                        "metric": policy.metric,
                        "suite_or_profile": improve.PROFILE,
                        "current_value": scoring_value,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    return path


def write_owned_suite_fixture(
    run_dir: Path,
    suite: str,
    *,
    arm: str = "candidate",
) -> tuple[dict, dict, dict[str, Path]]:
    suite_dir = run_dir / suite
    suite_dir.mkdir(parents=True, exist_ok=True)
    sha = ("b" if arm == "baseline" else "c") * 40
    repository_ref = "HEAD" if arm == "baseline" else "improve/test/evaluation"
    identities = {
        "product_fingerprint": ("1" if arm == "baseline" else "2") * 64,
        "grader_fingerprint": "3" * 64,
        "hardware_scoring_fingerprint": "4" * 64,
        "scenario_fingerprints": {
            "core": "5" * 64,
            "display": "6" * 64,
            "replay": "7" * 64,
        },
        "repository_sha": sha,
        "repository_ref": repository_ref,
        "worktree_clean": True,
    }
    plan = {"base_sha": "b" * 40, "candidate_sha": "c" * 40}
    source = suite_dir / "source.csv"
    source.write_text("header\nvalue\n", encoding="utf-8")
    run_id = f"owned-{arm}-{suite}"
    track = improve.EXPECTED_TRACK[suite]
    manifest = {
        "schema_version": 1,
        "run_id": run_id,
        "timestamp_utc": "2026-01-01T00:00:00Z",
        "git_sha": sha,
        "git_ref": repository_ref,
        "product_fingerprint": identities["product_fingerprint"],
        "grader_fingerprint": identities["grader_fingerprint"],
        "hardware_scoring_fingerprint": identities["hardware_scoring_fingerprint"],
        "scenario_fingerprint": identities["scenario_fingerprints"][suite],
        "board_id": improve.DEFAULT_BOARD_ID,
        "env": "perf-csv-import",
        **track,
        "result": "NO_BASELINE",
        "base_result": "PASS",
        "metrics_file": "metrics.ndjson",
        "scoring_file": "scoring.json",
        "source_input": str(source),
        "tracks": [improve.PROFILE],
        "unsupported_metrics": [],
    }
    identity = {
        "schema_version": 2,
        "kind": "bench_identity",
        "product_fingerprint": identities["product_fingerprint"],
        "grader_fingerprint": identities["grader_fingerprint"],
        "hardware_scoring_fingerprint": identities["hardware_scoring_fingerprint"],
        "scenario_fingerprint": identities["scenario_fingerprints"][suite],
        "traceability": {
            "repository_sha": sha,
            "repository_ref": repository_ref,
            "worktree_clean": True,
        },
    }
    records = [
        {
            "schema_version": 1,
            "run_id": run_id,
            "git_sha": sha,
            "run_kind": track["run_kind"],
            "suite_or_profile": track["suite_or_profile"],
            "metric": metric,
            "sample": "value",
            "value": value,
            "unit": "us",
            "tags": {},
        }
        for metric, value in (("disp_pipe_p95_us", 100.0), ("disp_pipe_max_peak_us", 120.0))
    ]
    scoring = {
        "schema_version": 1,
        "result": "NO_BASELINE",
        "manifest": {
            "path": str(suite_dir / "manifest.json"),
            **{
                key: manifest[key]
                for key in (
                    "run_id",
                    "git_sha",
                    "git_ref",
                    "run_kind",
                    "board_id",
                    "env",
                    "lane",
                    "suite_or_profile",
                    "stress_class",
                    "hardware_scoring_fingerprint",
                    "base_result",
                    "source_type",
                )
            },
        },
        "summary": {"hard_failures": 0, "advisory_failures": 0},
        "metrics": [
            {
                "metric": "disp_pipe_p95_us",
                "suite_or_profile": improve.PROFILE,
                "current_value": 100.0,
            }
        ],
    }
    paths = {
        "identity.json": suite_dir / "identity.json",
        "manifest.json": suite_dir / "manifest.json",
        "metrics.ndjson": suite_dir / "metrics.ndjson",
        "scoring.json": suite_dir / "scoring.json",
    }
    paths["identity.json"].write_text(json.dumps(identity), encoding="utf-8")
    paths["manifest.json"].write_text(json.dumps(manifest), encoding="utf-8")
    paths["metrics.ndjson"].write_text(
        "".join(json.dumps(record) + "\n" for record in records), encoding="utf-8"
    )
    paths["scoring.json"].write_text(json.dumps(scoring), encoding="utf-8")
    return plan, identities, paths


def test_suite_artifacts_bind_every_metric_to_the_planned_identity() -> None:
    with tempfile.TemporaryDirectory() as temp:
        run_dir = Path(temp) / "run"
        plan, identities, paths = write_owned_suite_fixture(run_dir, "replay")
        validated = improve.validate_suite_artifacts(
            run_dir,
            "replay",
            arm="candidate",
            plan=plan,
            identities=identities,
        )
        assert_true(validated["metric_count"] == 2, "owned metric count changed")

        manifest = json.loads(paths["manifest.json"].read_text(encoding="utf-8"))
        manifest["git_sha"] = "9" * 40
        paths["manifest.json"].write_text(json.dumps(manifest), encoding="utf-8")
        expect_error(
            lambda: improve.validate_suite_artifacts(
                run_dir, "replay", arm="candidate", plan=plan, identities=identities
            ),
            improve.GateFailure,
            "manifest git_sha",
        )

        plan, identities, paths = write_owned_suite_fixture(run_dir, "replay")
        records = [json.loads(line) for line in paths["metrics.ndjson"].read_text().splitlines()]
        records[1]["git_sha"] = "8" * 40
        paths["metrics.ndjson"].write_text(
            "".join(json.dumps(record) + "\n" for record in records), encoding="utf-8"
        )
        expect_error(
            lambda: improve.validate_suite_artifacts(
                run_dir, "replay", arm="candidate", plan=plan, identities=identities
            ),
            improve.GateFailure,
            "not owned by its manifest",
        )

        plan, identities, paths = write_owned_suite_fixture(run_dir, "replay")
        identity = json.loads(paths["identity.json"].read_text(encoding="utf-8"))
        identity["product_fingerprint"] = "9" * 64
        paths["identity.json"].write_text(json.dumps(identity), encoding="utf-8")
        expect_error(
            lambda: improve.validate_suite_artifacts(
                run_dir, "replay", arm="candidate", plan=plan, identities=identities
            ),
            improve.GateFailure,
            "identity is not owned",
        )


def test_final_decision_revalidates_qualification_and_cited_metric_bytes() -> None:
    with tempfile.TemporaryDirectory() as temp:
        session = Path(temp) / "session"
        store = improve.FileEvidenceStore(session)
        run_dir = session / "bench" / "candidate" / "release" / "runs" / "run-001"
        suite_paths: dict[str, dict[str, Path]] = {}
        for suite in ("core", "display", "replay"):
            _plan, _identities, paths = write_owned_suite_fixture(
                run_dir, suite, arm="candidate"
            )
            suite_paths[suite] = paths
        log_path = session / "logs" / "bench.log"
        log_path.parent.mkdir(parents=True)
        log_path.write_text("bench passed\n", encoding="utf-8")
        bench_result = run_dir / "bench_result.json"
        bench_result.write_text("{}\n", encoding="utf-8")
        qualification = session / "qualifications" / "run-001.json"
        qualification.parent.mkdir(parents=True)
        qualification.write_text(
            json.dumps(
                {
                    "evidence": {
                        "bench_result": str(bench_result),
                        "bench_result_sha256": improve.sha256_file(bench_result),
                    }
                }
            )
            + "\n",
            encoding="utf-8",
        )
        run = {
            "arm": "candidate",
            "arm_index": 1,
            "sequence": 1,
            "result": "PASS",
            "simulated": False,
            "run_dir": str(run_dir),
            "bench_log": str(log_path),
            "bench_log_sha256": improve.sha256_file(log_path),
            "bench_result": str(bench_result),
            "bench_result_sha256": improve.sha256_file(bench_result),
            "qualification": str(qualification),
            "qualification_sha256": improve.sha256_file(qualification),
            "suite_artifacts": {
                suite: {
                    name: {"path": str(path), "sha256": improve.sha256_file(path)}
                    for name, path in paths.items()
                }
                for suite, paths in suite_paths.items()
            },
        }
        store.update(runs=[run])

        original_record = improve.validate_qualification_record
        original_evidence = improve.validate_qualification_evidence
        improve.validate_qualification_record = lambda _record: None  # type: ignore[assignment]
        improve.validate_qualification_evidence = (  # type: ignore[assignment]
            lambda _record: (_ for _ in ()).throw(RuntimeError("owned replay video missing"))
        )
        try:
            expect_error(
                lambda: improve.verify_decision_evidence_files(store),
                improve.GateFailure,
                "owned replay video missing",
            )
            improve.validate_qualification_evidence = lambda _record: None  # type: ignore[assignment]
            replay_metrics = suite_paths["replay"]["metrics.ndjson"]
            replay_metrics.write_text(
                replay_metrics.read_text(encoding="utf-8") + "{}\n", encoding="utf-8"
            )
            expect_error(
                lambda: improve.verify_decision_evidence_files(store),
                improve.GateFailure,
                "bytes or ownership changed",
            )
            live_like_plan = {**improve.dry_plan(), "simulated": False}
            failure = improve.decision_payload(
                "REJECTED_GATE_FAILURE",
                live_like_plan,
                store,
                reason="cited metric bytes changed",
                require_valid_evidence=False,
            )
            assert_true(
                failure["evidence_integrity"]["status"] == "FAIL",
                "failure closeout recursively required already-invalid evidence",
            )
        finally:
            improve.validate_qualification_record = original_record  # type: ignore[assignment]
            improve.validate_qualification_evidence = original_evidence  # type: ignore[assignment]


def test_strict_target_extraction() -> None:
    policy, _ = improve.resolve_target_policy(
        ROOT / "tools" / "hardware_metric_catalog.json", "replay", "disp_pipe_p95_us"
    )
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        manifest = write_metric_fixture(root, policy, 123.5)
        assert_true(improve.extract_target_value(manifest, policy, "replay") == 123.5, "value changed")
        write_metric_fixture(root, policy, True)
        expect_error(
            lambda: improve.extract_target_value(manifest, policy, "replay"),
            improve.GateFailure,
            "non-boolean",
        )
        write_metric_fixture(root, policy, float("nan"))
        expect_error(
            lambda: improve.extract_target_value(manifest, policy, "replay"),
            improve.GateFailure,
            "not finite",
        )
        write_metric_fixture(root, policy, 123.5, unit="ms")
        expect_error(
            lambda: improve.extract_target_value(manifest, policy, "replay"),
            improve.GateFailure,
            "unit mismatch",
        )


def memory_payload(flash_used: int = 100, flash_limit: int = 1000) -> dict:
    return {
        "env": improve.DEFAULT_ENV,
        "memory": {
            "flash": {
                "used_bytes": flash_used,
                "limit_bytes": flash_limit,
                "headroom_bytes": flash_limit - flash_used,
            },
            "ram": {"used_bytes": 200, "limit_bytes": 500, "headroom_bytes": 300},
            "iram": {"used_bytes": 300, "limit_bytes": 300, "headroom_bytes": 0},
        },
    }


def test_resource_contract() -> None:
    deltas = improve.compare_memory_reports(memory_payload(), memory_payload(110))
    assert_true(deltas["flash"]["delta_used_bytes"] == 10, "flash delta changed")
    improve.validate_memory_report(memory_payload(1000), "exact-limit")
    expect_error(
        lambda: improve.validate_memory_report(memory_payload(1001), "overflow"),
        improve.ResourceFailure,
        "exceeds",
    )
    changed_limit = memory_payload(100, 1100)
    expect_error(
        lambda: improve.compare_memory_reports(memory_payload(), changed_limit),
        improve.ResourceFailure,
        "changed the flash resource limit",
    )
    corrupt = memory_payload()
    corrupt["memory"]["ram"]["headroom_bytes"] = 301
    expect_error(
        lambda: improve.validate_memory_report(corrupt, "corrupt"),
        improve.ResourceFailure,
        "arithmetic",
    )


def test_dry_run_is_deterministic_and_offline() -> None:
    first = improve.build_dry_run_report()
    second = improve.build_dry_run_report()
    assert_true(first == second, "dry-run output is not deterministic")
    assert_true(first["result"] == "PASS", "dry-run self-test failed")
    assert_true(
        first["hardware_actions"] == 0 and first["external_product_actions"] == 0,
        "dry-run touched a product-side tool",
    )
    assert_true(first["git_actions"] > 0, "dry-run did not exercise disposable Git")
    scenarios = first["scenarios"]
    assert_true(
        scenarios["no_op_rejection"]["decision"]["result"] == "REJECTED_NO_CHANGE",
        "no-op candidate was not rejected",
    )
    assert_true(
        scenarios["clear_improvement_acceptance"]["decision"]["result"] == "ACCEPTED",
        "clear improvement was not accepted",
    )
    assert_true(
        scenarios["candidate_flash_interruption_recovery"]["decision"]["result"]
        == "ABORTED_BASE_RESTORED",
        "uncertain candidate flash did not recover baseline",
    )


class FailingCandidateAdapter(improve.FakeAdapter):
    def collect(self, arm: str, arm_index: int, sequence: int) -> dict:
        if arm == "candidate":
            raise improve.GateFailure("candidate full gate failed")
        return super().collect(arm, arm_index, sequence)


class StopBeforeAcceptanceAdapter(improve.FakeAdapter):
    def __init__(self) -> None:
        super().__init__([100] * 5, [90] * 5)
        self.stop_requested = False

    def validate_regressions(self, runs: list[dict]) -> list[dict]:
        result = super().validate_regressions(runs)
        self.stop_requested = True
        return result

    def check_stop(self) -> None:
        if self.stop_requested:
            raise improve.ControllerInterrupted("simulated stop before acceptance")


def test_stop_during_final_validation_cannot_publish_acceptance() -> None:
    plan = improve.dry_plan()
    store = improve.MemoryEvidenceStore()
    adapter = StopBeforeAcceptanceAdapter()
    decision = improve.execute_experiment(plan, adapter, store)
    assert_true(
        decision["result"] == "ABORTED_BASE_RESTORED",
        "stop during final validation published or preserved acceptance",
    )
    assert_true(
        "flash:baseline:recovery" in adapter.operations,
        "stop during final validation did not restore baseline",
    )
    assert_true(
        adapter.operations[-1] == "finalize_evaluation",
        "stop during final validation did not revert evaluation branch",
    )


def test_candidate_gate_failure_restores_and_reverts() -> None:
    plan = improve.dry_plan()
    store = improve.MemoryEvidenceStore()
    adapter = FailingCandidateAdapter([100] * 5, [90] * 5)
    decision = improve.execute_experiment(plan, adapter, store)
    assert_true(decision["result"] == "REJECTED_GATE_FAILURE", "gate failure taxonomy changed")
    assert_true("flash:baseline:recovery" in adapter.operations, "baseline was not restored")
    assert_true(adapter.operations[-1] == "finalize_evaluation", "evaluation branch was not reverted")


class RestoreFailsOnceAdapter(FailingCandidateAdapter):
    def __init__(self) -> None:
        super().__init__([100] * 5, [90] * 5)
        self.fail_restore = True

    def flash(self, arm: str, *, recovery: bool = False) -> None:
        super().flash(arm, recovery=recovery)
        if arm == "baseline" and recovery and self.fail_restore:
            raise improve.GateFailure("simulated baseline restore failure")


class FinalizeFailsOnceAdapter(FailingCandidateAdapter):
    def __init__(self) -> None:
        super().__init__([100] * 5, [90] * 5)
        self.fail_revert = True

    def finalize_evaluation(self) -> dict[str, object]:
        self.operations.append("finalize_evaluation")
        if self.fail_revert:
            raise improve.GateFailure("simulated evaluation revert failure")
        return {"message": "simulated evaluation branch reverted to base tree"}


def test_restore_failure_stays_unresolved_and_retries() -> None:
    plan = improve.dry_plan()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "session"
        store = improve.FileEvidenceStore(root)
        adapter = RestoreFailsOnceAdapter()
        first = improve.execute_experiment(plan, adapter, store)
        assert_true(first["result"] == "RESTORE_FAILED", "restore failure taxonomy changed")
        assert_true(store.state["status"] == "RESTORE_FAILED", "restore failure was marked terminal")
        assert_true(store.state["restore_required"] is True, "restore obligation was cleared")
        assert_true(not store.decision_path.exists(), "unresolved restore published a terminal decision")
        assert_true(
            len(list((root / "cleanup_failures").glob("attempt-*.json"))) == 1,
            "restore failure attempt was not preserved",
        )

        adapter.fail_restore = False
        reopened = improve.FileEvidenceStore.open(root)
        recovered = improve.recover_experiment(plan, adapter, reopened)
        assert_true(
            recovered["result"] == "ABORTED_BASE_RESTORED",
            "successful retry did not close with restored baseline",
        )
        assert_true(reopened.state["restore_required"] is False, "restore obligation survived success")
        assert_true(reopened.decision_path.is_file(), "successful retry did not publish a decision")


def test_revert_failure_stays_unresolved_and_retries() -> None:
    plan = improve.dry_plan()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "session"
        store = improve.FileEvidenceStore(root)
        adapter = FinalizeFailsOnceAdapter()
        first = improve.execute_experiment(plan, adapter, store)
        assert_true(first["result"] == "CLEANUP_FAILED", "revert failure taxonomy changed")
        assert_true(store.state["status"] == "CLEANUP_FAILED", "revert failure was marked terminal")
        assert_true(
            store.state["evaluation_cleanup_required"] is True,
            "evaluation-branch cleanup obligation was cleared",
        )
        assert_true(not store.decision_path.exists(), "unresolved cleanup published a terminal decision")

        adapter.fail_revert = False
        reopened = improve.FileEvidenceStore.open(root)
        recovered = improve.recover_experiment(plan, adapter, reopened)
        assert_true(
            recovered["result"] == "ABORTED_NO_RESTORE",
            "successful cleanup retry did not close conservatively",
        )
        assert_true(
            reopened.state["evaluation_cleanup_required"] is False,
            "evaluation-branch cleanup obligation survived success",
        )
        assert_true(reopened.decision_path.is_file(), "successful cleanup retry did not publish a decision")


def test_recovery_finalizes_evaluation_after_restoring_uncertain_candidate() -> None:
    plan = improve.dry_plan()
    store = improve.MemoryEvidenceStore()
    store.update(
        status="CANDIDATE_FLASHED",
        current_firmware="candidate",
        candidate_may_be_installed=True,
        restore_required=True,
        # Reproduce the old accepted-publication crash window where this flag
        # had been cleared before decision.json existed.
        evaluation_cleanup_required=False,
    )
    adapter = improve.FakeAdapter([100] * 5, [90] * 5)
    decision = improve.recover_experiment(plan, adapter, store)
    assert_true(
        decision["result"] == "ABORTED_BASE_RESTORED",
        "uncertain accepted boundary was not recovered conservatively",
    )
    assert_true(
        "flash:baseline:recovery" in adapter.operations,
        "recovery did not restore baseline firmware",
    )
    assert_true(
        adapter.operations[-1] == "finalize_evaluation",
        "restored interrupted candidate left its evaluation branch active",
    )


def test_published_acceptance_normalizes_flags_without_reverting_candidate() -> None:
    plan = improve.dry_plan()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "session"
        store = improve.FileEvidenceStore(root)
        store.event("accepted")
        decision = improve.decision_payload(
            "ACCEPTED",
            plan,
            store,
            analysis={"accepted": True},
        )
        store.finish(decision)
        store.update(
            candidate_may_be_installed=True,
            restore_required=True,
            evaluation_cleanup_required=True,
        )
        reopened = improve.FileEvidenceStore.open(root)
        adapter = improve.FakeAdapter([100] * 5, [90] * 5)
        recovered = improve.recover_experiment(plan, adapter, reopened)
        assert_true(recovered["result"] == "ACCEPTED", "published acceptance changed")
        assert_true(
            not any(operation.startswith("flash:") for operation in adapter.operations),
            "published acceptance incorrectly restored firmware",
        )
        assert_true(
            "finalize_evaluation" not in adapter.operations,
            "published acceptance incorrectly reverted its evaluation branch",
        )
        assert_true(
            reopened.state["restore_required"] is False
            and reopened.state["evaluation_cleanup_required"] is False,
            "published acceptance bookkeeping was not normalized",
        )


def test_flash_commands_never_upload_filesystem() -> None:
    flash = improve.build_flash_command("/dev/cu.fixture")
    improve.assert_firmware_only_command(flash)
    assert_true(
        flash
        == [
            "pio",
            "run",
            "-e",
            improve.DEFAULT_ENV,
            "-t",
            "nobuild",
            "-t",
            "upload",
            "--upload-port",
            "/dev/cu.fixture",
            "--disable-auto-clean",
        ],
        "flash argv drifted",
    )
    bench = improve.build_bench_command("/dev/cu.fixture", Path("/tmp/evidence"))
    assert_true("--no-upload" in bench and "--no-baseline" in bench and "--camera" in bench, "bench guard missing")
    board_index = bench.index("--board-id")
    assert_true(
        bench[board_index + 1] == improve.DEFAULT_BOARD_ID,
        "bench command did not pin the planned board id",
    )
    for forbidden in ("-f", "--upload-fs", "uploadfs"):
        assert_true(forbidden not in flash and forbidden not in bench, f"forbidden token present: {forbidden}")


def test_bench_result_arm_identity() -> None:
    identities = {
        "product_fingerprint": "p" * 64,
        "grader_fingerprint": "g" * 64,
        "hardware_scoring_fingerprint": "h" * 64,
        "scenario_fingerprints": {
            "core": "1" * 64,
            "display": "2" * 64,
            "replay": "3" * 64,
        },
    }
    plan = {"base_sha": "b" * 40, "candidate_sha": "c" * 40}

    def fixture(sha: str) -> dict:
        return {
            "schema_version": 4,
            "kind": "bench_result",
            "result": "PASS",
            "git_sha": sha,
            "git_worktree_clean": True,
            **identities,
            "windows": [
                {
                    "suite": suite,
                    "result": "PASS",
                    "window_schema_version": 3,
                    "scenario_fingerprint": identities["scenario_fingerprints"][suite],
                }
                for suite in ("core", "display", "replay")
            ],
        }

    improve.validate_bench_result(
        fixture(plan["base_sha"]),
        arm="baseline",
        arm_index=1,
        plan=plan,
        identities=identities,
    )
    improve.validate_bench_result(
        fixture(plan["candidate_sha"]),
        arm="candidate",
        arm_index=1,
        plan=plan,
        identities=identities,
    )
    expect_error(
        lambda: improve.validate_bench_result(
            fixture(plan["candidate_sha"]),
            arm="baseline",
            arm_index=1,
            plan=plan,
            identities=identities,
        ),
        improve.GateFailure,
        "identity-owned canonical PASS",
    )


def test_file_evidence_is_hash_chained_and_immutable() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "session"
        store = improve.FileEvidenceStore(root)
        store.event("one", {"value": 1})
        store.event("two", {"value": 2})
        lines = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
        assert_true(lines[0]["previous_sha256"] == "0" * 64, "journal genesis changed")
        assert_true(lines[1]["previous_sha256"] == lines[0]["event_sha256"], "journal chain broke")
        for line in lines:
            stored_hash = line.pop("event_sha256")
            assert_true(stored_hash == hashlib.sha256(improve.canonical_bytes(line)).hexdigest(), "event hash invalid")
        decision = {
            "schema_version": 1,
            "kind": "improve_decision",
            "result": "ABORTED_NO_RESTORE",
        }
        store.finish(decision)
        store.finish(decision)
        expect_error(
            lambda: store.finish({**decision, "result": "ACCEPTED"}),
            improve.GateFailure,
            "immutable decision",
        )


def test_terminal_decision_anchors_final_journal_event() -> None:
    plan = improve.dry_plan()
    with tempfile.TemporaryDirectory() as temp:
        cases = (
            ("accepted", improve.FakeAdapter([100] * 5, [90] * 5), "ACCEPTED", "accepted"),
            (
                "rejected",
                improve.FakeAdapter([100] * 5, [100] * 5),
                "REJECTED_NO_CHANGE",
                "rejected",
            ),
            (
                "gate_failure",
                FailingCandidateAdapter([100] * 5, [90] * 5),
                "REJECTED_GATE_FAILURE",
                "terminal_failure",
            ),
        )
        for name, adapter, expected_result, expected_event in cases:
            root = Path(temp) / name
            store = improve.FileEvidenceStore(root)
            decision = improve.execute_experiment(plan, adapter, store)
            lines = [json.loads(line) for line in store.events_path.read_text().splitlines()]
            assert_true(decision["result"] == expected_result, f"{name} result changed")
            assert_true(bool(lines), f"{name} experiment wrote no journal events")
            assert_true(
                lines[-1]["event"] == expected_event,
                f"{name} decision was not preceded by its terminal event",
            )
            assert_true(
                decision["last_event_sha256"] == lines[-1]["event_sha256"],
                f"{name} decision does not anchor the final journal event",
            )
            assert_true(
                decision["last_event_sha256"] == store.state["last_event_sha256"],
                f"{name} decision and durable state disagree on the journal anchor",
            )
            assert_true(
                store.state["event_count"] == len(lines),
                f"{name} durable state does not own the complete terminal journal",
            )


def test_final_torn_journal_tail_is_preserved_and_recoverable() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "session"
        store = improve.FileEvidenceStore(root)
        store.event("durable_prefix", {"value": 1})
        durable_hash = store.state["last_event_sha256"]
        torn_tail = b'{"schema_version":1,"sequence":2,"event":"torn'
        with store.events_path.open("ab") as handle:
            handle.write(torn_tail)
            handle.flush()
            os.fsync(handle.fileno())

        reopened = improve.FileEvidenceStore.open(root)
        assert_true(reopened.state["event_count"] == 1, "torn tail changed the durable event count")
        assert_true(
            reopened.state["last_event_sha256"] == durable_hash,
            "torn tail changed the durable journal anchor",
        )
        reopened.event("recovery_continues")
        verified = improve.FileEvidenceStore.open(root)
        assert_true(verified.state["event_count"] == 2, "journal could not continue after torn-tail recovery")
        torn_evidence = [path for path in root.iterdir() if "torn" in path.name]
        assert_true(
            any(torn_tail in path.read_bytes() for path in torn_evidence if path.is_file()),
            "torn journal bytes were discarded instead of preserved as recovery evidence",
        )

        corrupt_root = Path(temp) / "corrupt-middle"
        corrupt = improve.FileEvidenceStore(corrupt_root)
        corrupt.event("valid_prefix")
        with corrupt.events_path.open("ab") as handle:
            handle.write(b'{"event":"corrupt"\n{}\n')
            handle.flush()
            os.fsync(handle.fileno())
        expect_error(
            lambda: improve.FileEvidenceStore.open(corrupt_root),
            improve.GateFailure,
            "invalid event journal line",
        )


def test_partial_event_append_rolls_back_before_recovery_events() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp) / "session"
        store = improve.FileEvidenceStore(root)
        store.event("durable_prefix")
        before = store.events_path.read_bytes()
        original_write = os.write
        calls = [0]

        def partial_then_full(fd: int, data: object) -> int:
            calls[0] += 1
            if calls[0] == 1:
                view = memoryview(data)  # type: ignore[arg-type]
                return original_write(fd, view[: max(1, len(view) // 2)])
            raise OSError(28, "simulated ENOSPC")

        os.write = partial_then_full  # type: ignore[assignment]
        try:
            expect_error(
                lambda: store.event("would_tear"),
                OSError,
                "simulated ENOSPC",
            )
        finally:
            os.write = original_write  # type: ignore[assignment]
        assert_true(
            store.events_path.read_bytes() == before,
            "failed event append left a corrupt journal tail",
        )
        store.event("recovery_continues")
        reopened = improve.FileEvidenceStore.open(root)
        assert_true(reopened.state["event_count"] == 2, "journal did not survive append rollback")


def test_event_journal_refuses_symlink_and_hardlink_writes() -> None:
    with tempfile.TemporaryDirectory() as temp:
        base = Path(temp)
        root = base / "session"
        store = improve.FileEvidenceStore(root)
        store.event("durable_prefix")
        original = store.events_path.read_bytes()

        outside = base / "outside.txt"
        store.events_path.replace(outside)
        store.events_path.symlink_to(outside)
        expect_error(
            lambda: store.event("must_not_escape"),
            improve.GateFailure,
            "event journal",
        )
        assert_true(outside.read_bytes() == original, "journal symlink modified an outside file")

        store.events_path.unlink()
        os.link(outside, store.events_path)
        expect_error(
            lambda: store.event("must_not_touch_hardlink"),
            improve.GateFailure,
            "ownership is invalid",
        )
        assert_true(outside.read_bytes() == original, "journal hardlink modified an outside file")


def test_control_files_refuse_symlink_and_hardlink_substitution() -> None:
    with tempfile.TemporaryDirectory() as temp:
        base = Path(temp)
        session = base / "session"
        store = improve.FileEvidenceStore(session)
        store.event("durable_prefix")

        outside_state = base / "outside-state.json"
        store.state_path.replace(outside_state)
        store.state_path.symlink_to(outside_state)
        expect_error(
            lambda: improve.FileEvidenceStore.open(session),
            improve.GateFailure,
            "improvement state",
        )
        store.state_path.unlink()
        os.link(outside_state, store.state_path)
        expect_error(
            lambda: improve.FileEvidenceStore.open(session),
            improve.GateFailure,
            "ownership is invalid",
        )
        store.state_path.unlink()
        store.state_path.write_bytes(outside_state.read_bytes())

        decision = {
            "kind": "improve_decision",
            "result": "ABORTED_NO_RESTORE",
        }
        store.finish(decision)
        outside_decision = base / "outside-decision.json"
        store.decision_path.replace(outside_decision)
        store.decision_path.symlink_to(outside_decision)
        expect_error(
            lambda: store.finish(decision),
            improve.GateFailure,
            "immutable improvement decision",
        )
        store.decision_path.unlink()
        os.link(outside_decision, store.decision_path)
        expect_error(
            lambda: store.finish(decision),
            improve.GateFailure,
            "ownership is invalid",
        )

        registry_session = base / "registry-session"
        registry_session.mkdir()
        plan_path = registry_session / "plan.json"
        plan_path.write_text(json.dumps(improve.dry_plan()) + "\n", encoding="utf-8")
        registry = improve.ActiveSessionRegistry(base / "state" / "active.json")
        registry.register(registry_session)
        outside_registry = base / "outside-registry.json"
        registry.path.replace(outside_registry)
        registry.path.symlink_to(outside_registry)
        expect_error(registry.unresolved, improve.InvalidInput, "unreadable")
        registry.path.unlink()
        os.link(outside_registry, registry.path)
        expect_error(registry.unresolved, improve.InvalidInput, "unreadable")

        decision_session = base / "decision-session"
        decision_store = improve.FileEvidenceStore(decision_session)
        plan = {**improve.dry_plan(), "simulated": False}
        dry_bytes = b'{"kind":"improve_dry_run_report","result":"PASS"}\n'
        plan["dry_run_report_sha256"] = improve.sha256_bytes(dry_bytes)
        plan_bytes = json.dumps(plan, indent=2, sort_keys=True).encode("utf-8") + b"\n"
        plan_file = decision_session / "plan.json"
        dry_file = decision_session / "dry_run_report.json"
        plan_file.write_bytes(plan_bytes)
        dry_file.write_bytes(dry_bytes)
        outside_plan = base / "outside-plan.json"
        outside_dry = base / "outside-dry-run.json"
        plan_file.replace(outside_plan)
        dry_file.replace(outside_dry)
        plan_file.symlink_to(outside_plan)
        dry_file.symlink_to(outside_dry)
        original_verify = improve.verify_decision_evidence_files
        improve.verify_decision_evidence_files = lambda *_args, **_kwargs: None  # type: ignore[assignment]
        try:
            expect_error(
                lambda: improve.decision_payload("ACCEPTED", plan, decision_store),
                improve.GateFailure,
                "opened safely",
            )
            tolerant = improve.decision_payload(
                "REJECTED_GATE_FAILURE",
                plan,
                decision_store,
                require_valid_evidence=False,
            )
            assert_true(
                tolerant["evidence_integrity"]["status"] == "FAIL",
                "unowned control files were reported as valid decision evidence",
            )
        finally:
            improve.verify_decision_evidence_files = original_verify  # type: ignore[assignment]


def test_cleanup_failure_evidence_does_not_follow_a_symlink() -> None:
    plan = improve.dry_plan()
    with tempfile.TemporaryDirectory() as temp:
        base = Path(temp)
        root = base / "session"
        store = improve.FileEvidenceStore(root)
        outside = base / "outside-cleanup"
        outside.mkdir()
        (root / "cleanup_failures").symlink_to(outside, target_is_directory=True)
        adapter = RestoreFailsOnceAdapter()
        expect_error(
            lambda: improve.execute_experiment(plan, adapter, store),
            improve.InvalidInput,
            "must not traverse a symlink",
        )
        assert_true(
            not list(outside.iterdir()),
            "cleanup failure evidence escaped the owned session",
        )


def test_active_registry_is_bound_to_exact_plan_hash() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        session = root / "session"
        session.mkdir()
        plan_path = session / "plan.json"
        plan_bytes = json.dumps(improve.dry_plan(), indent=2, sort_keys=True) + "\n"
        plan_path.write_text(plan_bytes, encoding="utf-8")
        registry = improve.ActiveSessionRegistry(root / "state" / "active.json")
        registry.register(session)
        payload = json.loads(registry.path.read_text(encoding="utf-8"))
        assert_true(
            payload["plan_sha256"] == hashlib.sha256(plan_bytes.encode("utf-8")).hexdigest(),
            "active registry did not pin the exact plan bytes",
        )
        assert_true(registry.unresolved() == session.resolve(), "registered session was not unresolved")

        plan_path.write_text(plan_bytes + " ", encoding="utf-8")
        expect_error(
            registry.unresolved,
            improve.InvalidInput,
            "missing or changed",
        )
        plan_path.write_text(plan_bytes, encoding="utf-8")
        (session / "state.json").write_text(
            json.dumps(
                {
                    "candidate_may_be_installed": False,
                    "restore_required": False,
                    "evaluation_cleanup_required": False,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        (session / "decision.json").write_text(
            json.dumps(
                {
                    "schema_version": improve.SCHEMA_VERSION,
                    "kind": "improve_decision",
                    "result": "ABORTED_NO_RESTORE",
                    "base_sha": None,
                    "candidate_sha": None,
                    "plan": {"sha256": hashlib.sha256(plan_bytes.encode("utf-8")).hexdigest()},
                }
            )
            + "\n",
            encoding="utf-8",
        )
        assert_true(registry.unresolved() is None, "terminal session kept the active registry")
        assert_true(not registry.path.exists(), "terminal registry pointer was not removed")


def test_active_registry_refuses_a_substituted_session_directory() -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        session = root / "session"
        session.mkdir()
        plan = improve.dry_plan()
        plan_bytes = json.dumps(plan, indent=2, sort_keys=True) + "\n"
        (session / "plan.json").write_text(plan_bytes, encoding="utf-8")
        (session / "state.json").write_text(
            json.dumps(
                {
                    "candidate_may_be_installed": True,
                    "restore_required": True,
                    "evaluation_cleanup_required": True,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        registry = improve.ActiveSessionRegistry(root / "state" / "active.json")
        registry.register(session)

        original = root / "unsafe-original-session"
        session.replace(original)
        fake = root / "fake-terminal-session"
        fake.mkdir()
        (fake / "plan.json").write_text(plan_bytes, encoding="utf-8")
        (fake / "state.json").write_text(
            json.dumps(
                {
                    "candidate_may_be_installed": False,
                    "restore_required": False,
                    "evaluation_cleanup_required": False,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        plan_hash = hashlib.sha256(plan_bytes.encode("utf-8")).hexdigest()
        (fake / "decision.json").write_text(
            json.dumps(
                {
                    "kind": "improve_decision",
                    "result": "ABORTED_NO_RESTORE",
                    "base_sha": plan.get("base_sha"),
                    "candidate_sha": plan.get("candidate_sha"),
                    "plan": {"sha256": plan_hash},
                }
            )
            + "\n",
            encoding="utf-8",
        )
        session.symlink_to(fake, target_is_directory=True)
        expect_error(
            registry.unresolved,
            improve.InvalidInput,
            "must not traverse a symlink",
        )
        assert_true(
            registry.path.exists(),
            "substituted session directory cleared the recovery pointer",
        )


def test_command_runner_escalates_stubborn_child_from_term_to_kill() -> None:
    class StubbornProcess:
        pid = 424242

        def __init__(self) -> None:
            self.returncode: int | None = None
            self.poll_count = 0

        def poll(self) -> int | None:
            self.poll_count += 1
            if self.poll_count > 20:
                raise AssertionError("CommandRunner waited forever after SIGTERM")
            return self.returncode

        def wait(self, timeout: float | None = None) -> int:
            if self.returncode is None:
                raise subprocess.TimeoutExpired("stubborn", timeout)
            return self.returncode

    process = StubbornProcess()
    signals: list[int] = []
    original_popen = subprocess.Popen
    original_killpg = os.killpg
    clock = [0.0]
    stop = improve.StopController()

    def fake_popen(*_args: object, **_kwargs: object) -> StubbornProcess:
        stop.requested = True
        return process

    def fake_killpg(_pid: int, signum: int) -> None:
        signals.append(signum)
        if signum == signal.SIGKILL:
            process.returncode = -signal.SIGKILL

    def fast_monotonic() -> float:
        clock[0] += 120.0
        return clock[0]

    subprocess.Popen = fake_popen  # type: ignore[assignment]
    os.killpg = fake_killpg  # type: ignore[assignment]
    try:
        with tempfile.TemporaryDirectory() as temp:
            expect_error(
                lambda: improve.CommandRunner(
                    stop,
                    poll_interval_seconds=0.0,
                    termination_grace_seconds=1.0,
                    command_timeout_seconds=10_000.0,
                    monotonic=fast_monotonic,
                    sleeper=lambda _seconds: None,
                ).run(
                    ["stubborn-child"],
                    cwd=Path(temp),
                    log_path=Path(temp) / "child.log",
                ),
                improve.ControllerInterrupted,
                "controlled stop",
            )
    finally:
        subprocess.Popen = original_popen  # type: ignore[assignment]
        os.killpg = original_killpg  # type: ignore[assignment]
    assert_true(
        signals == [signal.SIGTERM, signal.SIGKILL],
        f"stubborn child escalation changed: {signals}",
    )


def test_command_runner_honors_stop_at_spawn_boundaries() -> None:
    original_popen = subprocess.Popen
    spawned = [False]

    def forbidden_popen(*_args: object, **_kwargs: object) -> object:
        spawned[0] = True
        raise AssertionError("child spawned after a pre-existing stop request")

    subprocess.Popen = forbidden_popen  # type: ignore[assignment]
    try:
        stop = improve.StopController()
        stop.requested = True
        with tempfile.TemporaryDirectory() as temp:
            expect_error(
                lambda: improve.CommandRunner(stop).run(
                    ["never-spawn"], cwd=Path(temp), log_path=Path(temp) / "never.log"
                ),
                improve.ControllerInterrupted,
                "controlled stop",
            )
    finally:
        subprocess.Popen = original_popen  # type: ignore[assignment]
    assert_true(not spawned[0], "CommandRunner spawned after a pre-existing stop request")

    class ExitedAsSignalArrived:
        pid = 989898
        returncode = 0

        def __init__(self, stop: improve.StopController) -> None:
            self.stop = stop

        def poll(self) -> int:
            self.stop.requested = True
            return 0

    stop = improve.StopController()
    subprocess.Popen = (  # type: ignore[assignment]
        lambda *_args, **_kwargs: ExitedAsSignalArrived(stop)
    )
    try:
        with tempfile.TemporaryDirectory() as temp:
            expect_error(
                lambda: improve.CommandRunner(stop).run(
                    ["just-exited"], cwd=Path(temp), log_path=Path(temp) / "exited.log"
                ),
                improve.ControllerInterrupted,
                "controlled stop",
            )
    finally:
        subprocess.Popen = original_popen  # type: ignore[assignment]


def main() -> int:
    tests = [
        test_counterbalanced_schedule,
        test_improvement_rule,
        test_candidate_scope,
        test_serial_port_and_live_plan_validation,
        test_ambient_git_and_platformio_overrides_fail_closed,
        test_sanitized_git_environment_cannot_bypass_a_tracked_bash_hook,
        test_repository_fsmonitor_cannot_hide_a_dirty_worktree,
        test_checkout_filter_cannot_hide_changed_tracked_bytes,
        test_host_wide_state_paths_ignore_ambient_home,
        test_campaign_radio_lease_is_exclusive_and_inheritable,
        test_host_wide_leases_reject_an_intermediate_state_symlink,
        test_campaign_radio_lease_remains_owned_by_an_inherited_child,
        test_controller_leases_reject_hardlinked_lock_files,
        test_evaluation_revert_refuses_a_switched_submitted_branch,
        test_evaluation_revert_recovers_exact_staged_base_after_hook_abort,
        test_clean_finalization_commits_base_tree_without_git_revert,
        test_overlapping_measurements_reject_no_improvement_after_real_cleanup,
        test_recovered_cleanup_preserves_the_no_improvement_outcome,
        test_unresolved_cleanup_after_analysis_blocks_and_retains_analysis,
        test_effective_worktree_hook_override_is_rejected,
        test_target_policy,
        test_strict_target_extraction,
        test_suite_artifacts_bind_every_metric_to_the_planned_identity,
        test_final_decision_revalidates_qualification_and_cited_metric_bytes,
        test_resource_contract,
        test_dry_run_is_deterministic_and_offline,
        test_stop_during_final_validation_cannot_publish_acceptance,
        test_candidate_gate_failure_restores_and_reverts,
        test_restore_failure_stays_unresolved_and_retries,
        test_revert_failure_stays_unresolved_and_retries,
        test_recovery_finalizes_evaluation_after_restoring_uncertain_candidate,
        test_published_acceptance_normalizes_flags_without_reverting_candidate,
        test_flash_commands_never_upload_filesystem,
        test_bench_result_arm_identity,
        test_file_evidence_is_hash_chained_and_immutable,
        test_terminal_decision_anchors_final_journal_event,
        test_final_torn_journal_tail_is_preserved_and_recoverable,
        test_partial_event_append_rolls_back_before_recovery_events,
        test_event_journal_refuses_symlink_and_hardlink_writes,
        test_control_files_refuse_symlink_and_hardlink_substitution,
        test_cleanup_failure_evidence_does_not_follow_a_symlink,
        test_active_registry_is_bound_to_exact_plan_hash,
        test_active_registry_refuses_a_substituted_session_directory,
        test_command_runner_escalates_stubborn_child_from_term_to_kill,
        test_command_runner_honors_stop_at_spawn_boundaries,
    ]
    for test in tests:
        test()
    print(f"improve regression tests: PASS ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
