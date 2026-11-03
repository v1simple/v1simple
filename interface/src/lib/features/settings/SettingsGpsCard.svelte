<script>
    import { onMount } from 'svelte';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import ToggleSetting from '#lib/components/ToggleSetting.svelte';

    const BAUD_OPTIONS = [9600, 38400, 115200];

    let gpsEnabled = $state(false);
    let gpsBaud = $state(9600);
    let message = $state(null);
    let loaded = $state(false);
    let loading = $state(false);
    let saving = $state(false);

    onMount(() => { void retryLoad(); });

    async function fetchGpsConfig() {
        try {
            const res = await fetchJsonWithTimeout('/api/gps/config');
            if (!res.ok) throw new Error(`GPS config request failed with status ${res.status}`);
            const data = res.data;
            if (typeof data?.gpsEnabled !== 'boolean' || !BAUD_OPTIONS.includes(data?.gpsBaud)) {
                throw new Error('Incomplete GPS settings');
            }
            gpsEnabled = data.gpsEnabled;
            gpsBaud = data.gpsBaud;
            loaded = true;
            return true;
        } catch (err) {
            loaded = false;
            console.error('Failed to load GPS config', err);
            message = { type: 'error', text: 'Failed to load GPS settings.' };
            return false;
        }
    }

    async function retryLoad() {
        if (loading || saving) return;
        loading = true;
        message = null;
        await fetchGpsConfig();
        loading = false;
    }

    async function saveField(fields) {
        if (!loaded || loading || saving) return false;
        saving = true;
        message = null;
        let saved = false;
        try {
            const res = await fetchWithTimeout('/api/gps/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(fields)
            });
            saved = res.ok;
            if (!saved) message = { type: 'error', text: 'Failed to save GPS setting.' };
        } catch (_) {
            message = { type: 'error', text: 'Connection error.' };
        }
        const refreshed = await fetchGpsConfig();
        saving = false;
        return saved && refreshed;
    }

    async function handleToggleEnabled(checked) {
        if (!loaded || loading || saving) return;
        gpsEnabled = checked;
        await saveField({ gpsEnabled });
    }

    async function handleBaudChange(event) {
        if (!loaded || loading || saving) return;
        gpsBaud = Number(event.currentTarget.value);
        await saveField({ gpsBaud });
    }

</script>

<div class="surface-card">
    <div class="card-body space-y-4">
        <CardSectionHead
            title="GPS"
            subtitle="Adafruit Ultimate GPS v3 position, speed, and UTC source."
        />

        {#if message}
            <div class="alert alert-{message.type === 'error' ? 'error' : 'success'} text-sm">
                {message.text}
            </div>
        {/if}

        {#if !loaded}
            {#if loading}
                <p class="copy-caption">Loading GPS settings…</p>
            {:else}
                <button class="btn btn-outline btn-sm" onclick={retryLoad}>Retry GPS settings</button>
            {/if}
        {/if}

        {#if loaded}
            <ToggleSetting
                title="Enable GPS"
                description="Activates the GPS module on Serial1 (RX=GPIO1, TX=GPIO5). EN pin not driven — module is always powered via internal pull-up."
                disabled={saving}
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
                    disabled={saving}
                    value={gpsBaud}
                    onchange={handleBaudChange}
                >
                    {#each BAUD_OPTIONS as baud}
                        <option value={baud}>{baud}</option>
                    {/each}
                </select>
            </div>

            {#if gpsEnabled}
                <div class="divider my-1"></div>
                <div class="surface-note copy-muted text-sm">
                    GPS starts after exiting maintenance and returning to normal operation. Fix data
                    supplies product speed and time inputs. Live GPS data is not available in the
                    maintenance interface.
                </div>
            {/if}
        {/if}
    </div>
</div>
