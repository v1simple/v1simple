<script>
    import { onMount } from 'svelte';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import { fetchWithTimeout } from '$lib/utils/poll';

    const CATEGORY_LABELS = {
        power: 'Power',
        panic: 'Crash & Panic',
        performance: 'Performance',
        alp: 'ALP'
    };
    const CATEGORY_ORDER = ['power', 'panic', 'performance', 'alp'];

    let loading = $state(true);
    let loaded = $state(false);
    let message = $state(null);
    let files = $state([]);
    let truncated = $state(false);
    let lastPoweroffEvidence = $state('');
    let maxDownloadBytes = $state(0);

    onMount(() => {
        void loadLogs();
    });

    function formatBytes(bytes) {
        const value = Number(bytes);
        if (!Number.isFinite(value) || value < 0) return 'Unknown size';
        if (value < 1024) return `${Math.round(value)} B`;

        const units = ['KiB', 'MiB', 'GiB'];
        let scaled = value / 1024;
        let unitIndex = 0;
        while (scaled >= 1024 && unitIndex < units.length - 1) {
            scaled /= 1024;
            unitIndex += 1;
        }
        const precision = scaled >= 10 ? 0 : 1;
        return `${scaled.toFixed(precision)} ${units[unitIndex]}`;
    }

    function fileName(path) {
        const parts = String(path || '').split('/');
        return parts.at(-1) || path;
    }

    function downloadUrl(path) {
        return `/api/diagnostics/log?path=${encodeURIComponent(path)}`;
    }

    function exceedsDownloadLimit(file) {
        const sizeBytes = Number(file?.sizeBytes);
        return maxDownloadBytes > 0 && Number.isFinite(sizeBytes) && sizeBytes > maxDownloadBytes;
    }

    function groupedFiles() {
        const groups = new Map();
        for (const file of files) {
            const category = file.category || 'other';
            if (!groups.has(category)) groups.set(category, []);
            groups.get(category).push(file);
        }

        return [...groups.entries()]
            .sort(([left], [right]) => {
                const leftIndex = CATEGORY_ORDER.indexOf(left);
                const rightIndex = CATEGORY_ORDER.indexOf(right);
                if (leftIndex < 0 && rightIndex < 0) return left.localeCompare(right);
                if (leftIndex < 0) return 1;
                if (rightIndex < 0) return -1;
                return leftIndex - rightIndex;
            })
            .map(([category, entries]) => ({
                category,
                label: CATEGORY_LABELS[category] || category,
                entries
            }));
    }

    async function responseBody(response) {
        try {
            return await response.json();
        } catch {
            return {};
        }
    }

    async function loadLogs() {
        if (loading && loaded) return;

        loading = true;
        message = null;
        try {
            const response = await fetchWithTimeout('/api/diagnostics/logs');
            const data = await responseBody(response);
            if (!response.ok) {
                files = [];
                truncated = false;
                lastPoweroffEvidence = '';
                maxDownloadBytes = 0;
                if (response.status === 409 || data.error === 'maintenance_mode_required') {
                    message = {
                        type: 'warning',
                        text: 'Diagnostics and log downloads are available only in maintenance mode.'
                    };
                } else if (data.error === 'sd_card_unavailable') {
                    message = {
                        type: 'error',
                        text: 'The SD card is unavailable, so diagnostic logs cannot be listed.'
                    };
                } else if (data.error === 'storage_busy') {
                    message = {
                        type: 'warning',
                        text: 'Diagnostic storage is busy. Wait a moment, then refresh.'
                    };
                } else {
                    message = {
                        type: 'error',
                        text:
                            data.message ||
                            `Could not load diagnostic logs (HTTP ${response.status}).`
                    };
                }
                return;
            }

            files = Array.isArray(data.files)
                ? data.files.filter((file) => typeof file?.path === 'string' && file.path)
                : [];
            truncated = data.truncated === true;
            lastPoweroffEvidence =
                typeof data.lastPoweroffEvidence === 'string' ? data.lastPoweroffEvidence : '';
            maxDownloadBytes = Number(data.maxDownloadBytes) || 0;
        } catch {
            files = [];
            truncated = false;
            lastPoweroffEvidence = '';
            maxDownloadBytes = 0;
            message = {
                type: 'error',
                text: 'Could not reach the device diagnostics service.'
            };
        } finally {
            loading = false;
            loaded = true;
        }
    }
