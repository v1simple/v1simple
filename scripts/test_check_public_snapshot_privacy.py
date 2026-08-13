#!/usr/bin/env python3
"""Regression tests for tracked-snapshot privacy scanning."""

from __future__ import annotations

from pathlib import Path
import struct
import tempfile

import check_public_snapshot_privacy as checker


def git(repo: Path, *arguments: str) -> None:
    completed = checker.subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=False,
        stdout=checker.subprocess.PIPE,
        stderr=checker.subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr.decode("utf-8", errors="replace"))


def git_output(repo: Path, *arguments: str) -> str:
    completed = checker.subprocess.run(
        ["git", "-C", str(repo), *arguments],
        check=False,
        stdout=checker.subprocess.PIPE,
        stderr=checker.subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr.decode("utf-8", errors="replace"))
    return completed.stdout.decode("ascii").strip()


def make_repo(base: Path) -> Path:
    repo = base / "repo"
    repo.mkdir()
    git(repo, "init", "-q")
    git(repo, "config", "user.name", "v1simple")
    git(repo, "config", "user.email", "noreply@example.invalid")
    (repo / "tracked.txt").write_text("safe fixture\n", encoding="utf-8")
    git(repo, "add", "tracked.txt")
    git(repo, "commit", "-q", "-m", "fixture")
    return repo


def private_email() -> bytes:
    return b"person" + b"@" + b"corp.com"


def private_home() -> bytes:
    return b"/Users/" + b"named-account" + b"/project"


def github_token() -> bytes:
    return b"ghp_" + b"A" * 24


def real_mac_address() -> bytes:
    """A live-looking adapter address, split so this file never contains one."""
    return b"a0" + b":f2:62:e3:1f:c4"


def real_device_path() -> bytes:
    return b"/dev/" + b"cu.usbserial-A50285BI"


def public_ip_address() -> bytes:
    return b"93" + b".184.216.34"


def real_credential() -> bytes:
    return b"hunter" + b"2-correct-horse"


def png_with_text_chunk() -> bytes:
    payload = b"Author\x00fixture"
    return (
        b"\x89PNG\r\n\x1a\n"
        + struct.pack(">I", len(payload))
        + b"tEXt"
        + payload
        + b"\x00\x00\x00\x00"
    )


def test_safe_placeholders_and_license_contacts_pass() -> None:
    assert checker.scan_blob("fixture.txt", b"/Users/private/work\nrunner@example.invalid\n") == []
    assert checker.scan_blob("licenses/NOTICE.txt", private_email()) == []


def test_personal_email_and_home_path_are_reported_without_values() -> None:
    data = b"contact=" + private_email() + b"\npath=" + private_home() + b"\n"
    findings = checker.scan_blob("profile.txt", data)
    assert [finding.rule for finding in findings] == [
        "non-public-email",
        "personal-home-path",
    ]
    rendered = "\n".join(finding.render() for finding in findings)
    assert private_email().decode() not in rendered
    assert b"named-account".decode() not in rendered


def test_high_confidence_secret_and_binary_metadata_are_reported() -> None:
    secret_findings = checker.scan_blob("config.txt", b"token=" + github_token())
    assert [finding.rule for finding in secret_findings] == ["github-token"]
    image_findings = checker.scan_blob("logo.png", png_with_text_chunk())
    assert [finding.rule for finding in image_findings] == [
        "unreviewed-binary-media",
        "embedded-png-tEXt-metadata",
    ]


