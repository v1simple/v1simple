// Canonical readback owns unchanged fields. Compare against the submission for
// its original editor, or the saved baseline for an editor opened later. Keep
// that comparison point across failed reads; these records contain only scalars.
export function reconcileSettingsDraft(current, submitted, canonical) {
    if (!submitted) return canonical;
    return Object.fromEntries(Object.entries(canonical).map(([key, value]) => [
        key, current[key] === submitted[key] ? value : current[key]
    ]));
}
