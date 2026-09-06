#!/usr/bin/env python3
"""Ordered bounded reading must retain errors, original bytes and coverage."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "bench"))
import encounter_frame_workers as workers


class FrameWorkerTests(unittest.TestCase):
    def test_out_of_order_completions_keep_original_order_and_bounded_input(self):
        released = threading.Event()
        consumed, completed = [], []

        def frames():
            for index in range(20):
                consumed.append(index)
                yield index, bytes([index])

        def read(job):
            index, pixels = job
            if index == 0:
                self.assertTrue(released.wait(2))
            else:
                released.set()
            completed.append(index)
            return index, {"literal": pixels.hex()}, None, None

        with patch.object(workers, "_read", side_effect=read), ThreadPoolExecutor(2) as executor:
            stream = workers._ordered(executor, frames(), 4)
            first = next(stream)
            self.assertEqual(consumed, [0, 1, 2, 3])
            rows = [first, *stream]
        self.assertNotEqual(completed[0], 0)
        self.assertEqual([r[0] for r in rows], list(range(20)))
        self.assertEqual([r[1] for r in rows], [bytes([i]) for i in range(20)])
        self.assertEqual([r[2] for r in rows], [{"literal": bytes([i]).hex()} for i in range(20)])

    def test_reader_refusal_or_exception_does_not_remove_a_frame(self):
        def read(job):
            index, _ = job
            if index == 1:
                return index, None, "reader failed: ValueError: fixture", None
            return index, {"state": "unreadable"}, None, None

        with patch.object(workers, "_read", side_effect=read), ThreadPoolExecutor(2) as executor:
            rows = list(workers._ordered(executor, [(i, bytes([i])) for i in range(3)], 2))
        self.assertEqual([r[0] for r in rows], [0, 1, 2])
        self.assertEqual(rows[1][3], "reader failed: ValueError: fixture")
        self.assertEqual(rows[2][2], {"state": "unreadable"})

    def test_ocr_transport_failure_cannot_be_hidden_by_healthy_workers(self):
        def read(job):
            index, _ = job
            return index, {"state": "readable"}, None, "helper exited" if index == 1 else None

        with patch.object(workers, "_read", side_effect=read), ThreadPoolExecutor(2) as executor:
            stream = workers._ordered(executor, [(i, b"rgb") for i in range(4)], 2)
            self.assertEqual(next(stream)[0], 0)
            with self.assertRaisesRegex(RuntimeError, "OCR failed at frame 1"):
                list(stream)

    def test_decoder_error_after_final_image_is_not_lost(self):
        def frames():
            yield 0, b"rgb"
            raise ValueError("decoder failed after final frame")

        with patch.object(workers, "_read", return_value=(0, {}, None, None)), ThreadPoolExecutor(2) as executor:
            with self.assertRaisesRegex(ValueError, "after final frame"):
                list(workers._ordered(executor, frames(), 2))

    def test_wrong_worker_index_and_crashed_worker_are_analysis_errors(self):
        with ThreadPoolExecutor(2) as executor:
            with patch.object(workers, "_read", return_value=(9, {}, None, None)):
                with self.assertRaisesRegex(RuntimeError, "different frame index"):
                    list(workers._ordered(executor, [(0, b"rgb")], 2))
            with patch.object(workers, "_read", side_effect=RuntimeError("worker died")):
                with self.assertRaisesRegex(RuntimeError, "worker died"):
                    list(workers._ordered(executor, [(0, b"rgb")], 2))

    def test_serial_reader_exception_retains_error_and_closes_source(self):
        closed = []

        def frames():
            try:
                yield 0, b"rgb"
                yield 1, b"next"
            finally:
                closed.append(True)

        with patch("encounter_reader.analysis_session"), patch("encounter_reader.observe", side_effect=ValueError("fixture")):
            stream = workers.observe_frames(frames(), 1, 1, {}, None)
            self.assertEqual(next(stream), (0, b"rgb", None, "reader failed: ValueError: fixture"))
            stream.close()
        self.assertEqual(closed, [True])

    def test_invalid_worker_count_is_rejected(self):
        for count in (0, 9, True, 1.5):
            with self.assertRaises(ValueError):
                list(workers.observe_frames([], 1, 1, {}, None, workers=count))


if __name__ == "__main__":
    unittest.main()
