#!/usr/bin/env node
// No browser/device/network access: exercise the production click handlers.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { bindPreserveInstaller } from '../web-installer/preserve-install.mjs';

const manifest = {
  builds: [{ chipFamily: 'ESP32-S3', parts: [{ path: 'firmware-update.bin', offset: 0x20000 }] }],
};

class Element {
  listeners = {};
  disabled = false;
  hidden = false;
  textContent = '';
  dataset = { manifest: 'manifest-update.json' };
  addEventListener(name, handler) { this.listeners[name] = handler; }
  emit(name, event = {}) { return this.listeners[name]?.(event); }
  showModal() { this.open = true; }
  close() { this.open = false; }
  removeAttribute(name) { delete this[name]; }
}

function fixture(overrides = {}) {
  const elements = Object.fromEntries([
    'install', 'dialog', 'confirm', 'close', 'status', 'progress', 'availability',
  ].map(name => [name, new Element()]));
  const root = {
    baseURI: 'https://example.invalid/installer/',
    querySelector: selector => elements[selector.replace('#preserve-', '')],
  };
  const calls = [];
  const port = {
    readable: null, writable: null,
    async close() { calls.push('close'); this.readable = this.writable = null; },
  };
  const options = {
    secure: true,
    serial: { requestPort() { calls.push('requestPort'); return Promise.resolve(port); } },
    async fetchManifest(url) { calls.push(['manifest', url]); return { ok: true, json: async () => manifest }; },
    async flash(report, selected, path, data, erase) {
      calls.push(['flash', selected, path, data, erase]);
      report({ state: 'writing', message: 'Writing', details: { percentage: 50 } });
      report({ state: 'finished', message: 'Done' });
    },
    ...overrides,
  };
  elements.install.disabled = true;
  bindPreserveInstaller(root, options);
  return { elements, calls, port };
}

test('preserve choice confirms before permission, and never offers an erase choice', async () => {
  const { elements, calls, port } = fixture();
  elements.install.emit('click');
  assert.equal(elements.dialog.open, true);
  assert.deepEqual(calls, []);
  const pending = elements.confirm.emit('click');
  // No await between the click and requestPort; keeps browser activation.
  assert.deepEqual(calls, ['requestPort']);
  assert.equal(elements.close.disabled, true);
  let prevented = false;
  elements.dialog.emit('cancel', { preventDefault() { prevented = true; } });
  assert.equal(prevented, true);
  await elements.confirm.emit('click'); // duplicate confirmation cannot flash twice
  await pending;
  assert.deepEqual(calls.slice(1), [
    ['manifest', 'https://example.invalid/installer/manifest-update.json'],
    ['flash', port, 'https://example.invalid/installer/manifest-update.json', manifest, false],
  ]);
  assert.match(elements.status.textContent, /Update complete/);
  assert.equal(elements.progress.value, 50);
  assert.equal(elements.close.disabled, false);
  assert.equal(elements.confirm.hidden, true);
});

test('cancel before confirmation and cancel device selection write nothing', async () => {
  const cancelled = fixture();
  cancelled.elements.install.emit('click');
  cancelled.elements.close.emit('click');
  assert.equal(cancelled.elements.dialog.open, false);
  assert.deepEqual(cancelled.calls, []);
  const { elements, calls } = fixture({
    serial: { requestPort() { return Promise.reject(Object.assign(new Error(), { name: 'NotFoundError' })); } },
  });
  elements.install.emit('click');
  await elements.confirm.emit('click');
  assert.deepEqual(calls, []);
  assert.match(elements.status.textContent, /Nothing was written/);
  assert.equal(elements.close.disabled, false);
});

test('preservation rejects a fresh image or unexpected application offset', async () => {
  for (const part of [{ path: 'merged-firmware.bin', offset: 0 }, { path: 'firmware-update.bin', offset: 0 }]) {
    const { elements, calls } = fixture({
      fetchManifest: async () => ({ ok: true, json: async () => ({ builds: [{ chipFamily: 'ESP32-S3', parts: [part] }] }) }),
    });
    elements.install.emit('click');
    await elements.confirm.emit('click');
    assert.equal(calls.some(call => call[0] === 'flash'), false);
    assert.match(elements.status.textContent, /must contain only the application/);
  }
});

test('download, transport, and reported flashing errors stay visible and release open ports', async () => {
  for (const kind of ['download', 'throw', 'report', 'incomplete']) {
    const setup = fixture({
      fetchManifest: async () => {
        if (kind === 'download') return { ok: false, status: 503 };
        return { ok: true, json: async () => manifest };
      },
      flash: async (report, port) => {
        port.readable = {};
        if (kind === 'throw') throw new Error('transport disconnected');
        if (kind === 'report') report({ state: 'error', message: 'write failed' });
      },
    });
    setup.elements.install.emit('click');
    await setup.elements.confirm.emit('click');
    assert.match(setup.elements.status.textContent, /Update failed/);
    assert.equal(setup.elements.close.disabled, false);
    if (kind !== 'download') assert.equal(setup.calls.at(-1), 'close');
  }
});

test('unsupported or insecure browser cannot enable preservation flashing', () => {
  for (const options of [{ secure: false }, { serial: undefined }]) {
    const { elements, calls } = fixture(options);
    assert.equal(elements.install.disabled, true);
    elements.install.emit('click');
    assert.deepEqual(calls, []);
    assert.notEqual(elements.availability.textContent, '');
  }
});

test('the actual preserve handler leaves boot data, settings and LittleFS untouched', async () => {
  const memory = Buffer.alloc(0x1000000, 0xa5);
  const before = Buffer.from(memory);
  const image = Buffer.from([0xe9, 0, 0, 0]);
  const { elements } = fixture({
    flash: async (report, port, path, data, eraseFirst) => {
      if (eraseFirst) memory.fill(0xff);
      for (const part of data.builds[0].parts) image.copy(memory, part.offset);
      report({ state: 'finished' });
    },
  });
  elements.install.emit('click');
  await elements.confirm.emit('click');
  assert.deepEqual(memory.subarray(0, 0x20000), before.subarray(0, 0x20000));
  assert.deepEqual(memory.subarray(0x20000, 0x20004), image);
  assert.deepEqual(memory.subarray(0x20004), before.subarray(0x20004));
});
