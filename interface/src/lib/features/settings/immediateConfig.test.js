import { describe, expect, it, vi } from 'vitest';
import { createImmediateConfig } from './immediateConfig.svelte.js';

function deferred() {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return { promise, resolve };
}

describe('immediate configuration publication', () => {
    it('captures the admitted fields and rejects competing intent through readback', async () => {
        const writeResult = deferred();
        const readback = deferred();
        const read = vi.fn().mockResolvedValueOnce({ enabled: false }).mockReturnValueOnce(readback.promise);
        let submitted;
        const write = vi.fn(fields => { submitted = fields; return writeResult.promise; });
        const apply = vi.fn();
        const config = createImmediateConfig({ name: 'Example', read, write, validateAndApply: apply });
        expect(await config.save({ enabled: true })).toBe(false);
        expect(write).not.toHaveBeenCalled();
        expect(await config.load()).toBe(true);
        const fields = { enabled: false, duration: 0 };
        const saving = config.save(fields);
        fields.enabled = true;
        fields.duration = 5;
        expect(submitted).toEqual({ enabled: false, duration: 0 });
        expect(await config.save({ enabled: true })).toBe(false);
        expect(await config.load()).toBe(false);
        writeResult.resolve(true);
        await Promise.resolve();
        expect(config.saving).toBe(true);
        expect(config.canEdit).toBe(false);
        readback.resolve({ enabled: false, duration: 0 });
        expect(await saving).toBe(true);
        expect(config.canEdit).toBe(true);
        expect(apply).toHaveBeenLastCalledWith({ enabled: false, duration: 0 });
        expect(read.mock.calls).toEqual([[{ fresh: true }], [{ fresh: true }]]);
        expect(write).toHaveBeenCalledTimes(1);
    });

    it('never applies a read after the instance is retired', async () => {
        const pending = deferred();
        const apply = vi.fn();
        const read = vi.fn(() => pending.promise);
        const write = vi.fn();
        const config = createImmediateConfig({ name: 'Example', read, write, validateAndApply: apply });
        const loading = config.load({ fresh: false });
        config.dispose();
        pending.resolve({ enabled: true });
        expect(await loading).toBe(false);
        expect(apply).not.toHaveBeenCalled();
        expect(config.loaded).toBe(false);
        expect(config.message).toBeNull();
        expect(await config.load()).toBe(false);
        expect(await config.save({ enabled: false })).toBe(false);
        expect(read).toHaveBeenCalledTimes(1);
        expect(write).not.toHaveBeenCalled();
    });

    it('finishes an admitted freshness request after retirement without publishing or success text', async () => {
        const pending = deferred();
        const read = vi.fn().mockResolvedValue({ enabled: false });
        const apply = vi.fn();
        const config = createImmediateConfig({ name: 'Example', read,
            write: () => pending.promise, validateAndApply: apply });
        await config.load({ fresh: false });
        apply.mockClear();
        const saving = config.save({ enabled: true }, 'Setting saved.');
        config.dispose();
        pending.resolve(true);
        expect(await saving).toBe(false);
        expect(read.mock.calls).toEqual([[{ fresh: false }], [{ fresh: true }]]);
        expect(apply).not.toHaveBeenCalled();
        expect(config.message).toBeNull();
        expect(config.canEdit).toBe(false);
    });
});
