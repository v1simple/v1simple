<script>
    import { onMount, onDestroy } from 'svelte';
    import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';
    import ToggleSetting from '$lib/components/ToggleSetting.svelte';
    import { isMaintenance, retainRuntimeStatus } from '$lib/stores/runtimeStatus.svelte.js';

    const BAUD_OPTIONS = [9600, 38400, 115200];

    let gpsEnabled = $state(false);
    let gpsBaud = $state(9600);
    let gpsLogUtcToPerf = $state(true);
    let gpsLogUtcToAlp = $state(true);
    let gpsStatus = $state(null);
    let message = $state(null);
    let loaded = $state(false);
    let statusFetchInFlight = false;

    const statusPoll = createPoll(async () => {
        await fetchGpsStatus();
    }, 3000);

    onMount(() => {
        const releaseRuntimeStatus = retainRuntimeStatus({ needsStatus: true });
        void (async () => {
            await fetchGpsConfig();
            loaded = true;
            syncStatusPoll();
        })();
        return releaseRuntimeStatus;
    });

    onDestroy(() => {
        statusPoll.stop();
    });

    $effect(() => {
        if (loaded) {
            syncStatusPoll();
        }
    });

    function syncStatusPoll() {
        if (gpsEnabled && !$isMaintenance) {
            statusPoll.start();
            return;
        }
        statusPoll.stop();
    }

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

    async function fetchGpsStatus() {
        if (statusFetchInFlight) return;
        statusFetchInFlight = true;
        try {
            const res = await fetchWithTimeout('/api/gps/status');
            if (res.ok) gpsStatus = await res.json();
        } catch (err) {
            console.warn('Failed to poll GPS status', err);
        } finally {
            statusFetchInFlight = false;
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
        const ok = await saveField({ gpsEnabled });
        if (!ok) return;
        if (gpsEnabled && !$isMaintenance) {
            await fetchGpsStatus();
            statusPoll.start();
        } else {
            statusPoll.stop();
            gpsStatus = null;
        }
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

    function fixAgeLabel(ageMs) {
        if (ageMs === undefined || ageMs === null || ageMs < 0) return 'n/a';
        if (ageMs < 2000) return `${ageMs} ms`;
        return `${Math.round(ageMs / 1000)} s`;
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

            {#if gpsEnabled && $isMaintenance}
                <div class="divider my-1"></div>
                <div class="surface-note copy-muted text-sm">
                    Live GPS fix data is not running in maintenance mode. GPS settings remain
                    editable and apply on next normal boot.
                </div>
            {:else if gpsEnabled && gpsStatus}
                <div class="divider my-1"></div>
                <div class="space-y-1 text-sm">
                    <p class="copy-subtle font-medium">Live status</p>
                    <div class="grid grid-cols-2 gap-x-4 gap-y-0.5 copy-caption-soft">
                        <span>Module detected</span>
                        <span>{gpsStatus.moduleDetected ? 'Yes' : 'No'}</span>
                        <span>Fix</span>
                        <span
                            class={gpsStatus.stableHasFix
                                ? 'text-success'
                                : gpsStatus.hasFix
                                  ? 'text-warning'
                                  : 'text-error'}
                        >
                            {gpsStatus.stableHasFix
                                ? 'Stable'
                                : gpsStatus.hasFix
                                  ? 'Acquiring'
                                  : 'None'}
                        </span>
                        <span>Satellites</span>
                        <span>{gpsStatus.satellites}</span>
                        <span>HDOP</span>
                        <span>{gpsStatus.hdop >= 0 ? gpsStatus.hdop.toFixed(1) : 'n/a'}</span>
                        <span>Fix age</span>
                        <span>{fixAgeLabel(gpsStatus.fixAgeMs)}</span>
                        <span>Last sentence age</span>
                        <span
                            >{gpsStatus.lastSentenceAgeMs >= 0
                                ? fixAgeLabel(gpsStatus.lastSentenceAgeMs)
                                : 'n/a'}</span
                        >
                        <span>Sentences parsed</span>
                        <span>{gpsStatus.counters?.sentencesParsed ?? 0}</span>
                        <span>Parse failures</span>
                        <span>{gpsStatus.counters?.parseFailures ?? 0}</span>
                        <span>Checksum failures</span>
                        <span>{gpsStatus.counters?.checksumFailures ?? 0}</span>
                        <span>Bytes read</span>
                        <span>{gpsStatus.counters?.bytesRead ?? 0}</span>
                        <span>Parser active</span>
                        <span>{gpsStatus.parserActive ? 'Yes' : 'No'}</span>
                        <span>Detection timed out</span>
                        <span class={gpsStatus.detectionTimedOut ? 'text-error' : ''}
                            >{gpsStatus.detectionTimedOut ? 'Yes' : 'No'}</span
                        >
                    </div>
                </div>
            {/if}
        {/if}
    </div>
</div>
