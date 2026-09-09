"""Describe recorded reader availability without changing calibration or verdicts."""


def frequency_capability(calibration):
    """Summarize only the optional calibrated primary-frequency fallback."""
    if not isinstance(calibration, dict) or type(calibration.get("qualified")) is not bool:
        return {"status": "NOT_RECORDED", "reason": "startup calibration availability was not recorded"}
    if calibration["qualified"]:
        return {"status": "AVAILABLE", "reason": None}
    reason = calibration.get("reason")
    reason = " ".join(reason.split()) if isinstance(reason, str) else ""
    return {"status": "UNAVAILABLE", "reason": reason or "startup calibration did not qualify"}


def frequency_capability_summary(calibration):
    """Return one plain line suitable for both capture console and saved report."""
    capability = frequency_capability(calibration)
    if capability["status"] == "AVAILABLE":
        return "Calibrated frequency fallback available."
    if capability["status"] == "NOT_RECORDED":
        return "Calibrated frequency fallback availability was not recorded."
    return "Calibrated frequency fallback unavailable: " + capability["reason"]
