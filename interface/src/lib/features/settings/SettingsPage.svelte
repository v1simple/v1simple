<script>
    import { onMount } from 'svelte';
    import { createPoll, fetchWithTimeout } from '$lib/utils/poll';
    import {
        invalidateDeviceSettings,
        refreshDeviceSettings,
        retainDeviceSettings
    } from '$lib/stores/deviceSettings.svelte.js';
    import { postSettingsForm } from '$lib/api/settings';
    import CardSectionHead from '$lib/components/CardSectionHead.svelte';
    import SettingsAutoPowerOffCard from '$lib/features/settings/SettingsAutoPowerOffCard.svelte';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import SettingsBackupCard from '$lib/features/settings/SettingsBackupCard.svelte';
    import * as settingsLazyComponents from '$lib/features/settings/settingsLazyComponents.js';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    let settings = $state({
        ap_ssid: '',
        ap_password: '',
        autoPowerOffMinutes: 0,
        apTimeoutMinutes: 0,
        powerOffSdLog: false
    });

    let loading = $state(true);
    let saving = $state(false);
    let message = $state(null);
    let restoreFile = $state(null);
    let restoring = $state(false);
    let backingUpNow = $state(false);

    // WiFi client (STA) state
    let wifiStatus = $state({
        enabled: false,
        savedSSID: '',
        state: 'disabled',
        connectedSSID: '',
        connectedSlotIndex: null,
        ip: '',
        rssi: 0,
        scanRunning: false
    });
    let savedWifiSlots = $state([]);
    let wifiNetworksLoading = $state(false);
    let wifiNetworkActionInFlight = $state(false);
    let wifiTestingIndex = $state(null);
    let wifiOrdering = $state(false);
    let wifiNetworks = $state([]);
    let wifiScanning = $state(false);
    let showWifiModal = $state(false);
    let selectedNetwork = $state(null);
    let wifiPassword = $state('');
    let wifiScanSaving = $state(false);
    let wifiScanTargetIndex = $state(null);
    let wifiEditor = $state({
        open: false,
        mode: 'add',
        index: null,
        label: '',
        ssid: '',
        password: '',
        priority: 0,
        showPassword: false,
        hasExistingPassword: false
    });
    let wifiPoll = null;
    let wifiStatusFetchInFlight = false;
    let wifiTestRunId = 0;
    let SettingsWifiModalComponent = $state(null);
    let wifiModalLoading = $state(false);
    const WIFI_STATUS_ERROR_TEXT = 'Failed to load WiFi status';
    const WIFI_NETWORKS_ERROR_TEXT = 'Failed to load saved WiFi networks';
    const WIFI_SCAN_START_ERROR_TEXT = 'Failed to start WiFi scan';
    const WIFI_SCAN_ERROR_TEXT = 'Failed to update WiFi scan';
    const WIFI_TEST_POLL_INTERVAL_MS = 1000;
    const WIFI_TEST_STATUS_TIMEOUT_MS = 1500;
    const WIFI_TEST_TIMEOUT_MS = 20_000;
    const RECOGNIZED_BACKUP_TYPES = new Set(['v1simple_backup', 'v1simple_sd_backup']);

    onMount(() => {
        const releaseDeviceSettings = retainDeviceSettings();
        void (async () => {
            await fetchSettings();
            await fetchWifiStatus();
            await fetchSavedWifiNetworks();
        })();

        return () => {
            wifiTestRunId += 1;
            releaseDeviceSettings();
            stopWifiPoll();
        };
    });

    function stopWifiPoll() {
        if (!wifiPoll) return;
        wifiPoll.stop();
        wifiPoll = null;
    }

    function clearMessageText(text) {
        if (message?.text === text) {
            message = null;
        }
    }

    function getBackupValidationError(text) {
        let parsed;

        try {
            parsed = JSON.parse(text);
        } catch (_error) {
            return 'Selected file is not valid JSON.';
        }

        if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
            return 'Backup file must contain a JSON object.';
        }

        if (!RECOGNIZED_BACKUP_TYPES.has(parsed._type)) {
            return 'Selected file is not a V1 Simple settings backup.';
        }

        return null;
    }

    async function fetchSettings({ force = false } = {}) {
        try {
            const data = force ? await invalidateDeviceSettings() : await refreshDeviceSettings();
            if (!data) throw new Error('settings request failed');
            settings = { ...settings, ...data };
        } catch (e) {
            message = { type: 'error', text: 'Failed to load settings' };
        } finally {
            loading = false;
        }
    }

    async function fetchWifiStatus({ reportError = true, timeoutMs = 5000 } = {}) {
        if (wifiStatusFetchInFlight) return null;
        wifiStatusFetchInFlight = true;
        try {
            const res = await fetchWithTimeout('/api/wifi/status', {}, timeoutMs);
            if (res.ok) {
                const data = await res.json();
                wifiStatus = { ...wifiStatus, ...data };
                clearMessageText(WIFI_STATUS_ERROR_TEXT);
                return data;
            } else {
                if (reportError) {
                    message = { type: 'error', text: WIFI_STATUS_ERROR_TEXT };
                }
            }
        } catch (e) {
            if (reportError) {
                message = { type: 'error', text: WIFI_STATUS_ERROR_TEXT };
            }
        } finally {
            wifiStatusFetchInFlight = false;
        }
        return null;
    }

    function normalizeWifiSlot(slot, fallbackIndex = 0) {
        const index = Number.isFinite(Number(slot?.index)) ? Number(slot.index) : fallbackIndex;
        const priority = Number.isFinite(Number(slot?.priority)) ? Number(slot.priority) : index;
        const lastConnectedAtSec = Number.isFinite(Number(slot?.lastConnectedAtSec))
            ? Number(slot.lastConnectedAtSec)
            : 0;

        return {
            index,
            ssid: slot?.ssid || '',
            label: slot?.label || '',
            priority,
            lastConnectedAtSec,
            configured: Boolean(slot?.configured || slot?.ssid),
            hasPassword: Boolean(slot?.hasPassword)
        };
    }

    async function fetchSavedWifiNetworks() {
        wifiNetworksLoading = true;
        try {
            const res = await fetchWithTimeout('/api/wifi/networks');
            if (res.ok) {
                const data = await res.json();
                savedWifiSlots = Array.isArray(data.slots)
                    ? data.slots.map((slot, index) => normalizeWifiSlot(slot, index))
                    : [];
                clearMessageText(WIFI_NETWORKS_ERROR_TEXT);
            } else {
                message = { type: 'error', text: WIFI_NETWORKS_ERROR_TEXT };
            }
        } catch (e) {
            message = { type: 'error', text: WIFI_NETWORKS_ERROR_TEXT };
        } finally {
            wifiNetworksLoading = false;
        }
    }

    function slotConfigured(slot) {
        return Boolean(slot?.configured || slot?.ssid);
    }

    function isSlotConnected(slot) {
        if (!slotConfigured(slot) || wifiStatus.state !== 'connected') return false;

        const index = currentConnectedSlotIndex();
        if (index !== null) {
            return Number(slot.index) === index;
        }

        return Boolean(wifiStatus.connectedSSID && slot.ssid === wifiStatus.connectedSSID);
    }

    function currentConnectedSlotIndex() {
        const index = Number(wifiStatus.connectedSlotIndex);
        return Number.isInteger(index) && index >= 0 ? index : null;
    }

    function currentConnectedSlot() {
        const index = currentConnectedSlotIndex();
        if (index === null) return null;
        return savedWifiSlots.find((slot) => Number(slot.index) === index) || null;
    }

    function currentConnectedSlotText() {
        const slot = currentConnectedSlot();
        if (!slot) return '';
        return `Slot ${slot.index + 1}${slot.label ? ` (${slot.label})` : ''}`;
    }

    function wifiConnectedStatusText() {
        const slot = currentConnectedSlot();
        const ssid = wifiStatus.connectedSSID || slot?.ssid || 'network';
        const slotText = currentConnectedSlotText();
        const parts = [`Connected to ${ssid}`];
        if (slotText) parts.push(slotText);
        if (wifiStatus.ip) parts.push(wifiStatus.ip);
        if (Number.isFinite(Number(wifiStatus.rssi))) parts.push(`${wifiStatus.rssi} dBm`);
        return parts.join(' • ');
    }

    function sortedConfiguredWifiSlots() {
        return savedWifiSlots
            .filter(slotConfigured)
            .sort((a, b) => a.priority - b.priority || a.index - b.index);
    }

    function sortedWifiSlots() {
        return [...savedWifiSlots].sort((a, b) => {
            const aConnected = isSlotConnected(a);
            const bConnected = isSlotConnected(b);
            if (aConnected !== bConnected) return aConnected ? -1 : 1;

            const aConfigured = slotConfigured(a);
            const bConfigured = slotConfigured(b);
            if (aConfigured !== bConfigured) return aConfigured ? -1 : 1;
            if (aConfigured && bConfigured) {
                return a.priority - b.priority || a.index - b.index;
            }
            return a.index - b.index;
        });
    }

    function hasConfiguredWifiSlots() {
        return savedWifiSlots.some(slotConfigured);
    }

    function nextWifiPriority() {
        const configured = sortedConfiguredWifiSlots();
        if (configured.length === 0) return 0;
        return Math.min(255, Math.max(...configured.map((slot) => slot.priority)) + 1);
    }

    function moveDisabled(slot, direction) {
        const configured = sortedConfiguredWifiSlots();
        const position = configured.findIndex((candidate) => candidate.index === slot.index);
        if (position < 0) return true;
        return direction < 0 ? position === 0 : position === configured.length - 1;
    }

    async function startWifiScan(targetIndex = wifiScanTargetIndex) {
        if (targetIndex === null || typeof targetIndex === 'number') {
            wifiScanTargetIndex = targetIndex;
        }
        wifiScanning = true;
        wifiNetworks = [];
        showWifiModal = true;
        void ensureWifiModalLoaded();

        // Start polling for scan results
        stopWifiPoll();
        wifiPoll = createPoll(async () => {
            await pollWifiScan();
        }, 1000);
        wifiPoll.start();

        try {
            const res = await fetchWithTimeout('/api/wifi/scan', { method: 'POST' });
            if (!res.ok) {
                message = { type: 'error', text: WIFI_SCAN_START_ERROR_TEXT };
                wifiScanning = false;
                stopWifiPoll();
                closeWifiModal({ force: true });
                return;
            }

            const data = await res.json();
            clearMessageText(WIFI_SCAN_START_ERROR_TEXT);
            applyWifiScanResponse(data);
        } catch (e) {
            message = { type: 'error', text: WIFI_SCAN_START_ERROR_TEXT };
            wifiScanning = false;
            stopWifiPoll();
            closeWifiModal({ force: true });
        }
    }

    function applyWifiScanResponse(data) {
        const networks = Array.isArray(data?.networks) ? data.networks : [];
        if (data?.scanning) {
            wifiScanning = true;
            return;
        }

        // POST may return a scan that completed while the modal was closed. Keep
        // that response instead of waiting for a GET after firmware has released
        // its one-shot scan buffer.
        // An already-scheduled GET can observe the now-released firmware buffer
        // after this POST supplied the completed result. Do not let that empty
        // follow-up erase the result the user has not had a chance to use yet.
        if (networks.length > 0 || wifiNetworks.length === 0) {
            wifiNetworks = networks;
        }
        wifiScanning = false;
        stopWifiPoll();
    }

    async function pollWifiScan() {
        try {
            const res = await fetchWithTimeout('/api/wifi/scan');
            if (res.ok) {
                const data = await res.json();
                clearMessageText(WIFI_SCAN_ERROR_TEXT);
                applyWifiScanResponse(data);
            } else {
                message = { type: 'error', text: WIFI_SCAN_ERROR_TEXT };
                wifiScanning = false;
                stopWifiPoll();
                closeWifiModal({ force: true });
            }
        } catch (e) {
            message = { type: 'error', text: WIFI_SCAN_ERROR_TEXT };
            wifiScanning = false;
            stopWifiPoll();
            closeWifiModal({ force: true });
        }

        // Also update status
        await fetchWifiStatus();
    }

    function selectNetwork(network) {
        selectedNetwork = network;
        wifiPassword = '';
    }

    async function saveSelectedScanNetwork() {
        if (!selectedNetwork || wifiScanSaving || (selectedNetwork.secure && !wifiPassword)) return;

        const ssid = selectedNetwork.ssid;
        const password = wifiPassword;
        wifiScanSaving = true;

        try {
            const body = {
                ssid,
                priority: nextWifiPriority()
            };
            if (wifiScanTargetIndex !== null) {
                body.index = wifiScanTargetIndex;
            }
            if (selectedNetwork.secure || password) {
                body.password = password;
            }

            const res = await fetchWithTimeout('/api/wifi/networks', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });

            if (!res.ok) {
                message = { type: 'error', text: 'Failed to save WiFi network' };
                return;
            }

            message = { type: 'success', text: `Saved ${ssid}` };
            closeWifiModal({ force: true });
            await fetchSavedWifiNetworks();
        } catch (e) {
            message = { type: 'error', text: 'Failed to save WiFi network' };
        } finally {
            wifiScanSaving = false;
        }
    }

    async function disconnectWifi() {
        try {
            const res = await fetchWithTimeout('/api/wifi/disconnect', { method: 'POST' });
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to disconnect' };
                return;
            }
            await fetchWifiStatus();
            message = { type: 'success', text: 'Disconnected from WiFi' };
        } catch (e) {
            message = { type: 'error', text: 'Failed to disconnect' };
        }
    }

    async function toggleWifiClient(enabled) {
        try {
            const res = await fetchWithTimeout('/api/wifi/enable', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ enabled })
            });
            if (res.ok) {
                await fetchWifiStatus();
                await fetchSavedWifiNetworks();
                message = {
                    type: 'success',
                    text: enabled ? 'WiFi client enabled' : 'WiFi client disabled'
                };
            } else {
                message = { type: 'error', text: 'Failed to change WiFi setting' };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        }
    }

    function closeWifiModal({ force = false } = {}) {
        if (wifiScanSaving && !force) return;

        showWifiModal = false;
        selectedNetwork = null;
        wifiPassword = '';
        wifiScanTargetIndex = null;
        stopWifiPoll();
    }

    function openWifiEditor(slotIndex = null, seed = {}) {
        wifiEditor = {
            open: true,
            mode: seed.mode || 'add',
            index: slotIndex,
            label: seed.label || '',
            ssid: seed.ssid || '',
            password: '',
            priority: Number.isFinite(Number(seed.priority))
                ? Number(seed.priority)
                : nextWifiPriority(),
            showPassword: false,
            hasExistingPassword: Boolean(seed.hasExistingPassword)
        };
    }

    function openWifiEditorForSlot(slot) {
        openWifiEditor(slot.index, {
            mode: 'edit',
            label: slot.label,
            ssid: slot.ssid,
            priority: slot.priority,
            hasExistingPassword: slot.hasPassword
        });
    }

    function closeWifiEditor({ force = false } = {}) {
        if (wifiNetworkActionInFlight && !force) return;
        wifiEditor = {
            open: false,
            mode: 'add',
            index: null,
            label: '',
            ssid: '',
            password: '',
            priority: 0,
            showPassword: false,
            hasExistingPassword: false
        };
    }

    async function saveWifiEditor() {
        const ssid = wifiEditor.ssid.trim();
        if (!ssid || wifiNetworkActionInFlight) return;

        wifiNetworkActionInFlight = true;
        try {
            const body = {
                ssid,
                label: wifiEditor.label.trim(),
                priority: Math.max(0, Math.min(255, Number(wifiEditor.priority) || 0))
            };
            if (wifiEditor.index !== null) {
                body.index = wifiEditor.index;
            }
            if (
                wifiEditor.password ||
                wifiEditor.mode !== 'edit' ||
                !wifiEditor.hasExistingPassword
            ) {
                body.password = wifiEditor.password;
            }

            const res = await fetchWithTimeout('/api/wifi/networks', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to save WiFi network' };
                return;
            }
            message = { type: 'success', text: `Saved ${ssid}` };
            closeWifiEditor({ force: true });
            await fetchSavedWifiNetworks();
        } catch (e) {
            message = { type: 'error', text: 'Failed to save WiFi network' };
        } finally {
            wifiNetworkActionInFlight = false;
        }
    }

    async function deleteWifiSlot(slot) {
        if (!confirm(`Delete saved WiFi network "${slot.ssid}"?`)) return;

        wifiNetworkActionInFlight = true;
        try {
            const res = await fetchWithTimeout('/api/wifi/networks/delete', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ index: slot.index })
            });
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to delete WiFi network' };
                return;
            }
            message = { type: 'success', text: `Deleted ${slot.ssid}` };
            await fetchSavedWifiNetworks();
            await fetchWifiStatus();
        } catch (e) {
            message = { type: 'error', text: 'Failed to delete WiFi network' };
        } finally {
            wifiNetworkActionInFlight = false;
        }
    }

    async function testWifiSlot(slot) {
        if (wifiTestingIndex !== null) return;

        const runId = ++wifiTestRunId;
        const deadlineMs = Date.now() + WIFI_TEST_TIMEOUT_MS;
        wifiTestingIndex = slot.index;
        try {
            const res = await fetchWithTimeout('/api/wifi/networks/test', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ index: slot.index })
            });
            if (!res.ok) {
                message = { type: 'error', text: `Failed to test ${slot.ssid}` };
                return;
            }
            message = { type: 'info', text: `Testing connection to ${slot.ssid}...` };

            while (Date.now() < deadlineMs) {
                const statusTimeoutMs = Math.min(
                    WIFI_TEST_STATUS_TIMEOUT_MS,
                    deadlineMs - Date.now()
                );
                const status = await fetchWifiStatus({
                    reportError: false,
                    timeoutMs: statusTimeoutMs
                });
                if (runId !== wifiTestRunId) return;

                if (status?.state === 'connected') {
                    const connectedIndex = Number(status.connectedSlotIndex);
                    const hasConnectedIndex =
                        Number.isInteger(connectedIndex) && connectedIndex >= 0;
                    const targetConnected = hasConnectedIndex
                        ? connectedIndex === Number(slot.index)
                        : status.connectedSSID === slot.ssid;
                    if (targetConnected) {
                        message = { type: 'success', text: `Connected to ${slot.ssid}` };
                    } else {
                        message = {
                            type: 'error',
                            text: `Connection test reached ${status.connectedSSID || 'another saved network'}, not ${slot.ssid}`
                        };
                    }
                    return;
                }

                if (['failed', 'disabled', 'disconnected'].includes(status?.state)) {
                    message = { type: 'error', text: `Could not connect to ${slot.ssid}` };
                    return;
                }

                const remainingMs = deadlineMs - Date.now();
                if (remainingMs <= 0) break;
                await new Promise((resolve) =>
                    setTimeout(resolve, Math.min(WIFI_TEST_POLL_INTERVAL_MS, remainingMs))
                );
                if (runId !== wifiTestRunId) return;
            }

            message = { type: 'error', text: `Connection test timed out for ${slot.ssid}` };
        } catch (e) {
            message = { type: 'error', text: `Failed to test ${slot.ssid}` };
        } finally {
            if (runId === wifiTestRunId) {
                wifiTestingIndex = null;
            }
        }
    }

    async function moveWifiSlot(slot, direction) {
        const configured = sortedConfiguredWifiSlots();
        const position = configured.findIndex((candidate) => candidate.index === slot.index);
        const targetPosition = position + direction;
        if (
            wifiOrdering ||
            position < 0 ||
            targetPosition < 0 ||
            targetPosition >= configured.length
        ) {
            return;
        }

        const reordered = [...configured];
        [reordered[position], reordered[targetPosition]] = [
            reordered[targetPosition],
            reordered[position]
        ];

        wifiOrdering = true;
        try {
            for (let priority = 0; priority < reordered.length; priority += 1) {
                const candidate = reordered[priority];
                if (candidate.priority === priority) continue;
                const res = await fetchWithTimeout('/api/wifi/networks', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        index: candidate.index,
                        ssid: candidate.ssid,
                        label: candidate.label,
                        priority
                    })
                });
                if (!res.ok) {
                    message = { type: 'error', text: 'Failed to reorder WiFi networks' };
                    return;
                }
            }
            message = { type: 'success', text: 'WiFi network priority updated' };
            await fetchSavedWifiNetworks();
        } catch (e) {
            message = { type: 'error', text: 'Failed to reorder WiFi networks' };
        } finally {
            wifiOrdering = false;
        }
    }

    async function ensureWifiModalLoaded() {
        if (SettingsWifiModalComponent || wifiModalLoading) return;
        wifiModalLoading = true;
        try {
            const module = await settingsLazyComponents.loadSettingsWifiModal();
            SettingsWifiModalComponent = module.default;
        } catch (error) {
            closeWifiModal();
            message = { type: 'error', text: 'Failed to load WiFi modal' };
        } finally {
            wifiModalLoading = false;
        }
    }

    async function saveSettings() {
        saving = true;
        message = null;

        try {
            const formData = new FormData();
            formData.append('ap_ssid', settings.ap_ssid);
            formData.append('ap_password', settings.ap_password);
            formData.append('autoPowerOffMinutes', settings.autoPowerOffMinutes);
            formData.append('apTimeoutMinutes', settings.apTimeoutMinutes);
            formData.append('powerOffSdLog', settings.powerOffSdLog ? 'true' : 'false');

            const res = await postSettingsForm(formData, '/api/device/settings');

            if (res.ok) {
                message = {
                    type: 'success',
                    text: 'Settings saved! New AP credentials apply the next time the AP starts.'
                };
                await fetchSettings({ force: true });
            } else {
                message = { type: 'error', text: 'Failed to save settings' };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            saving = false;
        }
    }

    async function downloadBackup() {
        try {
            const res = await fetchWithTimeout('/api/settings/backup');
            if (res.ok) {
                const blob = await res.blob();
                const url = window.URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = 'v1simple_backup.json';
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                window.URL.revokeObjectURL(url);
                message = { type: 'success', text: 'Backup downloaded!' };
            } else {
                message = { type: 'error', text: 'Failed to download backup' };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        }
    }

    async function backupNowToSd() {
        backingUpNow = true;
        try {
            const res = await fetchWithTimeout('/api/settings/backup-now', { method: 'POST' });
            const data = await res.json().catch(() => ({}));
            if (res.ok && data.success) {
                message = { type: 'success', text: data.message || 'Backup saved to SD card.' };
            } else {
                message = { type: 'error', text: data.error || 'Failed to save backup to SD' };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        } finally {
            backingUpNow = false;
        }
    }

    function handleFileSelect(e) {
        const file = e.target.files[0];
        if (file) {
            restoreFile = file;
        }
    }

    async function restoreBackup() {
        if (!restoreFile) {
            message = { type: 'error', text: 'Please select a backup file first' };
            return;
        }

        // Confirm before overwriting
        if (
            !confirm(
                'Warning: This will overwrite all your current settings and profiles.\n\nSaved WiFi passwords are kept for networks whose name matches; other networks will need their password re-entered.\n\nAre you sure you want to restore from this backup?'
            )
        ) {
            return;
        }

        restoring = true;
        message = null;

        try {
            const text = await restoreFile.text();
            const validationError = getBackupValidationError(text);
            if (validationError) {
                message = { type: 'error', text: validationError };
                return;
            }

            const res = await fetchWithTimeout('/api/settings/restore', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: text
            });

            const data = await res.json();
            if (res.ok && data.success) {
                message = { type: 'success', text: 'Settings restored! Refresh to see changes.' };
                restoreFile = null;
                // Refresh settings
                await fetchSettings();
            } else {
                message = { type: 'error', text: data.error || 'Failed to restore backup' };
            }
        } catch (e) {
            message = { type: 'error', text: 'Failed to read backup file' };
        } finally {
            restoring = false;
        }
    }
</script>

<div class="page-stack">
    <PageHeader title="Settings" subtitle="Network, power, and backup configuration." />

    <StatusAlert {message} />

    {#if loading}
        <div class="state-loading">
            <span class="loading loading-spinner loading-lg"></span>
        </div>
    {:else}
        <!-- AP Settings -->
        <div class="surface-card">
            <div class="card-body">
                <CardSectionHead
                    title="Access Point (AP)"
                    subtitle="Device hosts its own hotspot for direct connection."
                />

                <div class="field-control">
                    <label class="label" for="ap-ssid">
                        <span class="field-label">AP Name</span>
                    </label>
                    <input
                        id="ap-ssid"
                        type="text"
                        class="input w-full"
                        bind:value={settings.ap_ssid}
                        placeholder="V1-Simple"
                    />
                </div>

                <div class="field-control">
                    <label class="label" for="ap-password">
                        <span class="field-label">AP Password</span>
                    </label>
                    <input
                        id="ap-password"
                        type="password"
                        class="input w-full"
                        bind:value={settings.ap_password}
                        placeholder="At least 8 characters"
                    />
                </div>

                <div class="field-control mt-4">
                    <label class="label cursor-pointer">
                        <span class="field-label">Disable AP Inactivity Timeout</span>
                        <input
                            type="checkbox"
                            class="toggle toggle-primary"
                            checked={settings.apTimeoutMinutes === 0}
                            onchange={(e) =>
                                (settings.apTimeoutMinutes = e.target.checked ? 0 : 15)}
                        />
                    </label>
                    <div class="label">
                        <span class="field-hint">
                            This controls AP inactivity only. The separate maintenance-session limit
                            and remaining time are shown in the banner above.
                        </span>
                    </div>
                    {#if settings.apTimeoutMinutes > 0}
                        <label class="label" for="ap-timeout">
                            <span class="field-label">Auto-off after (minutes)</span>
                        </label>
                        <input
                            id="ap-timeout"
                            type="range"
                            class="range range-sm"
                            min="5"
                            max="60"
                            step="5"
                            bind:value={settings.apTimeoutMinutes}
                        />
                        <div class="label">
                            <span class="field-hint">
                                AP will turn off after {settings.apTimeoutMinutes} minutes of inactivity
                            </span>
                        </div>
                    {/if}
                </div>
            </div>
        </div>

        <!-- WiFi Client (Saved Networks) -->
        <div class="surface-card">
            <div class="card-body space-y-4">
                <CardSectionHead
                    title="WiFi Client"
                    subtitle="Save up to four STA networks for maintenance-mode auto-join."
                >
                    <input
                        type="checkbox"
                        class="toggle toggle-primary"
                        checked={wifiStatus.enabled}
                        onchange={(e) => toggleWifiClient(e.target.checked)}
                    />
                </CardSectionHead>

                {#if wifiStatus.enabled}
                    {#if wifiStatus.state === 'connected'}
                        <StatusAlert
                            message={{ type: 'success', text: wifiConnectedStatusText() }}
                        />
                        <div class="flex gap-2">
                            <button class="btn btn-outline btn-sm" onclick={disconnectWifi}>
                                Disconnect
                            </button>
                        </div>
                    {:else if wifiStatus.state === 'connecting'}
                        <StatusAlert
                            message={{
                                type: 'info',
                                text: `Connecting to ${wifiStatus.savedSSID}...`
                            }}
                            busy
                        />
                    {:else}
                        <StatusAlert
                            message={{
                                type: 'info',
                                text: 'Saved networks will be tried in priority order while AP stays online.'
                            }}
                        />
                    {/if}

                    <div class="flex flex-wrap gap-2">
                        <button class="btn btn-primary btn-sm" onclick={() => openWifiEditor(null)}>
                            Enter manually
                        </button>
                        <button class="btn btn-outline btn-sm" onclick={() => startWifiScan(null)}>
                            Pick from scan
                        </button>
                    </div>

                    <div class="space-y-3">
                        <div class="flex items-center justify-between gap-3">
                            <h3 class="font-semibold">Saved Networks</h3>
                            {#if wifiNetworksLoading}
                                <span class="loading loading-spinner loading-sm"></span>
                            {/if}
                        </div>

                        {#if !wifiNetworksLoading && !hasConfiguredWifiSlots()}
                            <div class="surface-note copy-muted">
                                No saved WiFi networks yet. Add a car hotspot, phone hotspot, or
                                garage AP so future maintenance sessions can auto-join.
                            </div>
                        {/if}

                        <div class="space-y-2">
                            {#each sortedWifiSlots() as slot (slot.index)}
                                <div class="rounded-box border border-base-300 bg-base-100/70 p-3">
                                    {#if slotConfigured(slot)}
                                        <div
                                            class="flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between"
                                        >
                                            <div class="min-w-0 space-y-1">
                                                <div class="flex flex-wrap items-center gap-2">
                                                    <span class="font-semibold"
                                                        >{slot.label || slot.ssid}</span
                                                    >
                                                    {#if isSlotConnected(slot)}
                                                        <span class="badge badge-success badge-sm"
                                                            >Connected</span
                                                        >
                                                        <span class="badge badge-outline badge-sm"
                                                            >{wifiStatus.rssi} dBm</span
                                                        >
                                                    {:else if wifiStatus.state === 'connecting' && (wifiStatus.savedSSID === slot.ssid || wifiStatus.connectedSSID === slot.ssid)}
                                                        <span class="badge badge-info badge-sm"
                                                            >Connecting</span
                                                        >
                                                    {:else}
                                                        <span class="badge badge-ghost badge-sm"
                                                            >Saved</span
                                                        >
                                                    {/if}
                                                </div>
                                                <p class="copy-muted text-sm">SSID: {slot.ssid}</p>
                                                <p class="copy-muted text-xs">
                                                    Slot {slot.index + 1} • Priority {slot.priority +
                                                        1}
                                                    {#if slot.hasPassword}
                                                        • password saved
                                                    {:else}
                                                        • open/no saved password
                                                    {/if}
                                                    {#if slot.lastConnectedAtSec}
                                                        • last connected {slot.lastConnectedAtSec}s
                                                    {/if}
                                                </p>
                                            </div>

                                            <div class="flex flex-wrap gap-2">
                                                <button
                                                    class="btn btn-ghost btn-xs"
                                                    onclick={() => openWifiEditorForSlot(slot)}
                                                >
                                                    Edit
                                                </button>
                                                <button
                                                    class="btn btn-outline btn-xs"
                                                    onclick={() => testWifiSlot(slot)}
                                                    disabled={wifiTestingIndex !== null}
                                                >
                                                    {#if wifiTestingIndex === slot.index}
                                                        <span
                                                            class="loading loading-spinner loading-xs"
                                                        ></span>
                                                    {/if}
                                                    Test
                                                </button>
                                                <button
                                                    class="btn btn-ghost btn-xs"
                                                    aria-label={`Move ${slot.ssid} up`}
                                                    onclick={() => moveWifiSlot(slot, -1)}
                                                    disabled={wifiOrdering ||
                                                        moveDisabled(slot, -1)}
                                                >
                                                    ↑
                                                </button>
                                                <button
                                                    class="btn btn-ghost btn-xs"
                                                    aria-label={`Move ${slot.ssid} down`}
                                                    onclick={() => moveWifiSlot(slot, 1)}
                                                    disabled={wifiOrdering || moveDisabled(slot, 1)}
                                                >
                                                    ↓
                                                </button>
                                                <button
                                                    class="btn btn-error btn-outline btn-xs"
                                                    onclick={() => deleteWifiSlot(slot)}
                                                >
                                                    Delete
                                                </button>
                                            </div>
                                        </div>
                                    {:else}
                                        <div
                                            class="flex flex-col gap-3 lg:flex-row lg:items-center lg:justify-between"
                                        >
                                            <div>
                                                <p class="font-semibold">
                                                    Slot {slot.index + 1}: Empty slot
                                                </p>
                                                <p class="copy-muted text-sm">
                                                    Save another maintenance network here.
                                                </p>
                                            </div>
                                            <div class="flex flex-wrap gap-2">
                                                <button
                                                    class="btn btn-primary btn-xs"
                                                    onclick={() => openWifiEditor(slot.index)}
                                                >
                                                    Enter manually
                                                </button>
                                                <button
                                                    class="btn btn-outline btn-xs"
                                                    onclick={() => startWifiScan(slot.index)}
                                                >
                                                    Pick from scan
                                                </button>
                                            </div>
                                        </div>
                                    {/if}
                                </div>
                            {/each}
                        </div>
                    </div>
                {:else}
                    <div class="surface-note copy-muted">
                        WiFi client is disabled. Enable it to manage saved maintenance networks.
                    </div>
                {/if}
            </div>
        </div>

        {#if showWifiModal}
            {#if SettingsWifiModalComponent}
                <SettingsWifiModalComponent
                    open={showWifiModal}
                    title="Pick WiFi Network to Save"
                    selectedPrompt="Save"
                    actionLabel="Save Network"
                    {wifiScanning}
                    {wifiNetworks}
                    bind:selectedNetwork
                    bind:wifiPassword
                    wifiConnecting={wifiScanSaving}
                    onstartWifiScan={startWifiScan}
                    onselectNetwork={selectNetwork}
                    onconnectToNetwork={saveSelectedScanNetwork}
                    oncloseWifiModal={closeWifiModal}
                />
            {:else if wifiModalLoading}
                <div class="modal modal-open">
                    <div class="modal-box surface-modal max-w-md">
                        <div class="state-loading stack">
                            <span class="loading loading-spinner loading-md"></span>
                            <p class="copy-muted">Loading WiFi modal...</p>
                        </div>
                    </div>
                </div>
            {/if}
        {/if}

        {#if wifiEditor.open}
            <div class="modal modal-open">
                <div class="modal-box surface-modal max-w-md">
                    <h3 class="font-bold text-lg">
                        {wifiEditor.mode === 'edit' ? 'Edit Saved Network' : 'Add Saved Network'}
                    </h3>
                    <div class="py-4 space-y-4">
                        <div class="field-control">
                            <label class="label" for="wifi-editor-label">
                                <span class="field-label">Label</span>
                            </label>
                            <input
                                id="wifi-editor-label"
                                type="text"
                                class="input w-full"
                                bind:value={wifiEditor.label}
                                placeholder="Car hotspot, Garage, Phone"
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="wifi-editor-ssid">
                                <span class="field-label">SSID</span>
                            </label>
                            <input
                                id="wifi-editor-ssid"
                                type="text"
                                class="input w-full"
                                bind:value={wifiEditor.ssid}
                                placeholder="Network name"
                            />
                        </div>

                        <div class="field-control">
                            <label class="label" for="wifi-editor-password">
                                <span class="field-label">Password</span>
                            </label>
                            <div class="join w-full">
                                <input
                                    id="wifi-editor-password"
                                    type={wifiEditor.showPassword ? 'text' : 'password'}
                                    class="input join-item w-full"
                                    bind:value={wifiEditor.password}
                                    placeholder={wifiEditor.mode === 'edit' &&
                                    wifiEditor.hasExistingPassword
                                        ? 'Leave blank to keep saved password'
                                        : 'Blank for open network'}
                                />
                                <button
                                    type="button"
                                    class="btn btn-outline join-item"
                                    onclick={() =>
                                        (wifiEditor.showPassword = !wifiEditor.showPassword)}
                                >
                                    {wifiEditor.showPassword ? 'Hide' : 'Show'}
                                </button>
                            </div>
                            {#if wifiEditor.mode === 'edit' && wifiEditor.hasExistingPassword}
                                <div class="label">
                                    <span class="field-hint"
                                        >Leave blank to keep the saved password.</span
                                    >
                                </div>
                            {/if}
                        </div>

                        <div class="field-control">
                            <label class="label" for="wifi-editor-priority">
                                <span class="field-label">Priority</span>
                            </label>
                            <input
                                id="wifi-editor-priority"
                                type="number"
                                min="0"
                                max="255"
                                class="input w-full"
                                bind:value={wifiEditor.priority}
                            />
                            <div class="label">
                                <span class="field-hint"
                                    >0 is tried first; higher numbers are lower priority.</span
                                >
                            </div>
                        </div>
                    </div>

                    <div class="modal-action">
                        <button
                            class="btn btn-ghost"
                            onclick={closeWifiEditor}
                            disabled={wifiNetworkActionInFlight}
                        >
                            Cancel
                        </button>
                        <button
                            class="btn btn-primary"
                            onclick={saveWifiEditor}
                            disabled={wifiNetworkActionInFlight || !wifiEditor.ssid.trim()}
                        >
                            {#if wifiNetworkActionInFlight}
                                <span class="loading loading-spinner loading-sm"></span>
                            {/if}
                            Save
                        </button>
                    </div>
                </div>
                <div
                    class="modal-backdrop bg-black/50"
                    role="presentation"
                    onclick={closeWifiEditor}
                ></div>
            </div>
        {/if}

        <SettingsAutoPowerOffCard {settings} />

        <!-- Save Button -->
        <button class="btn btn-primary btn-block" onclick={saveSettings} disabled={saving}>
            {#if saving}
                <span class="loading loading-spinner loading-sm"></span>
            {/if}
            Save Settings
        </button>

        <SettingsBackupCard
            {backingUpNow}
            onbackupNowToSd={backupNowToSd}
            ondownloadBackup={downloadBackup}
            {restoreFile}
            {restoring}
            onfileSelect={handleFileSelect}
            onrestoreBackup={restoreBackup}
        />
    {/if}
</div>