def test_replay_tree_is_source_only() -> None:
    for path in (
        "tools/v1replay/Sources/v1replay/main.swift",
        "tools/v1replay/Tests/v1replayTests/V1ProtocolContractTests.swift",
    ):
        assert checker.scan_blob(path, b"import Foundation\n", local_terms=[]) == []

    for path in (
        "tools/v1replay/Tests/v1replayTests/Nested/ContractTests.swift",
        "tools/v1replay/Tests/v1replayTests/ContractTests.txt",
        "tools/v1replay/Tests/v1replayTests/ContractTests.json",
    ):
        findings = checker.scan_blob(path, b"safe fixture\n", local_terms=[])
        assert [finding.rule for finding in findings] == ["replay-source-only-boundary"]

    private_directory_findings = checker.scan_blob(
        "tools/v1replay/Tests/v1replayTests/captures/ContractTests.swift",
        b"safe fixture\n",
        local_terms=[],
    )
    assert [finding.rule for finding in private_directory_findings] == [
        "tracked-private-data-path",
        "replay-source-only-boundary",
    ]

    findings = checker.scan_blob(
        "tools/v1replay/private-input.json",
        b"{}\n",
        local_terms=[],
    )
    assert [finding.rule for finding in findings] == ["replay-source-only-boundary"]


def test_private_paths_and_renamed_replay_content_are_reported() -> None:
    path_findings = checker.scan_blob(
        ".private/replay-input/data.txt",
        b"private runtime input\n",
        local_terms=[],
    )
    assert [finding.rule for finding in path_findings] == ["tracked-private-data-path"]

    renamed_input = (
        b'{"samples":[{"strength":1,"direction":"F",'
        b'"frequencyGHz":34.7,"timestamp":"placeholder"}]}\n'
    )
    content_findings = checker.scan_blob("notes.txt", renamed_input, local_terms=[])
    assert [finding.rule for finding in content_findings] == ["replay-capture-content"]


def test_fixture_identifiers_are_not_findings() -> None:
    """Ordinary source must not trip the operational rules.

    Every value below is drawn from the tracked tree. A guard that fires on
    these is a guard people learn to bypass, which is worse than no guard.
    """
    benign = (
        b'lastV1Address = "AA:BB:CC:DD:EE:FF";\n'
        b'address: "A4:C1:38:00:11:22",\n'
        b'settings.mac = "11:22:33:44:55:66";\n'
        b"PORT=/dev/cu.usbmodem*\n"
        b"  DEVICE_PORT=/dev/cu.usbmodemXXXX ./scripts/run_device_tests.sh\n"
        b"probe /dev/tty.SLAB_USBtoUART*\n"
        b'ap_ip: "192.168.35.5", sta_ip: "192.168.1.23"\n'
        b"payload.ssid = slot.ssid;\n"
        b"const String ssid = entry[\"ssid\"] | \"\";\n"
        b'ssid: "GarageNet",\n'
        b"maintenanceExitMessage = 'Reboot requested. Reconnect after startup.';\n"
        b"echo \"All requested device suites passed.\"\n"
        b'"version": "1.0.0.0"\n'
    )
    assert checker.scan_blob("fixture.cpp", benign, local_terms=[]) == []


def test_real_operational_identifiers_are_reported_without_values() -> None:
    data = (
        b"mac=" + real_mac_address()
        + b"\nport=" + real_device_path()
        + b"\nhost=" + public_ip_address()
        + b'\npassword = "' + real_credential() + b'"\n'
    )
    findings = checker.scan_blob("leak.txt", data, local_terms=[])
    assert [finding.rule for finding in findings] == [
        "hardware-mac-address",
        "public-ip-address",
        "local-device-path",
        "credential-literal",
    ]
    rendered = "\n".join(finding.render() for finding in findings)
    # The identifier is the thing being protected; only its location may appear.
    for value in (real_mac_address(), real_device_path(), public_ip_address(), real_credential()):
        assert value.decode() not in rendered


def test_credential_allowlist_is_by_digest_and_covers_only_known_fixtures() -> None:
    """Tracked fixture values are exempt; a new value in the same file is not."""
    exempt = checker.scan_blob(
        "interface/src/routes/settings/page.test.js",
        b"password: 'secret123',\n",
        local_terms=[],
    )
    assert exempt == []

    findings = checker.scan_blob(
        "interface/src/routes/settings/page.test.js",
        b'password: "' + real_credential() + b'",\n',
        local_terms=[],
    )
    assert [finding.rule for finding in findings] == ["credential-literal"]

    # Placeholders and interpolations are values nobody needs to protect.
    for value in (b"changeme", b"<your-password>", b"${WIFI_PASSWORD}", b"xxxxxx"):
        assert checker.scan_blob("c.env", b"password=" + value, local_terms=[]) == []


