// The stock ESP Web Tools dialog offers a chip erase even for app-only images.
// Use its public flash API with an explicit false instead of that dialog.
export function bindPreserveInstaller(root, { flash, serial, fetchManifest, secure }) {
  const button = root.querySelector('#preserve-install');
  const dialog = root.querySelector('#preserve-dialog');
  const confirm = root.querySelector('#preserve-confirm');
  const close = root.querySelector('#preserve-close');
  const status = root.querySelector('#preserve-status');
  const progress = root.querySelector('#preserve-progress');
  const availability = root.querySelector('#preserve-availability');
  let busy = false;

  if (!secure || !serial || typeof dialog.showModal !== 'function') {
    availability.textContent = !secure
      ? 'Open the installer over HTTPS or localhost to allow serial access.'
      : 'Use a current desktop browser with Web Serial and dialog support.';
    return;
  }
  availability.hidden = true;
  button.disabled = false;
  button.addEventListener('click', () => {
    if (busy) return;
    status.textContent = 'Only firmware will be replaced. Device data and the existing web interface will be preserved.';
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
    try {
      // Must run directly inside the click handler, before any await, so the
      // browser's permission prompt retains the user's activation.
      port = await serial.requestPort();
      const manifestPath = new URL(button.dataset.manifest, root.baseURI).href;
      const response = await fetchManifest(manifestPath);
      if (!response.ok) throw new Error(`Firmware manifest download failed (${response.status}).`);
      const manifest = await response.json();
      const build = manifest.builds?.[0];
      const part = build?.parts?.[0];
      if (manifest.builds?.length !== 1 || build.chipFamily !== 'ESP32-S3' ||
          build.parts?.length !== 1 || part.path !== 'firmware-update.bin' || part.offset !== 0x20000) {
        throw new Error('The preservation manifest must contain only the application image.');
      }
      progress.hidden = false;
      progress.removeAttribute('value');
      let finished = false;
      let failure;
      await flash((state) => {
        status.textContent = state.message;
        if (state.state === 'writing') progress.value = state.details.percentage;
        if (state.state === 'finished') finished = true;
        if (state.state === 'error') failure = state.message;
      }, port, manifestPath, manifest, false);
      if (failure) throw new Error(failure);
      if (!finished) throw new Error('The update did not report completion.');
      status.textContent = 'Update complete. Device data and the existing web interface were preserved.';
    } catch (error) {
      status.textContent = error.name === 'NotFoundError'
        ? 'No device selected. Nothing was written.'
        : `Update failed: ${error.message}`;
    } finally {
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
