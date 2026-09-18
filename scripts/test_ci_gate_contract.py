#!/usr/bin/env python3
"""Regression tests for release-critical local CI sequencing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def test_car_build_is_clean_and_uses_one_argument_contract() -> None:
    source = (ROOT / "scripts" / "ci-test.sh").read_text(encoding="utf-8")
    declaration = "CAR_BUILD_ARGS=(-e esp32-s3-car-install)"
    jobs_extension = 'CAR_BUILD_ARGS+=(-j "$PIO_JOBS")'
    install = (
        'run_step "Car install dependencies" "$PIO_CMD" pkg install '
        '-e esp32-s3-car-install'
    )
    clean = (
        'run_step "Car install firmware clean" "$PIO_CMD" run '
        '"${CAR_BUILD_ARGS[@]}" -t clean'
    )
    build = (
        'run_step "Car install firmware build" "$PIO_CMD" run '
        '"${CAR_BUILD_ARGS[@]}"'
    )

    require(source.count(declaration) == 1, "car environment arguments are not singular")
    require(source.count(jobs_extension) == 1, "car build no longer shares the CI job limit")
    require(source.count(install) == 1, "CI must resolve car dependencies before cleaning")
    require(source.count(clean) == 1, "CI must clean the car firmware before building")
    require(source.count(build) == 1, "CI must build the car firmware after cleaning")
    require(source.index(install) < source.index(clean),
            "car dependencies must be present before fail-closed patch hooks run during clean")
    require(source.index(clean) < source.index(build), "car clean must precede car build")


def main() -> int:
    test_car_build_is_clean_and_uses_one_argument_contract()
    print("CI gate sequencing tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
