#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT = Path(__file__).with_name("check_no_empty_conditionals.py")
SPEC = importlib.util.spec_from_file_location("check_no_empty_conditionals", SCRIPT)
assert SPEC and SPEC.loader
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)


class EmptyConditionalCheckTests(unittest.TestCase):
    def test_rejects_empty_control_flow_bodies(self) -> None:
        source = """
        if (ready) {}
        for (;;) { /* old logging removed */ }
        while (busy) {
        }
        else {}
        """
        self.assertEqual(["if", "for", "while", "else"],
                         [finding.kind for finding in CHECKER.find_empty_bodies(source)])

    def test_accepts_body_with_explicit_reason(self) -> None:
        source = """
        while (queue.pop()) {
            // EMPTY_BODY_OK: popping the item is the drain operation.
        }
        """
        self.assertEqual([], CHECKER.find_empty_bodies(source))

    def test_ignores_non_control_blocks_and_literal_text(self) -> None:
        source = 'void noop() {}\nconst char* sample = "if (x) {}";\n'
        self.assertEqual([], CHECKER.find_empty_bodies(source))

    def test_annotation_requires_a_reason(self) -> None:
        source = "if (ready) { // EMPTY_BODY_OK:\n }"
        self.assertEqual(1, len(CHECKER.find_empty_bodies(source)))


if __name__ == "__main__":
    unittest.main()
