#!/usr/bin/env python3
"""Offline contract checks for the public V1Simple agent-context bridge."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
HOOK_CONFIG = ROOT / ".codex" / "hooks.json"
WRAPPER = ROOT / ".codex" / "hooks" / "v1simple_context.py"
BENCH_WRAPPER = ROOT / ".codex" / "hooks" / "v1simple_bench_kick.py"
SKILL = ROOT / ".agents" / "skills" / "v1simple-engineering" / "SKILL.md"
SKILL_METADATA = SKILL.parent / "agents" / "openai.yaml"
AGENT_CONTRACT = ROOT / "AGENTS.md"


class ContractError(AssertionError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def hook_context(result: subprocess.CompletedProcess[bytes], expected_event: str) -> str:
    require(result.returncode == 0, f"{expected_event} bridge must fail open")
    require(result.stderr == b"", f"{expected_event} bridge must not write stderr")
    output = json.loads(result.stdout)
    require(set(output) == {"hookSpecificOutput"}, f"{expected_event} output must be strict hook JSON")
    specific = output["hookSpecificOutput"]
    require(specific.get("hookEventName") == expected_event, f"{expected_event} output named the wrong event")
    context = specific.get("additionalContext")
    require(isinstance(context, str) and context, f"{expected_event} context must be non-empty")
    return context


def check_hook_config() -> None:
    config = json.loads(HOOK_CONFIG.read_text(encoding="utf-8"))
    hooks = config.get("hooks")
    require(isinstance(hooks, dict), "hooks.json must contain a hooks object")
    context_events = {"SessionStart", "UserPromptSubmit", "SubagentStart"}
    require(
        set(hooks) == context_events | {"Stop"},
        "hooks.json must configure the three context events and advisory Stop kick",
    )

    session = hooks["SessionStart"]
    require(len(session) == 1, "SessionStart must have one matcher group")
    require(
        session[0].get("matcher") == "startup|resume|clear|compact",
        "SessionStart must cover startup, resume, clear, and compact",
    )
    require(
        "matcher" not in hooks["UserPromptSubmit"][0],
        "UserPromptSubmit must not use an ignored matcher",
    )
    require("matcher" not in hooks["SubagentStart"][0], "SubagentStart must provide baseline context to all agents")

    wrapper_digest = hashlib.sha256(WRAPPER.read_bytes()).hexdigest()
    expected_path = "/.codex/hooks/v1simple_context.py"
    for event in context_events:
        groups = hooks[event]
        require(len(groups) == 1, f"{event} must have one matcher group")
        handlers = groups[0].get("hooks")
        require(isinstance(handlers, list) and len(handlers) == 1, f"{event} must have one handler")
        handler = handlers[0]
        require(handler.get("type") == "command", f"{event} handler must be a command")
        command = handler.get("command", "")
        require("git rev-parse --show-toplevel" in command, f"{event} must resolve the git root")
        require(expected_path in command, f"{event} must invoke the context wrapper")
        digests = re.findall(r"--contract-sha256 ([0-9a-f]{64})", command)
        require(digests == [wrapper_digest], f"{event} must bind trust review to the current wrapper digest")
        require(handler.get("timeout", 0) <= 5, f"{event} timeout must stay bounded")

    stop = hooks["Stop"]
    require(len(stop) == 1 and "matcher" not in stop[0], "Stop must have one matcher-free group")
    handlers = stop[0].get("hooks")
    require(isinstance(handlers, list) and len(handlers) == 1, "Stop must have one handler")
    handler = handlers[0]
    require(handler.get("type") == "command", "Stop handler must be a command")
    command = handler.get("command", "")
    require("git rev-parse --show-toplevel" in command, "Stop must resolve the git root")
    require("/.codex/hooks/v1simple_bench_kick.py" in command, "Stop must invoke the bench kick")
    digest = hashlib.sha256(BENCH_WRAPPER.read_bytes()).hexdigest()
    digests = re.findall(r"--contract-sha256 ([0-9a-f]{64})", command)
    require(digests == [digest], "Stop must bind trust review to the current bench wrapper digest")
    require(handler.get("timeout", 0) <= 2, "Stop kick timeout must stay non-blocking")


def make_public_fixture(base: Path) -> tuple[Path, Path, str]:
    public = base / "v1simple"
    hook_dir = public / ".codex" / "hooks"
    hook_dir.mkdir(parents=True)
    wrapper = hook_dir / WRAPPER.name
    shutil.copy2(WRAPPER, wrapper)
    digest = hashlib.sha256(wrapper.read_bytes()).hexdigest()
    return public, wrapper, digest


def run_wrapper(
    wrapper: Path,
    digest: str,
    public: Path,
    payload: dict[str, object],
    env: dict[str, str],
) -> subprocess.CompletedProcess[bytes]:
    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    run_env = env.copy()
    run_env["EXPECTED_HOOK_SHA256"] = hashlib.sha256(raw).hexdigest()
    return subprocess.run(
        [sys.executable, str(wrapper), "--contract-sha256", digest],
        input=raw,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=public,
        env=run_env,
        check=False,
        timeout=5,
        shell=False,
    )


def check_offline_degradation() -> None:
    with tempfile.TemporaryDirectory(prefix="v1simple-context-offline-") as temporary:
        public, wrapper, digest = make_public_fixture(Path(temporary))
        environment = os.environ.copy()
        environment.pop("V1SIMPLE_INTERNAL_REPO", None)
        secret = "prompt-must-not-appear-in-degraded-output"
        result = run_wrapper(
            wrapper,
            digest,
            public,
            {"hook_event_name": "UserPromptSubmit", "prompt": secret, "cwd": str(public)},
            environment,
        )
        context = hook_context(result, "UserPromptSubmit")
        require("unavailable" in context, "missing private context must be visible")
        require(secret not in context, "degraded output must not echo the prompt")


FAKE_INTERNAL = '''#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import sys

raw = sys.stdin.buffer.read()
payload = json.loads(raw)
args = sys.argv[1:]
event = args[args.index("--event") + 1]
public_repo = Path(args[args.index("--public-repo") + 1])
if payload.get("prompt") == "__large__":
    print("A" * 20000)
else:
    forwarded = hashlib.sha256(raw).hexdigest() == os.environ["EXPECTED_HOOK_SHA256"]
    argv_clean = "sensitive-example" not in args
    print(f"[v1simple-context] event={event}; forwarded={forwarded}; argv_clean={argv_clean}; public={public_repo.name}")
'''


def check_forwarding_and_safety() -> None:
    with tempfile.TemporaryDirectory(prefix="v1simple-context-online-") as temporary:
        base = Path(temporary)
        public, wrapper, digest = make_public_fixture(base)
        internal_script = base / "v1simple-internal" / "scripts" / "v1i.py"
        internal_script.parent.mkdir(parents=True)
        internal_script.write_text(FAKE_INTERNAL, encoding="utf-8")
        environment = os.environ.copy()
        environment.pop("V1SIMPLE_INTERNAL_REPO", None)

        payloads = {
            "SessionStart": {"hook_event_name": "SessionStart", "source": "startup", "cwd": str(public)},
            "UserPromptSubmit": {
                "hook_event_name": "UserPromptSubmit",
                "prompt": "sensitive-example",
                "cwd": str(public),
            },
            "SubagentStart": {
                "hook_event_name": "SubagentStart",
                "agent_type": "worker",
                "cwd": str(public),
            },
        }
        injection_sentinel = base / "prompt-must-not-execute"
        payloads["UserPromptSubmit"]["prompt"] = (
            f"sensitive-example; touch {injection_sentinel}"
        )
        mapped_events = {"SessionStart": "session", "UserPromptSubmit": "prompt", "SubagentStart": "subagent"}
        for hook_event, payload in payloads.items():
            result = run_wrapper(wrapper, digest, public, payload, environment)
            context = hook_context(result, hook_event)
            require(f"event={mapped_events[hook_event]}" in context, f"{hook_event} mapping is wrong")
            require("forwarded=True" in context, f"{hook_event} input was not forwarded unchanged")
            require("argv_clean=True" in context, f"{hook_event} prompt escaped stdin")
        require(not injection_sentinel.exists(), "prompt text must never execute as a shell command")

        large = run_wrapper(
            wrapper,
            digest,
            public,
            {"hook_event_name": "UserPromptSubmit", "prompt": "__large__", "cwd": str(public)},
            environment,
        )
        context = hook_context(large, "UserPromptSubmit")
        require(len(context.encode("utf-8")) <= 8_000, "model-visible context must stay under the public cap")
        require("truncated by the public safety limit" in context, "truncation must be visible")

        stale = run_wrapper(wrapper, "0" * 64, public, payloads["SessionStart"], environment)
        stale_context = hook_context(stale, "SessionStart")
        require("digest is stale" in stale_context, "stale wrapper trust binding must be visible")


FAKE_BENCH = '''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

Path(os.environ["BENCH_TEST_SENTINEL"]).write_text(json.dumps({
    "argv": sys.argv[1:],
    "stdin": sys.stdin.buffer.read().decode("utf-8"),
}), encoding="utf-8")
'''


def check_advisory_bench_kick() -> None:
    with tempfile.TemporaryDirectory(prefix="v1simple-bench-kick-") as temporary:
        base = Path(temporary)
        public = base / "v1simple"
        hook_dir = public / ".codex" / "hooks"
        hook_dir.mkdir(parents=True)
        wrapper = hook_dir / BENCH_WRAPPER.name
        shutil.copy2(BENCH_WRAPPER, wrapper)
        digest = hashlib.sha256(wrapper.read_bytes()).hexdigest()

        internal_script = base / "v1simple-internal" / "scripts" / "bench.py"
        internal_script.parent.mkdir(parents=True)
        internal_script.write_text(FAKE_BENCH, encoding="utf-8")
        sentinel = base / "bench-invocation.json"
        injection = base / "prompt-must-not-execute"
        environment = os.environ.copy()
        environment["BENCH_TEST_SENTINEL"] = str(sentinel)
        environment.pop("V1SIMPLE_INTERNAL_REPO", None)
        raw = json.dumps({
            "hook_event_name": "Stop",
            "prompt": f"do not forward; touch {injection}",
            "cwd": str(public),
        }).encode("utf-8")
        result = subprocess.run(
            [sys.executable, str(wrapper), "--contract-sha256", digest],
            input=raw,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=public,
            env=environment,
            check=False,
            timeout=2,
            shell=False,
        )
        require(result.returncode == 0, "Stop bench kick must fail open")
        require(result.stdout == b"" and result.stderr == b"", "Stop bench kick must stay silent")

        deadline = time.monotonic() + 2
        while not sentinel.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        require(sentinel.exists(), "Stop did not launch the advisory private bench")
        invocation = json.loads(sentinel.read_text(encoding="utf-8"))
        require(
            invocation["argv"] == ["--fast", "--fail-fast", "--no-wait"],
            "Stop must launch only the bounded advisory bench tier",
        )
        require(invocation["stdin"] == "", "Stop must not forward prompt or transcript input")
        require(not injection.exists(), "Stop input must never execute as a shell command")

        sentinel.unlink()
        stale = subprocess.run(
            [sys.executable, str(wrapper), "--contract-sha256", "0" * 64],
            input=raw,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=public,
            env=environment,
            check=False,
            timeout=2,
            shell=False,
        )
        require(stale.returncode == 0 and not sentinel.exists(), "stale Stop trust binding must no-op")


def check_skill_contract() -> None:
    skill = SKILL.read_text(encoding="utf-8")
    require(skill.startswith("---\n"), "SKILL.md must start with YAML frontmatter")
    frontmatter = skill.split("---", 2)[1].lower()
    require("name: v1simple-engineering" in frontmatter, "skill name is wrong")
    triggers = [
        "design",
        "bug fix",
        "refactor",
        "quality",
        "functionality",
        "tests",
        "hardware",
        "protocol",
        "reviews",
        "releases",
    ]
    for trigger in triggers:
        require(trigger in frontmatter, f"skill description must trigger for {trigger}")
    require("before planning" in skill.lower(), "skill must consult context before planning")
    require("lowest practical layer" in skill.lower(), "skill must enforce test-in-the-layer")
    require("degraded-context" in skill.lower(), "skill must define missing-context behavior")

    metadata = SKILL_METADATA.read_text(encoding="utf-8")
    require('display_name: "V1Simple Engineering"' in metadata, "skill display name is wrong")
    require("$v1simple-engineering" in metadata, "default prompt must name the skill")


def check_public_boundary() -> None:
    contract = AGENT_CONTRACT.read_text(encoding="utf-8").lower()
    for phrase in ("standalone", "privacy boundary", "lowest practical layer", "main-only"):
        require(phrase in contract, f"AGENTS.md is missing {phrase!r}")

    created_files = [
        AGENT_CONTRACT,
        HOOK_CONFIG,
        WRAPPER,
        BENCH_WRAPPER,
        SKILL,
        SKILL_METADATA,
        Path(__file__).resolve(),
    ]
    for path in created_files:
        content = path.read_text(encoding="utf-8")
        local_home_prefix = "/" + "Users/"
        require(local_home_prefix not in content, f"{path.relative_to(ROOT)} contains a local absolute path")

    wrapper_source = WRAPPER.read_text(encoding="utf-8")
    require("stderr=subprocess.DEVNULL" in wrapper_source, "private stderr must be suppressed")
    require("shell=False" in wrapper_source, "the private command must use fixed argv without a shell")
    require("logging" not in wrapper_source, "the bridge must not add prompt logging")
    require("transcript_path" not in wrapper_source, "the bridge must never inspect transcripts")
    require("PYTHONDONTWRITEBYTECODE" in wrapper_source, "the bridge must avoid private bytecode writes")
    bench_source = BENCH_WRAPPER.read_text(encoding="utf-8")
    require("shell=True" not in bench_source, "the bench kick must never invoke a shell")
    require("transcript_path" not in bench_source, "the bench kick must never inspect transcripts")
    require("logging" not in bench_source, "the bench kick must not add prompt logging")
    require("stdin=subprocess.DEVNULL" in bench_source, "the bench kick must not forward hook input")


def main() -> int:
    checks = [
        ("hook configuration and digest binding", check_hook_config),
        ("offline degradation", check_offline_degradation),
        ("event forwarding and output safety", check_forwarding_and_safety),
        ("advisory turn-end bench kick", check_advisory_bench_kick),
        ("engineering skill contract", check_skill_contract),
        ("public privacy and independence boundary", check_public_boundary),
    ]
    try:
        for label, check in checks:
            check()
            print(f"PASS: {label}")
    except (ContractError, json.JSONDecodeError, OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"PASS: {len(checks)} agent-context bridge checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