</script>

<div class="page-stack">
    <PageHeader
        title="Diagnostics & Logs"
        subtitle="Inspect and download bounded SD-card diagnostics during maintenance mode."
    >
        <button class="btn btn-primary btn-sm" onclick={loadLogs} disabled={loading}>
            {loading ? 'Refreshing…' : 'Refresh'}
        </button>
    </PageHeader>

    <StatusAlert {message} />

    {#if loading && !loaded}
        <div class="surface-card">
            <div class="card-body state-loading stack">
                <span class="loading loading-spinner loading-lg"></span>
                <p class="copy-muted">Loading diagnostics from the SD card…</p>
            </div>
        </div>
    {:else if !message}
        {#if lastPoweroffEvidence}
            <div class="surface-alert alert-info" role="status">
                <div class="min-w-0">
                    <div class="font-semibold">Recent power-off evidence</div>
                    <code class="copy-caption mt-1 block whitespace-pre-wrap break-all"
                        >{lastPoweroffEvidence}</code
                    >
                </div>
            </div>
        {/if}

        {#if truncated}
            <StatusAlert
                message={{
                    type: 'warning',
                    text: 'The file list reached its safety limit. Download or remove older logs from the SD card to see the rest.'
                }}
            />
        {/if}

        {#if files.length === 0}
            <div class="surface-card">
                <div class="card-body">
                    <p class="state-empty">No diagnostic log files were found on the SD card.</p>
                    <p class="copy-caption-soft mt-2">
                        Run the device normally with the relevant logging enabled, then return to
                        maintenance mode and refresh this page.
                    </p>
                </div>
            </div>
        {:else}
            <div class="space-y-4">
                {#each groupedFiles() as group (group.category)}
                    <section class="surface-card" aria-labelledby={`logs-${group.category}`}>
                        <div class="card-body">
                            <div class="flex items-center justify-between gap-3">
                                <h2 id={`logs-${group.category}`} class="text-lg font-semibold">
                                    {group.label}
                                </h2>
                                <span class="badge badge-outline">
                                    {group.entries.length}
                                    {group.entries.length === 1 ? 'file' : 'files'}
                                </span>
                            </div>

                            <div class="divide-y divide-base-300">
                                {#each group.entries as file (file.path)}
                                    <div
                                        class="flex flex-col gap-3 py-3 sm:flex-row sm:items-center sm:justify-between"
                                    >
                                        <div class="min-w-0">
                                            <div class="break-all font-medium">
                                                {fileName(file.path)}
                                            </div>
                                            <div class="copy-caption font-mono break-all">
                                                {file.path}
                                            </div>
                                            <div class="copy-caption-soft">
                                                {formatBytes(file.sizeBytes)}
                                            </div>
                                        </div>
                                        {#if exceedsDownloadLimit(file)}
                                            <button
                                                type="button"
                                                class="btn btn-outline btn-sm shrink-0"
                                                disabled
                                                title={`Exceeds the ${formatBytes(maxDownloadBytes)} device download limit`}
                                            >
                                                Too large
                                            </button>
                                        {:else}
                                            <a
                                                class="btn btn-outline btn-sm shrink-0"
                                                href={downloadUrl(file.path)}
                                                download={fileName(file.path)}
                                            >
                                                Download
                                            </a>
                                        {/if}
                                    </div>
                                {/each}
                            </div>
                        </div>
                    </section>
                {/each}
            </div>
        {/if}

        {#if maxDownloadBytes > 0}
            <p class="copy-caption-soft">
                Each download is limited to {formatBytes(maxDownloadBytes)} by the device.
            </p>
        {/if}
    {/if}
</div>
