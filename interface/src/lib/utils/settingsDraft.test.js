import { describe, expect, it } from 'vitest';
import { reconcileSettingsDraft } from './settingsDraft.js';

describe('settings draft reconciliation', () => {
    it('accepts canonical values on initial load and for fields unchanged after submission', () => {
        const submitted = { volume: 200, enabled: false };
        const canonical = { volume: 100, enabled: true };
        expect(reconcileSettingsDraft(submitted, null, canonical)).toEqual(canonical);
        expect(reconcileSettingsDraft(submitted, submitted, canonical)).toEqual(canonical);
    });

    it('preserves later scalar edits while applying canonical clamps to untouched fields', () => {
        const submitted = { volume: 200, enabled: true, color: 100, hidden: false };
        const current = { ...submitted, enabled: false, color: 0, hidden: true };
        const canonical = { volume: 100, enabled: true, color: 200, hidden: false };
        expect(reconcileSettingsDraft(current, submitted, canonical)).toEqual({
            volume: 100, enabled: false, color: 0, hidden: true
        });
        expect(current.volume).toBe(200);
        expect(canonical.color).toBe(200);
    });
});
