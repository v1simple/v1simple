<script>
    let { detector = $bindable(), editingSettings, knownFirmware = null } = $props();

    const lightChoices = [
        { value: 'unchanged', label: 'Keep current lights', display: 'unchanged', bluetoothLed: 'unchanged' },
        { value: 'on', label: 'Display on', display: 'on', bluetoothLed: 'unchanged' },
        { value: 'dark', label: 'All lights off', display: 'off', bluetoothLed: 'off' },
        { value: 'dark_bluetooth_on', label: 'Display off, Bluetooth light active', display: 'off', bluetoothLed: 'on' },
        { value: 'dark_bluetooth_unchanged', label: 'Display off, keep Bluetooth light as it is', display: 'off', bluetoothLed: 'unchanged' },
        { value: 'bluetooth_off', label: 'Turn Bluetooth light off', display: 'unchanged', bluetoothLed: 'off', conditional: true },
        { value: 'bluetooth_on', label: 'Keep Bluetooth light active', display: 'unchanged', bluetoothLed: 'on', conditional: true }
    ];
    let lights = $derived(lightChoices.find((choice) =>
        choice.display === detector.display && choice.bluetoothLed === detector.bluetoothLed));

    function selectLights(value) {
        const choice = lightChoices.find((item) => item.value === value);
        if (!choice) return;
        detector.display = choice.display;
        detector.bluetoothLed = choice.bluetoothLed;
    }

    function selectMode(value) {
        detector.modePolicy = value === 'unchanged' ? 'unchanged' : 'value';
        if (value !== 'unchanged') detector.mode = Number(value);
    }

    function selectVolumePolicy(value) {
        detector.volumePolicy = value;
        if (value !== 'temporary') detector.volumeDisconnect = 'restore_saved';
    }
</script>

<section class="surface-panel space-y-4" aria-labelledby="profile-everyday-title">
    <div>
        <h3 id="profile-everyday-title" class="font-semibold">Display & everyday controls</h3>
        <p class="copy-caption">These settings control the Valentine One detector. Choose what this profile changes when applied.</p>
    </div>
    <div class="grid gap-4 sm:grid-cols-2">
        <div class="space-y-2">
            <label class="field-control">
                <span class="field-label">Detector lights</span>
                <select class="select w-full" value={lights?.value ?? 'unsupported'}
                    onchange={(event) => selectLights(event.currentTarget.value)} disabled={!editingSettings}>
                    {#if !lights}<option value="unsupported" disabled>Review saved light settings</option>{/if}
                    {#each lightChoices.filter((choice) => !choice.conditional) as choice}
                        <option value={choice.value}>{choice.label}</option>
                    {/each}
                    <optgroup label="Bluetooth only — display must already be off">
                        {#each lightChoices.filter((choice) => choice.conditional) as choice}
                            <option value={choice.value}>{choice.label}</option>
                        {/each}
                    </optgroup>
                </select>
            </label>
            <p class="copy-caption">
                {#if lights?.value === 'dark'}
                    Turns off both the V1 display and its Bluetooth light. V1Simple continues to show alerts.
                {:else if lights?.conditional}
                    Changes only the Bluetooth light. The V1 display must already be off when this profile is applied.
                {:else if lights?.value === 'dark_bluetooth_unchanged'}
                    The Bluetooth light may stay on. Choose “All lights off” for a fully dark detector.
                {:else if lights?.value === 'on'}
                    Turns on the V1 display. The detector controls its Bluetooth light while the display is on.
                {:else if lights?.value === 'unchanged'}
                    Keeps the detector’s current display and Bluetooth-light settings.
                {:else if !lights}
                    The saved display and Bluetooth-light choices cannot be applied together. Choose a light setting above.
                {/if}
            </p>
            {#if detector.bluetoothLed === 'on' || lights?.value === 'dark_bluetooth_unchanged'}
                <p class="copy-caption">Keeping the Bluetooth light active requires V1 firmware 4.1032 or newer. The detector decides whether it is steady or blinking.</p>
            {/if}
        </div>
        <label class="field-control">
            <span class="field-label">Operating mode</span>
            <select class="select w-full" value={detector.modePolicy === 'value' ? String(detector.mode) : 'unchanged'}
                onchange={(event) => selectMode(event.currentTarget.value)} disabled={!editingSettings}>
                <option value="unchanged">Keep current mode</option>
                <option value="1">All Bogeys (A)</option>
                <option value="2">Logic (l)</option>
                <option value="3">Advanced Logic (L)</option>
            </select>
            {#if knownFirmware && knownFirmware < 41028 && detector.modePolicy === 'value'}
                <span class="text-xs text-warning">Setting the operating mode requires V1 4.1028 or newer.</span>
            {/if}
        </label>
    </div>
    <div class="grid gap-4 sm:grid-cols-3">
        <label class="field-control">
            <span class="field-label">Detector volume</span>
            <select class="select w-full" value={detector.volumePolicy}
                onchange={(event) => selectVolumePolicy(event.currentTarget.value)} disabled={!editingSettings}>
                <option value="unchanged">Keep current volume</option>
                <option value="temporary">Set temporarily</option>
                <option value="saved">Save on V1</option>
            </select>
        </label>
        {#if detector.volumePolicy !== 'unchanged'}
            <label class="field-control">
                <span class="field-label">Main volume (0–9)</span>
                <input class="input w-full" type="number" min="0" max="9" bind:value={detector.mainVolume} disabled={!editingSettings} />
            </label>
            <label class="field-control">
                <span class="field-label">Muted volume (0–9)</span>
                <input class="input w-full" type="number" min="0" max="9" bind:value={detector.mutedVolume} disabled={!editingSettings} />
            </label>
        {/if}
    </div>
    {#if detector.volumePolicy !== 'unchanged'}
        {#if knownFirmware && knownFirmware < 41037}
            <p class="text-xs text-warning">Setting detector volume requires V1 4.1037 or newer.</p>
        {/if}
        <details class="surface-collapse">
            <summary class="collapse-title min-h-0 py-3 text-sm font-semibold">Volume feedback & disconnect behavior</summary>
            <div class="collapse-content grid gap-4 pt-2 sm:grid-cols-2">
                <label class="field-control">
                    <span class="field-label copy-caption">Volume feedback</span>
                    <select class="select select-sm w-full" bind:value={detector.volumeFeedback} disabled={!editingSettings}>
                        <option value="none">Silent</option>
                        <option value="changed_only">Only when the level changes</option>
                        <option value="always">Every time volume is set</option>
                    </select>
                </label>
                <label class="field-control">
                    <span class="field-label copy-caption">After Bluetooth disconnect</span>
                    <select class="select select-sm w-full" bind:value={detector.volumeDisconnect} disabled={!editingSettings || detector.volumePolicy !== 'temporary'}>
                        <option value="restore_saved">Restore saved volume</option>
                        <option value="keep_current">Keep temporary volume</option>
                    </select>
                </label>
                <p class="copy-caption sm:col-span-2">Disconnect behavior applies to temporary volume. Saved volume remains on the detector.</p>
                {#if knownFirmware && knownFirmware < 41038 && detector.volumePolicy === 'temporary' && detector.volumeDisconnect === 'keep_current'}
                    <p class="text-xs text-warning sm:col-span-2">Keeping temporary volume after disconnect requires V1 4.1038 or newer.</p>
                {/if}
            </div>
        </details>
    {/if}
</section>
