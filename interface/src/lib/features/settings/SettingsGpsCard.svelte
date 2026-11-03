<script>
    import { onMount } from 'svelte';
    import { fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';
    import { createImmediateConfig } from './immediateConfig.svelte.js';
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import ToggleSetting from '#lib/components/ToggleSetting.svelte';

    const BAUD_OPTIONS = [9600, 38400, 115200];

    let gpsEnabled = $state(false);
    let gpsBaud = $state(9600);
    const config = createImmediateConfig({
        name: 'GPS',
        async read() {
            const response = await fetchJsonWithTimeout('/api/gps/config');
            if (!response.ok) throw new Error(`GPS config request failed with status ${response.status}`);
            return response.data;
        },
        async write(fields) {
            return (await fetchWithTimeout('/api/gps/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(fields)
            })).ok;
        },
        validateAndApply: applyConfig
    });

    onMount(() => {
        void config.load();
        return config.dispose;
    });

    function applyConfig(data) {
        if (typeof data?.gpsEnabled !== 'boolean' || !BAUD_OPTIONS.includes(data?.gpsBaud)) {
            throw new Error('Incomplete GPS settings');
        }
        gpsEnabled = data.gpsEnabled;
        gpsBaud = data.gpsBaud;
    }

    async function handleToggleEnabled(checked) {
        if (!config.canEdit) return;
        gpsEnabled = checked;
        await config.save({ gpsEnabled });
    }

    async function handleBaudChange(event) {
        if (!config.canEdit) return;
        gpsBaud = Number(event.currentTarget.value);
        await config.save({ gpsBaud });
    }

</script>

<div class="surface-card">
    <div class="card-body space-y-4">
        <CardSectionHead
            title="GPS"
            subtitle="Adafruit Ultimate GPS v3 position, speed, and UTC source."
        />

        {#if config.message}
            <div class="alert alert-{config.message.type === 'error' ? 'error' : 'success'} text-sm">
                {config.message.text}
            </div>
        {/if}

        {#if !config.loaded}
            {#if config.loading}
                <p class="copy-caption">Loading GPS settings…</p>
            {:else}
                <button class="btn btn-outline btn-sm" onclick={config.load}>Retry GPS settings</button>
            {/if}
        {/if}

        {#if config.loaded}
            <ToggleSetting
                title="Enable GPS"
                description="Activates the GPS module on Serial1 (RX=GPIO1, TX=GPIO5). EN pin not driven — module is always powered via internal pull-up."
                disabled={config.saving}
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
                    disabled={config.saving}
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
