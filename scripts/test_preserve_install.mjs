#!/usr/bin/env node
// No browser/device/network access: exercise the production click handlers.
import assert from 'node:assert/strict';
import { createHash, webcrypto } from 'node:crypto';
import { test } from 'node:test';
import { bindPreserveInstaller } from '../web-installer/preserve-install.mjs';

const image = Uint8Array.of(0xe9, 0, 0, 0);
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
const manifest = {
  builds: [{ chipFamily: 'ESP32-S3', parts: [{
    path: 'firmware-update.bin', offset: 0x20000, size: image.length, sha256: sha256(image),
  }] }],
};

function firmwareResponse(chunks = [image], options = {}) {
  let index = 0;
  const result = {
    ok: options.ok ?? true, status: options.status ?? 200,
    headers: new Headers(options.headers),
    reads: 0, cancelled: false, released: false,
    body: { getReader: () => ({
      async read() {
        result.reads++;
        if (options.readError) throw new Error('download disconnected');
        return index < chunks.length ? { value: chunks[index++], done: false } : { done: true };
      },
      async cancel() { result.cancelled = true; },
      releaseLock() { result.released = true; },
    }) },
  };
  return result;
}

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
  const blobs = new Map();
  const revoked = [];
  const flashedImages = [];
  const port = {
    readable: null, writable: null,
    async close() { calls.push('close'); this.readable = this.writable = null; },
  };
  const options = {
    secure: true,
    crypto: webcrypto,
    serial: { requestPort() { calls.push('requestPort'); return Promise.resolve(port); } },
    async fetchManifest(url) { calls.push(['manifest', url]); return { ok: true, json: async () => manifest }; },
    async fetchFirmware(url) { calls.push(['firmware', url]); return firmwareResponse(); },
    createObjectURL(blob) {
      const url = 'blob:https://example.invalid/verified-image';
      blobs.set(url, blob);
      return url;
    },
    revokeObjectURL(url) { revoked.push(url); blobs.delete(url); },
    async flash(report, selected, path, data, erase) {
      calls.push(['flash', selected, path, data, erase]);
      flashedImages.push(new Uint8Array(await blobs.get(data.builds[0].parts[0].path).arrayBuffer()));
      report({ state: 'writing', message: 'Writing', details: { percentage: 50 } });
      report({ state: 'finished', message: 'Done' });
    },
    ...overrides,
  };
  elements.install.disabled = true;
  bindPreserveInstaller(root, options);
  return { elements, calls, port, blobs, revoked, flashedImages };
}