def test_mac_heuristic_separates_fixtures_from_real_addresses() -> None:
    for synthetic in (b"AA:BB:CC:DD:EE:FF", b"00:00:00:00:00:00", b"11:22:33:44:55:66",
                      b"A4:C1:38:00:11:22", b"aa:bb:cc:dd:ee:00"):
        assert checker.mac_is_synthetic(synthetic), synthetic
    for real in (real_mac_address(), b"8c" + b":1f:64:9a:2b:07", b"74" + b":31:e5:f4:c3:c1"):
        assert not checker.mac_is_synthetic(real), real


def test_new_binary_media_requires_an_explicit_public_allowlist_entry() -> None:
    safe_logo = checker.scan_blob(
        "interface/static/branding/v1simple-logo-transparent.png",
        b"public logo bytes",
        local_terms=[],
    )
    assert safe_logo == []

    findings = checker.scan_blob("docs/new-image.png", b"image bytes", local_terms=[])
    assert [finding.rule for finding in findings] == ["unreviewed-binary-media"]


def test_index_mode_scans_staged_content_not_head() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        (repo / "profile.txt").write_bytes(private_email())
        git(repo, "add", "profile.txt")
        assert checker.scan_repository(repo, revision="HEAD") == []
        findings = checker.scan_repository(repo, index=True)
        assert [finding.rule for finding in findings] == ["non-public-email"]


def test_revision_mode_scans_committed_binary_metadata() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        (repo / "logo.png").write_bytes(png_with_text_chunk())
        git(repo, "add", "logo.png")
        git(repo, "commit", "-q", "-m", "image fixture")
        findings = checker.scan_repository(repo, revision="HEAD")
        assert [finding.rule for finding in findings] == [
            "unreviewed-binary-media",
            "embedded-png-tEXt-metadata",
        ]


def test_local_blocklist_matches_are_reported_without_the_term() -> None:
    """A local-term hit must name the blocklist position, never the term.

    The blocklist is private by construction; if a failure message echoed the
    term or the matching source line, the guard would leak exactly what it
    exists to protect. Dummy terms only — never a real one.
    """
    dummy_one = "zzqqx-dummy-alpha"
    dummy_two = "zzqqx-dummy-beta"
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        base = Path(raw)
        repo = make_repo(base)
        blocklist = base / "terms.txt"
        blocklist.write_text(
            f"# comment line is skipped\n{dummy_one}\n\n{dummy_two}\n",
            encoding="utf-8",
        )
        terms = checker.load_local_terms(blocklist)
        # Comments and blank lines hold their slot so numbering matches the file.
        assert terms == [b"", dummy_one.encode(), b"", dummy_two.encode()]

        (repo / "notes.md").write_text(
            f"contact {dummy_two} about the rig\n", encoding="utf-8"
        )
        git(repo, "add", "notes.md")
        findings = checker.scan_repository(repo, index=True, local_terms=terms)

        assert [f.rule for f in findings] == ["local-term #4"]
        rendered = findings[0].render()
        assert rendered == "local-term #4 matched at notes.md:1"
        # The redaction contract: neither term nor source line may appear.
        assert dummy_two not in rendered
        assert dummy_one not in rendered
        assert "contact" not in rendered
        assert "rig" not in rendered


def test_absent_local_blocklist_is_silently_skipped() -> None:
    """Public CI has no blocklist file; that must not fail or warn."""
    missing = Path("/nonexistent/v1simple/privacy_terms.txt")
    assert checker.load_local_terms(missing) == []
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        (repo / "notes.md").write_text("ordinary content\n", encoding="utf-8")
        git(repo, "add", "notes.md")
        assert checker.scan_repository(repo, index=True, local_terms=[]) == []


