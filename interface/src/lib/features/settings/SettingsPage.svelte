<script>
    import { onMount } from 'svelte';
    import { createPoll, fetchJsonWithTimeout, fetchWithTimeout } from '#lib/utils/poll.js';
    import {
        invalidateDeviceSettings,
        refreshDeviceSettings,
        retainDeviceSettings
    } from '#lib/stores/deviceSettings.svelte.js';
    import { postSettingsForm } from '#lib/api/settings.js';
    import CardSectionHead from '#lib/components/CardSectionHead.svelte';
    import SettingsAutoPowerOffCard from '#lib/features/settings/SettingsAutoPowerOffCard.svelte';
    import PageHeader from '#lib/components/PageHeader.svelte';
    import SettingsBackupCard from '#lib/features/settings/SettingsBackupCard.svelte';
    import * as settingsLazyComponents from '#lib/features/settings/settingsLazyComponents.js';
    import {
        applyWifiScanResponse,
        beginWifiScan,
        buildWifiEditorRequest,
        closeWifiEditorState,
        closeWifiScan,
        createWifiEditorState,
        createWifiScanState,
        disposeWifiScan,
        isCurrentWifiScan,
        openWifiEditorState,
        selectWifiScanNetwork,
        setWifiScanSaving
    } from '#lib/features/settings/wifiNetworkUiModel.js';
    import StatusAlert from '#lib/components/StatusAlert.svelte';
    let settings = $state({
        ap_ssid: '',
        ap_password: '',
        autoPowerOffMinutes: 0,
        apTimeoutMinutes: 0
    });

    let loading = $state(true);
    let settingsLoaded = $state(false);
    let saving = $state(false);
    let message = $state(null);
    let restoreFile = $state(null);
    let restoring = $state(false);
    let backingUpNow = $state(false);

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
    let wifiNetworkActionsInFlight = $state(0);
    const wifiNetworkActionInFlight = $derived(wifiNetworkActionsInFlight > 0);
    let wifiNetworksRunId = 0;
    let wifiTestingIndex = $state(null);
    let wifiOrdering = $state(false);
    let wifiScan = $state(createWifiScanState());
    let wifiEditor = $state(createWifiEditorState());
    let wifiPoll = null;
    let wifiStatusFetchPromise = null;
    let wifiStatusRunId = 0;
    let wifiToggleInFlight = $state(false);
    let wifiToggleRunId = 0;
    let componentMounted = false;
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

    // The status API omits connection details when the STA link is down. Replace
    // these fields instead of merging them so a disconnect cannot retain a stale
    // SSID, address, or signal. Other fields still merge because invalid optional
    // values are intentionally ignored.
    const WIFI_CONNECTION_FIELD_DEFAULTS = {
        connectedSSID: '',
        connectedSlotIndex: null,
        ip: '',
        rssi: 0
    };

    function mergeWifiStatus(previous, payload) {
        const merged = { ...previous, ...payload };
        for (const [key, fallback] of Object.entries(WIFI_CONNECTION_FIELD_DEFAULTS)) {
            if (!Object.prototype.hasOwnProperty.call(payload, key)) {
                merged[key] = fallback;
            }
        }
        return merged;
    }

    onMount(() => {
        componentMounted = true;
        const releaseDeviceSettings = retainDeviceSettings();
        const statusPoll = createPoll(async () => {
            // Explicit Test/Scan and mutations own their reads while active.
            if (wifiTestingIndex !== null || wifiScan.scanning || wifiScan.saving ||
                wifiToggleInFlight || wifiNetworkActionInFlight || wifiOrdering) return;
            await fetchWifiStatus({ reportError: false, timeoutMs: WIFI_TEST_STATUS_TIMEOUT_MS });
        }, 3000);
        void (async () => {
            await fetchSettings();
            await fetchWifiStatus();
            await fetchSavedWifiNetworks();
            if (componentMounted) statusPoll.start();
        })();

        return () => {
            componentMounted = false;
            wifiScan = disposeWifiScan(wifiScan);
            wifiEditor = closeWifiEditorState(wifiEditor, { force: true }).state;
            wifiTestRunId += 1;
            wifiToggleRunId += 1;
            invalidateSavedWifiNetworks();
            releaseDeviceSettings();
            statusPoll.stop();
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
        if (!settingsLoaded) loading = true;
        try {
            const data = force ? await invalidateDeviceSettings() : await refreshDeviceSettings();
            if (!data) throw new Error('settings request failed');
            settings = { ...settings, ...data };
            settingsLoaded = true;
            clearMessageText('Failed to load settings');
        } catch (e) {
            message = { type: 'error', text: 'Failed to load settings' };
        } finally {
            loading = false;
        }
    }

    async function fetchWifiStatus({
        reportError = true,
        timeoutMs = 5000,
        forceFresh = false
    } = {}) {
        if (forceFresh) {
            // A mutation has settled. Older reads cannot describe its result,
            // including results consumed by an in-flight connection test.
            wifiStatusRunId += 1;
            while (wifiStatusFetchPromise) {
                await wifiStatusFetchPromise;
            }
        } else if (wifiStatusFetchPromise) {
            return await wifiStatusFetchPromise;
        }

        if (!componentMounted) return null;
        const runId = wifiStatusRunId;
        const isCurrent = () => componentMounted && runId === wifiStatusRunId;

        const request = (async () => {
            try {
                const res = await fetchWithTimeout(
                    '/api/wifi/status',
                    {},
                    timeoutMs,
                    async (response) => ({
                        ok: response.ok,
                        data: response.ok ? await response.json() : null
                    })
                );
                if (!isCurrent()) return null;
                if (res.ok) {
                    const data = res.data;
                    const normalizedStatus =
                        data && typeof data === 'object' && !Array.isArray(data) ? { ...data } : {};
                    if (typeof normalizedStatus.enabled !== 'boolean') {
                        delete normalizedStatus.enabled;
                    }
                    wifiStatus = mergeWifiStatus(wifiStatus, normalizedStatus);
                    clearMessageText(WIFI_STATUS_ERROR_TEXT);
                    return normalizedStatus;
                } else if (reportError) {
                    message = { type: 'error', text: WIFI_STATUS_ERROR_TEXT };
                }
            } catch (e) {
                if (reportError && isCurrent()) {
                    message = { type: 'error', text: WIFI_STATUS_ERROR_TEXT };
                }
            }
            return null;
        })();

        wifiStatusFetchPromise = request;
        try {
            return await request;
        } finally {
            if (wifiStatusFetchPromise === request) {
                wifiStatusFetchPromise = null;
            }
        }
    }

    async function refreshWifiAfterMutation({ includeNetworks = true } = {}) {
        const refreshes = [fetchWifiStatus({ forceFresh: true })];
        if (includeNetworks) refreshes.push(fetchSavedWifiNetworks());
        await Promise.all(refreshes);
    }

    function normalizeWifiSlot(slot, fallbackIndex = 0) {
        const index = Number.isFinite(Number(slot?.index)) ? Number(slot.index) : fallbackIndex;
        const priority = Number.isFinite(Number(slot?.priority)) ? Number(slot.priority) : index;
        const lastConnectedOrder = Number.isFinite(
            Number(slot?.lastConnectedOrder ?? slot?.lastConnectedAtSec)
        )
            ? Number(slot?.lastConnectedOrder ?? slot?.lastConnectedAtSec)
            : 0;

        return {
            index,
            ssid: slot?.ssid || '',
            label: slot?.label || '',
            priority,
            lastConnectedOrder,
            configured: Boolean(slot?.configured || slot?.ssid),
            hasPassword: Boolean(slot?.hasPassword)
        };
    }

    function invalidateSavedWifiNetworks() {
        // A read started before a mutation cannot describe its result. Every
        // settled mutation starts a fresh read, including failed/ambiguous POSTs.
        wifiNetworksRunId += 1;
    }

    async function fetchSavedWifiNetworks() {
        if (!componentMounted) return;
        const runId = ++wifiNetworksRunId;
        const isCurrent = () => componentMounted && runId === wifiNetworksRunId;
        wifiNetworksLoading = true;
        try {
            const res = await fetchJsonWithTimeout('/api/wifi/networks');
            if (!isCurrent()) return;
            if (res.ok) {
                const data = res.data;
                savedWifiSlots = Array.isArray(data.slots)
                    ? data.slots.map((slot, index) => normalizeWifiSlot(slot, index))
                    : [];
                clearMessageText(WIFI_NETWORKS_ERROR_TEXT);
                return true;
            } else {
                message = { type: 'error', text: WIFI_NETWORKS_ERROR_TEXT };
                return false;
            }
        } catch (e) {
            if (isCurrent()) {
                message = { type: 'error', text: WIFI_NETWORKS_ERROR_TEXT };
                return false;
            }
        } finally {
            if (isCurrent()) wifiNetworksLoading = false;
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

    async function startWifiScan(targetIndex = wifiScan.targetIndex) {
        wifiScan = beginWifiScan(wifiScan, targetIndex);
        const runId = wifiScan.runId;
        stopWifiPoll();
        void ensureWifiModalLoaded();

        try {
            const res = await fetchJsonWithTimeout('/api/wifi/scan', { method: 'POST' });
            if (!isCurrentWifiScan(wifiScan, runId)) return;
            if (!res.ok) {
                message = { type: 'error', text: WIFI_SCAN_START_ERROR_TEXT };
                stopWifiPoll();
                closeWifiModal({ force: true });
                return;
            }

            const data = res.data;
            if (!isCurrentWifiScan(wifiScan, runId)) return;
            clearMessageText(WIFI_SCAN_START_ERROR_TEXT);
            clearMessageText(WIFI_SCAN_ERROR_TEXT);
            applyWifiScanApiResponse(data, runId);
        } catch (e) {
            if (!isCurrentWifiScan(wifiScan, runId)) return;
            message = { type: 'error', text: WIFI_SCAN_START_ERROR_TEXT };
            stopWifiPoll();
            closeWifiModal({ force: true });
        }
    }

    function ensureWifiScanPoll(runId) {
        if (!isCurrentWifiScan(wifiScan, runId) || wifiPoll) return;
        wifiPoll = createPoll(async () => {
            await pollWifiScan(runId);
        }, 1000);
        wifiPoll.start();
    }

    function applyWifiScanApiResponse(data, runId) {
        const transition = applyWifiScanResponse(wifiScan, runId, data);
        if (!transition.accepted) return;
        wifiScan = transition.state;
        if (transition.shouldPoll) {
            ensureWifiScanPoll(runId);
            return;
        }

        stopWifiPoll();
    }

    async function pollWifiScan(runId) {
        if (!isCurrentWifiScan(wifiScan, runId)) return;

        try {
            const res = await fetchJsonWithTimeout('/api/wifi/scan');
            if (!isCurrentWifiScan(wifiScan, runId)) return;
            if (res.ok) {
                const data = res.data;
                if (!isCurrentWifiScan(wifiScan, runId)) return;
                clearMessageText(WIFI_SCAN_ERROR_TEXT);
                applyWifiScanApiResponse(data, runId);
            } else {
                message = { type: 'error', text: WIFI_SCAN_ERROR_TEXT };
                stopWifiPoll();
                closeWifiModal({ force: true });
            }
        } catch (e) {
            if (!isCurrentWifiScan(wifiScan, runId)) return;
            message = { type: 'error', text: WIFI_SCAN_ERROR_TEXT };
            stopWifiPoll();
            closeWifiModal({ force: true });
        }

        if (isCurrentWifiScan(wifiScan, runId)) {
            await fetchWifiStatus();
        }
    }

    function selectNetwork(network) {
        wifiScan = selectWifiScanNetwork(wifiScan, network);
    }

    async function saveSelectedScanNetwork() {
        if (
            !wifiScan.selectedNetwork ||
            wifiScan.saving ||
            (wifiScan.selectedNetwork.secure && !wifiScan.password)
        ) {
            return;
        }

        const ssid = wifiScan.selectedNetwork.ssid;
        const secure = wifiScan.selectedNetwork.secure;
        const password = wifiScan.password;
        const targetIndex = wifiScan.targetIndex;
        const submittedRunId = wifiScan.runId;
        wifiScan = setWifiScanSaving(wifiScan, true);
        invalidateSavedWifiNetworks();

        try {
            const body = {
                ssid,
                priority: nextWifiPriority(),
                // Omission preserves old credentials; an open scan result clears them.
                password: secure ? password : ''
            };
            if (targetIndex !== null) {
                body.index = targetIndex;
            }

            const res = await fetchWithTimeout('/api/wifi/networks', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });

            if (!componentMounted) return;
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to save WiFi network' };
                return;
            }

            message = { type: 'success', text: `Saved ${ssid}` };
            if (wifiScan.runId === submittedRunId && wifiScan.password === password) {
                closeWifiModal({ force: true });
            }
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to save WiFi network' };
        } finally {
            await refreshWifiAfterMutation();
            wifiScan = setWifiScanSaving(wifiScan, false);
        }
    }

    async function disconnectWifi() {
        try {
            const res = await fetchWithTimeout('/api/wifi/disconnect', { method: 'POST' });
            if (!componentMounted) return;
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to disconnect' };
                return;
            }
            message = { type: 'success', text: 'Disconnected from WiFi' };
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to disconnect' };
        } finally {
            await refreshWifiAfterMutation({ includeNetworks: false });
        }
    }

    function handleWifiToggleChange(event) {
        const requestedEnabled = event.currentTarget.checked;
        event.currentTarget.checked = wifiStatus.enabled;
        void toggleWifiClient(requestedEnabled);
    }

    async function toggleWifiClient(enabled) {
        if (wifiToggleInFlight) return;

        const runId = ++wifiToggleRunId;
        let response = null;
        let transportFailed = false;
        wifiToggleInFlight = true;
        invalidateSavedWifiNetworks();
        try {
            try {
                response = await fetchWithTimeout('/api/wifi/enable', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ enabled })
                });
            } catch (e) {
                transportFailed = true;
            }
            if (runId !== wifiToggleRunId) return;

            const statusRefresh = fetchWifiStatus({
                reportError: false,
                forceFresh: true
            });
            const statusRunId = wifiStatusRunId;
            const refreshedStatus = await statusRefresh;
            if (runId !== wifiToggleRunId || statusRunId !== wifiStatusRunId) return;

            const networksLoaded = await fetchSavedWifiNetworks();
            if (runId !== wifiToggleRunId || statusRunId !== wifiStatusRunId || networksLoaded !== true) return;

            if (transportFailed) {
                message = { type: 'error', text: 'Connection error' };
            } else if (response?.ok) {
                const confirmed =
                    typeof refreshedStatus?.enabled === 'boolean' &&
                    refreshedStatus.enabled === enabled;
                message = confirmed
                    ? {
                          type: 'success',
                          text: enabled ? 'WiFi client enabled' : 'WiFi client disabled'
                      }
                    : { type: 'error', text: 'Failed to confirm WiFi setting' };
            } else {
                message = { type: 'error', text: 'Failed to change WiFi setting' };
            }
        } finally {
            if (runId === wifiToggleRunId) {
                wifiToggleInFlight = false;
            }
        }
    }

    function closeWifiModal({ force = false } = {}) {
        const transition = closeWifiScan(wifiScan, { force });
        if (!transition.closed) return;
        wifiScan = transition.state;
        stopWifiPoll();
    }

    function openWifiEditor(slotIndex = null, seed = {}) {
        wifiEditor = openWifiEditorState(slotIndex, seed, nextWifiPriority());
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
        const transition = closeWifiEditorState(wifiEditor, {
            force,
            actionInFlight: wifiNetworkActionInFlight
        });
        if (transition.closed) {
            wifiEditor = transition.state;
        }
    }

    async function saveWifiEditor() {
        const body = buildWifiEditorRequest(wifiEditor);
        if (!body || wifiNetworkActionInFlight) return;

        const submittedDraft = JSON.stringify(wifiEditor);
        wifiNetworkActionsInFlight += 1;
        invalidateSavedWifiNetworks();
        try {
            const res = await fetchWithTimeout('/api/wifi/networks', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(body)
            });
            if (!componentMounted) return;
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to save WiFi network' };
                return;
            }
            message = { type: 'success', text: `Saved ${body.ssid}` };
            if (JSON.stringify(wifiEditor) === submittedDraft) closeWifiEditor({ force: true });
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to save WiFi network' };
        } finally {
            await refreshWifiAfterMutation();
            wifiNetworkActionsInFlight -= 1;
        }
    }

    async function deleteWifiSlot(slot) {
        if (!confirm(`Delete saved WiFi network "${slot.ssid}"?`)) return;

        wifiNetworkActionsInFlight += 1;
        invalidateSavedWifiNetworks();
        try {
            const res = await fetchWithTimeout('/api/wifi/networks/delete', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ index: slot.index })
            });
            if (!componentMounted) return;
            if (!res.ok) {
                message = { type: 'error', text: 'Failed to delete WiFi network' };
                return;
            }
            message = { type: 'success', text: `Deleted ${slot.ssid}` };
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to delete WiFi network' };
        } finally {
            await refreshWifiAfterMutation();
            wifiNetworkActionsInFlight -= 1;
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
            if (runId !== wifiTestRunId) return;
            if (!res.ok) {
                message = { type: 'error', text: `Failed to test ${slot.ssid}` };
                return;
            }
            message = { type: 'info', text: `Testing connection to ${slot.ssid}...` };
            let firstStatusRead = true;

            while (Date.now() < deadlineMs) {
                const statusTimeoutMs = Math.min(
                    WIFI_TEST_STATUS_TIMEOUT_MS,
                    deadlineMs - Date.now()
                );
                const statusRefresh = fetchWifiStatus({
                    reportError: false,
                    timeoutMs: statusTimeoutMs,
                    forceFresh: firstStatusRead
                });
                // Starting Test changes the connection. Its first read must
                // follow the POST, and capture the generation it just created.
                const statusRunId = wifiStatusRunId;
                firstStatusRead = false;
                let status = await statusRefresh;
                if (runId !== wifiTestRunId) return;
                // A mutation can settle after the reader completes but before
                // this consumer resumes. Retry through the same bounded loop.
                if (statusRunId !== wifiStatusRunId) status = null;

                if (status?.state === 'connected') {
                    const connectedIndex = Number(status.connectedSlotIndex);
                    const hasConnectedIndex =
                        Number.isInteger(connectedIndex) && connectedIndex >= 0;
                    // A saved slot can be edited while Test is pending. Its
                    // index alone cannot confirm the originally requested SSID.
                    const targetConnected = hasConnectedIndex
                        ? connectedIndex === Number(slot.index) &&
                          (!status.connectedSSID || status.connectedSSID === slot.ssid)
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
        const updates = reordered.flatMap((candidate, priority) =>
            candidate.priority === priority ? [] : [{ index: candidate.index, priority }]
        );

        wifiOrdering = true;
        let resultMessage = null;
        invalidateSavedWifiNetworks();
        try {
            const res = await fetchWithTimeout('/api/wifi/networks/priorities', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ updates })
            });
            if (!componentMounted) return;
            if (!res.ok) {
                resultMessage = { type: 'error', text: 'Failed to reorder WiFi networks' };
                return;
            }
            resultMessage = { type: 'success', text: 'WiFi network priority updated' };
        } catch (e) {
            resultMessage = { type: 'error', text: 'Failed to reorder WiFi networks' };
        } finally {
            if (await fetchSavedWifiNetworks()) message = resultMessage;
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
        if (!settingsLoaded || saving) return;

        saving = true;
        message = null;

        try {
            const formData = new FormData();
            formData.append('ap_ssid', settings.ap_ssid);
            formData.append('ap_password', settings.ap_password);
            formData.append('autoPowerOffMinutes', settings.autoPowerOffMinutes);
            formData.append('apTimeoutMinutes', settings.apTimeoutMinutes);

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
            const res = await fetchWithTimeout(
                '/api/settings/backup',
                {},
                undefined,
                async (response) => ({
                    ok: response.ok,
                    blob: response.ok ? await response.blob() : null
                })
            );
            if (res.ok) {
                const blob = res.blob;
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
            const res = await fetchWithTimeout(
                '/api/settings/backup-now',
                { method: 'POST' },
                undefined,
                async (response) => ({
                    ok: response.ok,
                    data: await response.json().catch(() => ({}))
                })
            );
            const data = res.data;
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

        if (
            !confirm(
                'Warning: Restoring this backup changes your current settings and profiles.\n\n' +
                    'Current-format backups replace the saved profile catalog. Accepted older partial backups may merge profiles with your existing catalog, leaving omitted profiles in place.\n\n' +
                    'If a WiFi password is absent from the backup, you may need to re-enter it after restoring.\n\n' +
                    'Are you sure you want to restore from this backup?'
            )
        ) {
            return;
        }

        restoring = true;
        message = null;
        let restoreSubmitted = false;
        let restoreSucceeded = false;

        try {
            const text = await restoreFile.text();
            if (!componentMounted) return;
            const validationError = getBackupValidationError(text);
            if (validationError) {
                message = { type: 'error', text: validationError };
                return;
            }

            restoreSubmitted = true;
            invalidateSavedWifiNetworks();
            const res = await fetchWithTimeout(
                '/api/settings/restore',
                {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: text
                },
                undefined,
                async (response) => ({
                    ok: response.ok,
                    data: await response.json()
                })
            );

            if (!componentMounted) return;
            const data = res.data;
            if (res.ok && data.success) {
                message = { type: 'success', text: 'Settings restored and reloaded.' };
                restoreFile = null;
                restoreSucceeded = true;
            } else {
                message = { type: 'error', text: data.error || 'Failed to restore backup' };
            }
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to read backup file' };
        } finally {
            if (restoreSubmitted) {
                const refreshes = [refreshWifiAfterMutation()];
                if (restoreSucceeded) refreshes.push(fetchSettings({ force: true }));
                await Promise.all(refreshes);
            }
            restoring = false;
        }
    }
</script>

<div class="page-stack">
    <PageHeader title="Settings" subtitle="Network, power, and backup configuration." />

    <StatusAlert {message} />

    {#if loading}
        <div class="state-loading">
            <span class="loading loading-lg loading-spinner"></span>
        </div>
    {:else}
        {#if !settingsLoaded}
            <button class="btn btn-outline" onclick={() => fetchSettings({ force: true })}>
                Retry settings
            </button>
        {/if}

        <fieldset class="surface-card" disabled={saving || !settingsLoaded}>
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

                <p class="field-hint mt-4">
                    The device returns to normal mode after 10 idle minutes or 30 minutes total.
                    The banner shows the remaining time.
                </p>
            </div>
        </fieldset>

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
                        disabled={wifiToggleInFlight}
                        onchange={handleWifiToggleChange}
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
                                <span class="loading loading-sm loading-spinner"></span>
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
                                                        <span class="badge badge-sm badge-success"
                                                            >Connected</span
                                                        >
                                                        <span class="badge badge-outline badge-sm"
                                                            >{wifiStatus.rssi} dBm</span
                                                        >
                                                    {:else if wifiStatus.state === 'connecting' && (wifiStatus.savedSSID === slot.ssid || wifiStatus.connectedSSID === slot.ssid)}
                                                        <span class="badge badge-sm badge-info"
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
                                                    {#if slot.lastConnectedOrder}
                                                        • connected previously
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
                                                            class="loading loading-xs loading-spinner"
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
                                                    class="btn btn-outline btn-error btn-xs"
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

        {#if wifiScan.open}
            {#if SettingsWifiModalComponent}
                <SettingsWifiModalComponent
                    open={wifiScan.open}
                    title="Pick WiFi Network to Save"
                    selectedPrompt="Save"
                    actionLabel="Save Network"
                    wifiScanning={wifiScan.scanning}
                    wifiNetworks={wifiScan.networks}
                    bind:selectedNetwork={wifiScan.selectedNetwork}
                    bind:wifiPassword={wifiScan.password}
                    wifiConnecting={wifiScan.saving}
                    onstartWifiScan={startWifiScan}
                    onselectNetwork={selectNetwork}
                    onconnectToNetwork={saveSelectedScanNetwork}
                    oncloseWifiModal={closeWifiModal}
                />
            {:else if wifiModalLoading}
                <div class="modal modal-open">
                    <div class="surface-modal modal-box max-w-md">
                        <div class="state-loading stack">
                            <span class="loading loading-md loading-spinner"></span>
                            <p class="copy-muted">Loading WiFi modal...</p>
                        </div>
                    </div>
                </div>
            {/if}
        {/if}

        {#if wifiEditor.open}
            <div class="modal modal-open">
                <div class="surface-modal modal-box max-w-md">
                    <h3 class="text-lg font-bold">
                        {wifiEditor.mode === 'edit' ? 'Edit Saved Network' : 'Add Saved Network'}
                    </h3>
                    <div class="space-y-4 py-4">
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
                                    class="btn join-item btn-outline"
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
                            disabled={wifiNetworkActionInFlight || !wifiEditor.ssid}
                        >
                            {#if wifiNetworkActionInFlight}
                                <span class="loading loading-sm loading-spinner"></span>
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

        <fieldset disabled={saving || !settingsLoaded}>
            <SettingsAutoPowerOffCard {settings} />
        </fieldset>

        <button
            class="btn btn-block btn-primary"
            onclick={saveSettings}
            disabled={saving || !settingsLoaded}
        >
            {#if saving}
                <span class="loading loading-sm loading-spinner"></span>
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
