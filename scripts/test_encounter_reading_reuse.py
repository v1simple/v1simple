#!/usr/bin/env python3
"""Keep literal reuse tied to original capture, method, samples and PNGs."""
from copy import deepcopy
import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench.encounter_reading_reuse import PIXEL_READER_FILES, load_reusable_readings
from bench.encounter_qualification import FIELDS, STATIC_READER_IMPLEMENTATION_FILES


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(root):
    prior, out = root / "prior", root / "new"
    (prior / "method").mkdir(parents=True)
    (prior / "frames").mkdir()
    out.mkdir()
    method = {}
    for name in STATIC_READER_IMPLEMENTATION_FILES:
        path = prior / "method" / name
        path.write_text("fixed method " + name)
        method[name] = digest(path)
    identity = {"capture_id": "same-camera-capture", "capture_manifest_sha256": "b" * 64,
                "runtime_identity": {"git_sha": "1111111", "image_id": "123456789"},
                "camera_artifacts": {"video": {"sha256": "c" * 64, "size_bytes": 100}}}
    identity.update({name + "_sha256": hashlib.sha256(name.encode()).hexdigest()
                     for name in ("window_result", "stimulus", "delivery", "scenario")})
    rows = [{"host_capture_ns": 1000 + 100 * i, "frame_seq": i + 10} for i in range(3)]
    samples = [{"frame_id": f"{i+1:04d}", "video_frame_index": i,
                "capture_ns": rows[i]["host_capture_ns"], "source_frame_seq": rows[i]["frame_seq"]} for i in range(2)]
    selection = {"schema_version": 1, "kind": "authored_event_full_frame_selection", "identity": identity, "samples": samples}
    (prior / "selection.json").write_text(json.dumps(selection))
    records = [{"frame_id": s["frame_id"], "video_frame_index": s["video_frame_index"], "capture_ns": s["capture_ns"],
                "observed": {"fields": {f: {"state": "readable", "value": [1]} for f in FIELDS},
                             "diagnostics": {"original_measurement": True}}} for s in samples]
    image = prior / "frames/000000.png"
    image.write_bytes(b"\x89PNG\r\n\x1a\nretained original bytes")
    result = {"schema_version": 1, "kind": "firmware_visual_behavior", "result": "DIFFERENCES_FOUND", "errors": [],
              "reader_qualification": {"status": "QUALIFIED"}, "reader_method": method, "implementation_sha256": method,
              "evidence": {**identity, "selection_sha256": digest(prior / "selection.json")},
              "samples_index": [{**samples[0], "frame_index": 0, "image": "frames/000000.png", "image_sha256": digest(image)}],
              "events": [{"old_expected_state": "never reused"}]}
    write_records(prior, result, records)
    return prior, out, result, {"identity": deepcopy(identity), "rows": rows}, dict(method), deepcopy(samples), records


def write_result(prior, result):
    (prior / "result.json").write_text(json.dumps(result))


def write_records(prior, result, records):
    with gzip.open(prior / "readings.ndjson.gz", "wt") as handle:
        for record in records:
            handle.write(json.dumps(record) + "\n")
    result["evidence"]["readings_sha256"] = digest(prior / "readings.ndjson.gz")
    write_result(prior, result)