def test_history_range_catches_a_leak_deleted_from_the_tip() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        base = git_output(repo, "rev-parse", "HEAD")
        (repo / "temporary.txt").write_bytes(private_email())
        git(repo, "add", "temporary.txt")
        git(repo, "commit", "-q", "-m", "temporary change")
        (repo / "temporary.txt").unlink()
        git(repo, "add", "-u")
        git(repo, "commit", "-q", "-m", "remove temporary change")

        assert checker.scan_repository(repo, revision="HEAD", local_terms=[]) == []
        findings = checker.scan_repository(
            repo,
            history_tip="HEAD",
            history_base=base,
            local_terms=[],
        )
        assert "non-public-email" in [finding.rule for finding in findings]


def test_history_range_catches_a_leak_on_a_merged_side_branch() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        base = git_output(repo, "rev-parse", "HEAD")
        primary_branch = git_output(repo, "symbolic-ref", "--short", "HEAD")
        git(repo, "switch", "-q", "-c", "side")
        (repo / "side.txt").write_bytes(private_email())
        git(repo, "add", "side.txt")
        git(repo, "commit", "-q", "-m", "side intermediate")
        (repo / "side.txt").unlink()
        git(repo, "add", "-u")
        git(repo, "commit", "-q", "-m", "side cleanup")
        git(repo, "switch", "-q", primary_branch)
        (repo / "main.txt").write_text("main fixture\n", encoding="utf-8")
        git(repo, "add", "main.txt")
        git(repo, "commit", "-q", "-m", "main change")
        git(repo, "merge", "-q", "--no-ff", "side", "-m", "merge side")

        assert checker.scan_repository(repo, revision="HEAD", local_terms=[]) == []
        findings = checker.scan_repository(
            repo,
            history_tip="HEAD",
            history_base=base,
            local_terms=[],
        )
        assert "non-public-email" in [finding.rule for finding in findings]


def test_export_ignore_cannot_hide_revision_index_or_history_content() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        (repo / ".gitattributes").write_text("hidden.txt export-ignore\n", encoding="utf-8")
        (repo / "hidden.txt").write_bytes(private_email())
        git(repo, "add", ".gitattributes", "hidden.txt")

        assert "non-public-email" in {
            finding.rule for finding in checker.scan_repository(repo, index=True, local_terms=[])
        }
        git(repo, "commit", "-q", "-m", "tracked ignored export")
        assert "non-public-email" in {
            finding.rule for finding in checker.scan_repository(repo, revision="HEAD", local_terms=[])
        }
        assert "non-public-email" in {
            finding.rule
            for finding in checker.scan_repository(repo, history_tip="HEAD", local_terms=[])
        }


def test_replacement_refs_cannot_conceal_the_real_revision() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        (repo / "tracked.txt").write_bytes(private_email())
        git(repo, "add", "tracked.txt")
        git(repo, "commit", "-q", "-m", "unsafe object")
        unsafe_commit = git_output(repo, "rev-parse", "HEAD")
        (repo / "tracked.txt").write_text("safe replacement\n", encoding="utf-8")
        git(repo, "add", "tracked.txt")
        git(repo, "commit", "-q", "-m", "safe object")
        safe_commit = git_output(repo, "rev-parse", "HEAD")
        git(repo, "replace", unsafe_commit, safe_commit)

        findings = checker.scan_repository(repo, revision=unsafe_commit, local_terms=[])
        assert "non-public-email" in [finding.rule for finding in findings]


def test_old_blob_reused_at_a_private_path_is_checked_by_path() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        base = git_output(repo, "rev-parse", "HEAD")
        private_dir = repo / ".private"
        private_dir.mkdir()
        (private_dir / "copied.txt").write_text("safe fixture\n", encoding="utf-8")
        git(repo, "add", ".private/copied.txt")
        git(repo, "commit", "-q", "-m", "reuse existing blob")

        findings = checker.scan_repository(
            repo,
            history_tip="HEAD",
            history_base=base,
            local_terms=[],
        )
        assert [finding.rule for finding in findings] == ["tracked-private-data-path"]
        assert ".private" not in findings[0].render()


