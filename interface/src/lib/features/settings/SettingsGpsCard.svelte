<script>
    import { onMount } from 'svelte';
    import { fetchWithTimeout } from '$lib/utils/poll';
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';
    import ToggleSetting from '$lib/components/ToggleSetting.svelte';

    const BAUD_OPTIONS = [9600, 38400, 115200];

    let gpsEnabled = $state(false);
    let gpsBaud = $state(9600);
    let gpsLogUtcToPerf = $state(true);
    let gpsLogUtcToAlp = $state(true);
    let message = $state(null);
    let loaded = $state(false);

    onMount(() => {
        void (async () => {
            await fetchGpsConfig();
            loaded = true;
        })();
    });

    async function fetchGpsConfig() {
        try {
            const res = await fetchWithTimeout('/api/gps/config');
            if (!res.ok) throw new Error(`GPS config request failed with status ${res.status}`);
            const data = await res.json();
            if (typeof data.gpsEnabled === 'boolean') gpsEnabled = data.gpsEnabled;
            if (typeof data.gpsBaud === 'number') gpsBaud = data.gpsBaud;
            if (typeof data.gpsLogUtcToPerf === 'boolean') gpsLogUtcToPerf = data.gpsLogUtcToPerf;
            if (typeof data.gpsLogUtcToAlp === 'boolean') gpsLogUtcToAlp = data.gpsLogUtcToAlp;
        } catch (err) {
            console.error('Failed to load GPS config', err);
            message = { type: 'error', text: 'Failed to load GPS settings.' };
        }
    }

    async function saveField(fields) {
        message = null;
        try {
            const res = await fetchWithTimeout('/api/gps/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(fields)
            });
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to save GPS setting.' };
                await fetchGpsConfig();
                return false;
            }
            return true;
        } catch (_) {
            message = { type: 'error', text: 'Connection error.' };
            await fetchGpsConfig();
            return false;
        }
    }

    async function handleToggleEnabled(checked) {
        gpsEnabled = checked;
        await saveField({ gpsEnabled });
    }

    async function handleBaudChange(event) {
        gpsBaud = Number(event.currentTarget.value);
        await saveField({ gpsBaud });
    }

    async function handleLogPerfChange(checked) {
        gpsLogUtcToPerf = checked;
        await saveField({ gpsLogUtcToPerf: checked });
    }

    async function handleLogAlpChange(checked) {
        gpsLogUtcToAlp = checked;
        await saveField({ gpsLogUtcToAlp: checked });
    }
</script>

<div class="surface-card">
    <div class="card-body space-y-4">
        <CardSectionHead
            title="GPS"
            subtitle="Adafruit Ultimate GPS v3 — attaches UTC timestamps to perf and ALP log rows."
        />

        {#if message}
            <div class="alert alert-{message.type === 'error' ? 'error' : 'success'} text-sm">
                {message.text}
            </div>
        {/if}

        {#if loaded}
            <ToggleSetting
                title="Enable GPS"
                description="Activates the GPS module on Serial1 (RX=GPIO1, TX=GPIO5). EN pin not driven — module is always powered via internal pull-up."
                checked={gpsEnabled}
                onChange={handleToggleEnabled}
            />

            <div class="field-control">
                <label class="label" for="gps-baud">
                    <span class="field-label">Baud rate</span>
                </label>
                <select
                    id="gps-baud"
                    class="select w-32"
                    value={gpsBaud}
                    onchange={handleBaudChange}
                >
                    {#each BAUD_OPTIONS as baud}
                        <option value={baud}>{baud}</option>
                    {/each}
                </select>
            </div>

            <ToggleSetting
                title="Log UTC to perf CSV"
                description="Adds a utc column to every perf CSV row (schema v37). Has no effect when GPS has no fix."
                checked={gpsLogUtcToPerf}
                onChange={handleLogPerfChange}
            />

            <ToggleSetting
                title="Log UTC to ALP CSV"
                description="Adds a utc column to every ALP event row (schema v2). Has no effect when GPS has no fix."
                checked={gpsLogUtcToAlp}
                onChange={handleLogAlpChange}
            />

            {#if gpsEnabled}
                <div class="divider my-1"></div>
                <div class="surface-note copy-muted text-sm">
                    GPS starts after exiting maintenance and returning to normal operation. Fix data
                    is used by the runtime, and valid UTC is written to enabled logs. Live GPS data
                    is not available in the maintenance interface.
                </div>
            {/if}
        {/if}
    </div>
</div>