class ReadingReuseTests(unittest.TestCase):
    def test_exact_literals_and_originals_are_reused_without_expected_states(self):
        with tempfile.TemporaryDirectory() as tmp:
            prior, out, result, data, method, samples, records = fixture(Path(tmp))
            unchanged = deepcopy((data, method, samples))
            reused = load_reusable_readings(prior / "result.json", data, method, samples, out)
            self.assertEqual((data, method, samples), unchanged)
            self.assertEqual(reused["readings"], {i: record["observed"] for i, record in enumerate(records)})
            self.assertEqual(set(reused), {"readings", "originals", "provenance"})
            self.assertEqual((out / "frames/000000.png").stat().st_ino, (prior / "frames/000000.png").stat().st_ino)
            self.assertEqual(reused["provenance"]["prior_result_sha256"], digest(prior / "result.json"))
            self.assertEqual(reused["provenance"]["reader_method"], {k: method[k] for k in PIXEL_READER_FILES})
            inventory = reused["provenance"]["dependency_inventory"]
            self.assertEqual({name for group in inventory.values() for name in group}, set(PIXEL_READER_FILES))
            self.assertNotIn("events", reused)

    def test_current_subset_and_new_behavior_rules_do_not_reuse_old_judgments(self):
        with tempfile.TemporaryDirectory() as tmp:
            prior, out, result, data, method, samples, records = fixture(Path(tmp))
            result["result"] = "MEASUREMENT_INCOMPLETE"  # Unknown target; complete literal read stream.
            result["behavior_contract"] = {"obsolete_rule": "must not be consulted"}
            write_result(prior, result)
            method["encounter_behavior.py"] = "new rules do not change pixel readings"
            for name in set(STATIC_READER_IMPLEMENTATION_FILES) - set(PIXEL_READER_FILES):
                method[name] = "new expectations and qualification do not change retained literals"
            reused = load_reusable_readings(prior / "result.json", data, method, samples[1:], out)
            self.assertEqual(reused["readings"], {1: records[1]["observed"]})
            self.assertEqual(reused["originals"], {})
            self.assertNotIn("events", reused)
            self.assertNotIn("behavior_contract", reused)
            self.assertNotIn("expected", json.dumps(reused["readings"]))

    def test_every_pixel_dependency_must_match_current_method_and_retained_bytes(self):
        for name in PIXEL_READER_FILES:
            for changed in ("current_hash", "retained_bytes"):
                with self.subTest(name=name, changed=changed), tempfile.TemporaryDirectory() as tmp:
                    prior, out, result, data, method, samples, records = fixture(Path(tmp))
                    if changed == "current_hash":
                        method[name] = "d" * 64
                    else:
                        (prior / "method" / name).write_text("changed actual reader")
                    with self.assertRaises(ValueError):
                        load_reusable_readings(prior / "result.json", data, method, samples, out)
                    self.assertEqual(list(out.iterdir()), [])

    def test_different_capture_firmware_method_and_unqualified_or_failed_analysis_reject(self):
        for case in ("capture", "manifest", "firmware", "video", "method", "retained_method", "qualification", "error",
                     "window_result", "stimulus", "delivery", "scenario"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                prior, out, result, data, method, samples, records = fixture(Path(tmp))
                if case == "capture": data["identity"]["capture_id"] = "other"
                elif case == "manifest": data["identity"]["capture_manifest_sha256"] = "d" * 64
                elif case == "firmware": data["identity"]["runtime_identity"]["git_sha"] = "2222222"
                elif case == "video": data["identity"]["camera_artifacts"]["video"]["sha256"] = "d" * 64
                elif case == "method": method[PIXEL_READER_FILES[0]] = "d" * 64
                elif case == "retained_method": (prior / "method" / PIXEL_READER_FILES[0]).write_text("changed")
                elif case == "qualification": result["reader_qualification"]["status"] = "REJECTED"
                elif case == "error": result["errors"] = ["interrupted analysis"]
                else: data["identity"][case + "_sha256"] = "d" * 64
                write_result(prior, result)
                with self.assertRaises(ValueError):
                    load_reusable_readings(prior / "result.json", data, method, samples, out)
                self.assertEqual(list(out.iterdir()), [])

    def test_complete_stream_must_match_prior_selection_and_current_original_sidecar(self):
        for case in ("duplicate", "missing", "extra", "capture", "frame_id", "fields", "sidecar", "new_frame", "duplicate_selection", "malformed"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                prior, out, result, data, method, samples, records = fixture(Path(tmp))
                if case == "duplicate": records.append(records[0])
                elif case == "missing": records.pop()
                elif case == "extra": records.append({**records[0], "video_frame_index": 2})
                elif case == "capture": records[0]["capture_ns"] += 1
                elif case == "frame_id": records[0]["frame_id"] = "wrong"
                elif case == "fields": records[0]["observed"]["fields"].pop("main_arrows")
                elif case == "sidecar": data["rows"][0]["frame_seq"] += 1
                elif case == "new_frame": samples.append({"video_frame_index": 2, "capture_ns": 1200, "source_frame_seq": 12})
                elif case == "duplicate_selection": samples.append(samples[0])
                else: records[0] = []
                write_records(prior, result, records)
                with self.assertRaises(ValueError):
                    load_reusable_readings(prior / "result.json", data, method, samples, out)
                self.assertEqual(list(out.iterdir()), [])

    def test_raw_and_selection_hashes_and_original_witness_paths_are_bound(self):
        for case in ("raw_hash", "selection_hash", "png_hash", "path", "symlink", "witness_identity", "output_exists", "truncated"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                prior, out, result, data, method, samples, records = fixture(Path(tmp))
                if case == "raw_hash": result["evidence"]["readings_sha256"] = "d" * 64
                elif case == "selection_hash": result["evidence"]["selection_sha256"] = "d" * 64
                elif case == "png_hash": (prior / "frames/000000.png").write_bytes(b"changed")
                elif case == "path": result["samples_index"][0]["image"] = "../private.png"
                elif case == "symlink":
                    source = prior / "frames/000000.png"
                    source.rename(prior / "other.png")
                    source.symlink_to(prior / "other.png")
                elif case == "witness_identity": result["samples_index"][0]["frame_id"] = "wrong"
                elif case == "output_exists":
                    (out / "frames").mkdir()
                    (out / "frames/000000.png").write_bytes(b"do not replace")
                else:
                    raw = prior / "readings.ndjson.gz"
                    raw.write_bytes(raw.read_bytes()[:-8])
                    result["evidence"]["readings_sha256"] = digest(raw)
                write_result(prior, result)
                with self.assertRaises(ValueError):
                    load_reusable_readings(prior / "result.json", data, method, samples, out)
                if case == "output_exists":
                    self.assertEqual((out / "frames/000000.png").read_bytes(), b"do not replace")
                else:
                    self.assertEqual(list(out.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