test('preserve choice confirms before permission, and never offers an erase choice', async () => {
  const { elements, calls, port, revoked, blobs, flashedImages } = fixture();
  elements.install.emit('click');
  assert.equal(elements.dialog.open, true);
  assert.match(elements.status.textContent, /firmware and web interface will be updated together/);
  assert.match(elements.status.textContent, /settings, Wi-Fi networks, profiles, and stored audio will be preserved/);
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
  const verifiedManifest = structuredClone(manifest);
  verifiedManifest.builds[0].parts[0].path = 'blob:https://example.invalid/verified-image';
  assert.deepEqual(calls.slice(1), [
    ['manifest', 'https://example.invalid/installer/manifest-update.json'],
    ['firmware', 'https://example.invalid/installer/firmware-update.bin'],
    ['flash', port, 'https://example.invalid/installer/manifest-update.json', verifiedManifest, false],
  ]);
  assert.deepEqual(flashedImages, [image]);
  assert.deepEqual(revoked, ['blob:https://example.invalid/verified-image']);
  assert.equal(blobs.size, 0);
  assert.equal(manifest.builds[0].parts[0].path, 'firmware-update.bin');
  assert.match(elements.status.textContent, /Transfer complete/);
  assert.match(elements.status.textContent, /firmware and web interface were transferred/);
  assert.match(elements.status.textContent, /Restart the device, then reopen maintenance mode/);
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

test('preservation rejects manifests that could change the write region', async () => {
  const appPart = manifest.builds[0].parts[0];
  for (const builds of [
    [{ chipFamily: 'ESP32-S3', parts: [{ path: 'merged-firmware.bin', offset: 0 }] }],
    [{ chipFamily: 'ESP32-S3', parts: [{ ...appPart, offset: 0 }] }],
    [{ chipFamily: 'ESP32-S3', parts: [appPart, { path: 'littlefs.bin', offset: 0xDA0000 }] }],
    [{ chipFamily: 'ESP32', parts: [appPart] }],
    [manifest.builds[0], manifest.builds[0]],
    [],
  ]) {
    const { elements, calls } = fixture({
      fetchManifest: async () => ({ ok: true, json: async () => ({ builds }) }),
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
    assert.doesNotMatch(setup.elements.status.textContent, /Transfer complete/);
    assert.equal(setup.elements.close.disabled, false);
    if (kind !== 'download') assert.equal(setup.calls.at(-1), 'close');
    assert.equal(setup.revoked.length, kind === 'download' ? 0 : 1);
    assert.equal(setup.blobs.size, 0);
  }
});

test('unsupported or insecure browser cannot enable preservation flashing', () => {
  for (const options of [{ secure: false }, { serial: undefined }, { crypto: null }]) {
    const { elements, calls } = fixture(options);
    assert.equal(elements.install.disabled, true);
    elements.install.emit('click');
    assert.deepEqual(calls, []);
    assert.notEqual(elements.availability.textContent, '');
  }
});

test('missing or malformed firmware identity is rejected before downloading or flashing', async () => {
  for (const metadata of [
    { size: undefined }, { size: 0 }, { size: -1 }, { size: 1.5 }, { size: '4' },
    { size: 0xD80001 }, { sha256: undefined }, { sha256: '' }, { sha256: 'x'.repeat(64) },
  ]) {
    const data = structuredClone(manifest);
    Object.assign(data.builds[0].parts[0], metadata);
    const setup = fixture({ fetchManifest: async () => ({ ok: true, json: async () => data }) });
    setup.elements.install.emit('click');
    await setup.elements.confirm.emit('click');
    assert.match(setup.elements.status.textContent, /valid firmware size and checksum/);
    assert.equal(setup.calls.some(call => ['firmware', 'flash'].includes(call[0])), false);
    assert.equal(setup.blobs.size, 0);
  }
});

test('download failures reject bytes before flashing and release the stream', async () => {
  const cases = [
    ['HTTP failure', firmwareResponse([], { ok: false, status: 503 }), /download failed/],
    ['oversized Content-Length', firmwareResponse([image], { headers: { 'Content-Length': String(0xD80001) } }), /length does not match/],
    ['missing length and oversized stream', firmwareResponse([image, Uint8Array.of(1)]), /exceeds the release size/],
    ['lying length and oversized stream', firmwareResponse([image, Uint8Array.of(1)], { headers: { 'Content-Length': String(image.length) } }), /exceeds the release size/],
    ['single oversized chunk', firmwareResponse([new Uint8Array(0xD80001)]), /exceeds the release size/],
    ['truncated', firmwareResponse([image.subarray(0, 3)]), /incomplete/],
    ['empty', firmwareResponse([]), /incomplete/],
    ['HTTP 200 HTML', firmwareResponse([new TextEncoder().encode('<htm')]), /checksum/],
    ['changed bytes', firmwareResponse([Uint8Array.of(0xe9, 1, 0, 0)]), /checksum/],
    ['read error', firmwareResponse([], { readError: true }), /download disconnected/],
  ];
  for (const [name, response, message] of cases) {
    const setup = fixture({ fetchFirmware: async () => response });
    setup.elements.install.emit('click');
    await setup.elements.confirm.emit('click');
    assert.match(setup.elements.status.textContent, message, name);
    assert.equal(setup.calls.some(call => call[0] === 'flash'), false, name);
    assert.equal(setup.revoked.length, 0, name);
    assert.equal(setup.blobs.size, 0, name);
    if (response.ok) assert.equal(response.released, true, name);
    if (name.includes('oversized') || name === 'read error') assert.equal(response.cancelled, true, name);
    if (name === 'oversized Content-Length') assert.equal(response.reads, 0);
  }
});

test('verification hashes decoded bytes when HTTP compresses the download', async () => {
  const response = firmwareResponse([image.subarray(0, 2), image.subarray(2)], {
    headers: { 'Content-Encoding': 'gzip', 'Content-Length': '24' },
  });
  const setup = fixture({ fetchFirmware: async () => response });
  setup.elements.install.emit('click');
  await setup.elements.confirm.emit('click');
  assert.match(setup.elements.status.textContent, /Transfer complete/);
  assert.deepEqual(setup.flashedImages, [image]);
  assert.equal(response.released, true);
});

test('flashing uses the verified Blob even if the remote image changes, then revokes it', async () => {
  let downloads = 0;
  let remote = image;
  let blobURL;
  let flashed;
  const setup = fixture({
    async fetchFirmware(url) {
      assert.equal(url, 'https://example.invalid/installer/firmware-update.bin');
      downloads++;
      return firmwareResponse([remote]);
    },
    createObjectURL(blob) {
      remote = new TextEncoder().encode('changed server response');
      blobURL = URL.createObjectURL(blob);
      return blobURL;
    },
    revokeObjectURL(url) { assert.equal(url, blobURL); URL.revokeObjectURL(url); },
    async flash(report, port, path, data, erase) {
      assert.equal(erase, false);
      assert.equal(data.builds[0].parts.length, 1);
      assert.equal(data.builds[0].parts[0].offset, 0x20000);
      assert.equal(new URL(data.builds[0].parts[0].path, path).href, blobURL);
      flashed = new Uint8Array(await (await fetch(blobURL)).arrayBuffer());
      report({ state: 'finished' });
    },
  });
  setup.elements.install.emit('click');
  await setup.elements.confirm.emit('click');
  assert.match(setup.elements.status.textContent, /Transfer complete/);
  assert.equal(downloads, 1);
  assert.deepEqual(flashed, image);
  await assert.rejects(fetch(blobURL));
});

test('crypto and Blob creation failures never flash, and close failures still revoke the Blob', async () => {
  for (const options of [
    { crypto: { subtle: { digest: async () => { throw new Error('checksum unavailable'); } } } },
    { createObjectURL() { throw new Error('Blob unavailable'); } },
  ]) {
    const setup = fixture(options);
    setup.elements.install.emit('click');
    await setup.elements.confirm.emit('click');
    assert.match(setup.elements.status.textContent, /Update failed/);
    assert.equal(setup.calls.some(call => call[0] === 'flash'), false);
    assert.equal(setup.blobs.size, 0);
    assert.equal(setup.elements.close.disabled, false);
  }
  const setup = fixture({
    async flash(report, port) { port.readable = {}; throw new Error('transport failed'); },
  });
  setup.port.close = async () => { throw new Error('port stuck'); };
  setup.elements.install.emit('click');
  await setup.elements.confirm.emit('click');
  assert.equal(setup.revoked.length, 1);
  assert.equal(setup.blobs.size, 0);
  assert.match(setup.elements.status.textContent, /Disconnect and reconnect/);
  assert.equal(setup.elements.close.disabled, false);
});

test('a removed device after selection cannot be reported as an unwritten cancellation', async () => {
  const setup = fixture({
    async flash(report, port) {
      port.readable = {};
      throw Object.assign(new Error('device removed after writing'), { name: 'NotFoundError' });
    },
  });
  setup.elements.install.emit('click');
  await setup.elements.confirm.emit('click');
  assert.match(setup.elements.status.textContent, /Update failed: device removed after writing/);
  assert.doesNotMatch(setup.elements.status.textContent, /Nothing was written/);
  assert.equal(setup.revoked.length, 1);
  assert.equal(setup.calls.at(-1), 'close');
});

test('the actual preserve handler leaves boot data, settings and LittleFS untouched', async () => {
  const memory = Buffer.alloc(0x1000000, 0xa5);
  const before = Buffer.from(memory);
  const { elements } = fixture({
    flash: async (report, port, path, data, eraseFirst) => {
      if (eraseFirst) memory.fill(0xff);
      for (const part of data.builds[0].parts) memory.set(image, part.offset);
      report({ state: 'finished' });
    },
  });
  elements.install.emit('click');
  await elements.confirm.emit('click');
  assert.deepEqual(memory.subarray(0, 0x20000), before.subarray(0, 0x20000));
  assert.deepEqual(memory.subarray(0x20000, 0x20004), Buffer.from(image));
  assert.deepEqual(memory.subarray(0x20004), before.subarray(0x20004));
});
