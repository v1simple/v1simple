// Canonical readback owns unchanged fields; input entered after submission remains a draft.
// These settings records contain only scalar fields.
export function reconcileSettingsDraft(current, submitted, canonical) {
    if (!submitted) return canonical;
    return Object.fromEntries(Object.entries(canonical).map(([key, value]) => [
        key, current[key] === submitted[key] ? value : current[key]
    ]));
}
