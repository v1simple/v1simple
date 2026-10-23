// The stock ESP Web Tools dialog offers a chip erase even for app-only images.
// Use its public flash API with an explicit false instead of that dialog.
const APP_OFFSET = 0x20000;
const APP_SIZE = 0xD80000;

async function verifiedFirmware(response, part, crypto) {
  if (!response.ok) throw new Error(`Firmware download failed (${response.status}).`);
  if (!response.body?.getReader) throw new Error('This browser cannot safely read the firmware download.');
  const reader = response.body.getReader();
  let complete = false;
  try {
    const encoding = response.headers.get('Content-Encoding');
    const length = response.headers.get('Content-Length');
    // Fetch decompresses encoded responses; their Content-Length describes the
    // encoded body. The streaming bound below always checks the decoded bytes.
    if ((!encoding || encoding === 'identity') && length !== null &&
        (!/^[0-9]+$/.test(length) || Number(length) !== part.size)) {
      throw new Error('Firmware download length does not match the release. Nothing was written.');
    }
    const data = new Uint8Array(part.size);
    let received = 0;
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      if (!(value instanceof Uint8Array) || value.byteLength > part.size - received) {
        throw new Error('Firmware download exceeds the release size. Nothing was written.');
      }
      data.set(value, received);
      received += value.byteLength;
    }
    complete = true;
    if (received !== part.size) {
      throw new Error('Firmware download is incomplete. Nothing was written.');
    }
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', data));
    const sha256 = Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
    if (sha256 !== part.sha256) {
      throw new Error('Firmware download does not match the release checksum. Nothing was written.');
    }
    return new Blob([data], { type: 'application/octet-stream' });
  } finally {
    if (!complete) {
      try { await reader.cancel(); } catch { /* Release a failed download where possible. */ }
    }
    reader.releaseLock();
  }
}

export function bindPreserveInstaller(root, {
  flash, serial, fetchManifest, fetchFirmware = fetchManifest, secure,
  crypto = globalThis.crypto,
  createObjectURL = blob => URL.createObjectURL(blob),
  revokeObjectURL = url => URL.revokeObjectURL(url),
}) {
  const button = root.querySelector('#preserve-install');
  const dialog = root.querySelector('#preserve-dialog');
  const confirm = root.querySelector('#preserve-confirm');
  const close = root.querySelector('#preserve-close');
  const status = root.querySelector('#preserve-status');
  const progress = root.querySelector('#preserve-progress');
  const availability = root.querySelector('#preserve-availability');
  let busy = false;

  if (!secure || !serial || !crypto?.subtle || typeof dialog.showModal !== 'function') {
    availability.textContent = !secure
      ? 'Open the installer over HTTPS or localhost to allow serial access.'
      : 'Use a current desktop browser with Web Serial and dialog support.';
    return;
  }
  availability.hidden = true;
  button.disabled = false;
  button.addEventListener('click', () => {
    if (busy) return;
    status.textContent = 'The firmware and web interface will be updated together. Saved settings, Wi-Fi networks, profiles, and stored audio will be preserved.';
    progress.hidden = true;
    confirm.hidden = false;
    close.textContent = 'Cancel';
    dialog.showModal();
  });
  close.addEventListener('click', () => { if (!busy) dialog.close(); });
  dialog.addEventListener('cancel', (event) => { if (busy) event.preventDefault(); });

  confirm.addEventListener('click', async () => {
    if (busy) return;
    busy = true;
    confirm.disabled = true;
    close.disabled = true;
    status.textContent = 'Choose your device. Keep the cable connected until the update completes.';
    let port;
    let firmwareURL;
    let flashInvoked = false;
    let failure;
    try {
      // Must run directly inside the click handler, before any await, so the
      // browser's permission prompt retains the user's activation.
      port = await serial.requestPort();
      const manifestPath = new URL(button.dataset.manifest, root.baseURI).href;
      const response = await fetchManifest(manifestPath);
      if (!response.ok) throw new Error(`Firmware manifest download failed (${response.status}).`);
      const manifest = await response.json();
      const build = manifest?.builds?.[0];
      const part = build?.parts?.[0];
      if (manifest?.builds?.length !== 1 || build?.chipFamily !== 'ESP32-S3' ||
          build?.parts?.length !== 1 || part?.path !== 'firmware-update.bin' || part.offset !== APP_OFFSET) {
        throw new Error('The preservation manifest must contain only the application image.');
      }
      if (!Number.isSafeInteger(part.size) || part.size <= 0 || part.size > APP_SIZE ||
          typeof part.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(part.sha256)) {
        throw new Error('The preservation manifest must contain a valid firmware size and checksum.');
      }
      status.textContent = 'Downloading and verifying the firmware and web interface before writing.';
      const firmwareResponse = await fetchFirmware(new URL(part.path, manifestPath).href);
      const firmware = await verifiedFirmware(firmwareResponse, part, crypto);
      firmwareURL = createObjectURL(firmware);
      // The pinned flash API fetches parts itself. Give it only these verified
      // bytes, so a second remote download cannot change the image being written.
      const verifiedManifest = {
        ...manifest,
        builds: [{ ...build, parts: [{ ...part, path: firmwareURL }] }],
      };
      progress.hidden = false;
      progress.removeAttribute('value');
      let finished = false;
      flashInvoked = true;
      await flash((state) => {
        status.textContent = state.message;
        if (state.state === 'writing') progress.value = state.details.percentage;
        if (state.state === 'finished') finished = true;
        // The flasher reports write errors before attempting reset/disconnect,
        // which can throw too. Keep the original failure through that cleanup.
        if (state.state === 'error' && failure === undefined) {
          failure = state.message || 'The device reported a flashing error.';
        }
      }, port, manifestPath, verifiedManifest, false);
      if (failure) throw new Error(failure);
      if (!finished) throw new Error('The update did not report completion.');
      status.textContent = 'Transfer complete. The firmware and web interface were transferred without replacing device data. Restart the device, then reopen maintenance mode to use the updated interface.';
    } catch (error) {
      status.textContent = !port && error.name === 'NotFoundError'
        ? 'No device selected. Nothing was written.'
        : `Update failed: ${failure ?? error.message}`;
      if (flashInvoked) {
        status.textContent += ' The update may be incomplete. Disconnect USB-C, hold Power + GEAR, then reconnect USB-C while holding both buttons to enter bootloader mode. Close this dialog and choose Update and preserve data again. Do not choose Fresh install to recover; it erases saved device data.';
      }
    } finally {
      if (firmwareURL) revokeObjectURL(firmwareURL);
      // flash normally disconnects itself. Also release the port if an
      // unexpected error escaped that routine, without opening it ourselves.
      try {
        if (port && (port.readable || port.writable)) await port.close();
      } catch {
        status.textContent += ' Disconnect and reconnect the USB cable before retrying.';
      }
      busy = false;
      confirm.disabled = false;
      confirm.hidden = true;
      close.disabled = false;
      close.textContent = 'Close';
    }
  });
}