def test_history_scans_commit_tag_and_reference_text_without_echoing_values() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        sensitive = private_email().decode("ascii")
        git(repo, "commit", "--allow-empty", "-q", "-m", f"Reviewed-by: {sensitive}")
        git(repo, "tag", "-a", "safe-tag", "-m", f"Contact {sensitive}")
        private_ref = "refs/heads/reviewer-" + sensitive
        findings = checker.scan_repository(
            repo,
            all_history=True,
            ref_names=[private_ref],
            local_terms=[],
        )
        rules = [finding.rule for finding in findings]
        assert rules.count("non-public-email") >= 3
        rendered = "\n".join(finding.render() for finding in findings)
        assert sensitive not in rendered


def test_history_scan_rejects_shallow_repositories() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        base = Path(raw)
        source = make_repo(base)
        clone = base / "shallow"
        completed = checker.subprocess.run(
            ["git", "clone", "-q", "--depth", "1", source.as_uri(), str(clone)],
            check=False,
            stdout=checker.subprocess.PIPE,
            stderr=checker.subprocess.PIPE,
        )
        assert completed.returncode == 0
        try:
            checker.scan_repository(clone, all_history=True, local_terms=[])
        except RuntimeError as exc:
            assert "non-shallow" in str(exc)
        else:  # pragma: no cover - assertion branch
            raise AssertionError("shallow history scan unexpectedly passed")


def test_blob_object_reader_deduplicates_unchanged_content_by_hash() -> None:
    with tempfile.TemporaryDirectory(prefix="snapshot-privacy-") as raw:
        repo = make_repo(Path(raw))
        for index in range(3):
            (repo / f"change-{index}.txt").write_text(f"change {index}\n", encoding="utf-8")
            git(repo, "add", f"change-{index}.txt")
            git(repo, "commit", "-q", "-m", f"change {index}")

        observed_batches: list[list[str]] = []
        original = checker.iter_git_objects

        def observed(repo_path: Path, object_ids: object):
            requested = list(object_ids)  # type: ignore[arg-type]
            observed_batches.append(requested)
            yield from original(repo_path, requested)

        checker.iter_git_objects = observed
        try:
            assert checker.scan_repository(repo, all_history=True, local_terms=[]) == []
        finally:
            checker.iter_git_objects = original

        blob_batch = observed_batches[0]
        assert len(blob_batch) == len(set(blob_batch))
        assert len(blob_batch) < sum(
            len(checker.tree_entries(repo, commit))
            for commit in checker.all_history_commits(repo)
        )


def main() -> int:
    tests = (
        test_safe_placeholders_and_license_contacts_pass,
        test_personal_email_and_home_path_are_reported_without_values,
        test_high_confidence_secret_and_binary_metadata_are_reported,
        test_replay_tree_is_source_only,
        test_private_paths_and_renamed_replay_content_are_reported,
        test_fixture_identifiers_are_not_findings,
        test_real_operational_identifiers_are_reported_without_values,
        test_credential_allowlist_is_by_digest_and_covers_only_known_fixtures,
        test_mac_heuristic_separates_fixtures_from_real_addresses,
        test_new_binary_media_requires_an_explicit_public_allowlist_entry,
        test_index_mode_scans_staged_content_not_head,
        test_revision_mode_scans_committed_binary_metadata,
        test_local_blocklist_matches_are_reported_without_the_term,
        test_absent_local_blocklist_is_silently_skipped,
        test_history_range_catches_a_leak_deleted_from_the_tip,
        test_history_range_catches_a_leak_on_a_merged_side_branch,
        test_export_ignore_cannot_hide_revision_index_or_history_content,
        test_replacement_refs_cannot_conceal_the_real_revision,
        test_old_blob_reused_at_a_private_path_is_checked_by_path,
        test_history_scans_commit_tag_and_reference_text_without_echoing_values,
        test_history_scan_rejects_shallow_repositories,
        test_blob_object_reader_deduplicates_unchanged_content_by_hash,
    )
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"PASS {len(tests)} public snapshot privacy regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
