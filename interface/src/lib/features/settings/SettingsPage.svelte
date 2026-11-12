<script>
    import { onMount } from 'svelte';
    import { nativeDialog } from '#lib/utils/nativeDialog.js';
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
    let settingsRunId = 0;
    let restoreReadbackRequired = false;
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
    let wifiNetworksReadbackRequired = $state(false);
    const wifiControlsDisabled = $derived(restoring || wifiNetworksReadbackRequired);
    let wifiPendingSlotChanges = $state([]);
    let wifiSlotChangeId = 0;
    let wifiNetworkActionsInFlight = $state(0);
    const wifiNetworkActionInFlight = $derived(wifiNetworkActionsInFlight > 0);
    let wifiScanSavesInFlight = $state(0);
    let wifiNetworksRunId = 0;
    let wifiTestingIndex = $state(null);
    let wifiOrdering = $state(false);
    let wifiScan = $state(createWifiScanState());
    let wifiEditor = $state(createWifiEditorState());
    let wifiEditorSlotSsid = $state();
    let wifiScanSelectedSlotIndex = $state(null);
    let wifiEditorEnteredSlotIndex = $state(null);
    const wifiScanTargetIndex = $derived(
        wifiScan.targetIndex ?? (wifiScan.selectedNetwork ? wifiScanSelectedSlotIndex : null));
    const wifiEditorTargetIndex = $derived(wifiEditor.index ?? wifiEditorEnteredSlotIndex);
    const wifiScanSlotChanged = $derived(
        wifiSaveTargetChanged(wifiScanTargetIndex, wifiScan.selectedNetwork?.ssid,
            wifiScan.targetIndex === null ? wifiScan.selectedNetwork?.ssid : ''));
    const wifiEditorSlotChanged = $derived(
        wifiSaveTargetChanged(wifiEditorTargetIndex, wifiEditor.ssid,
            wifiEditor.index === null ? wifiEditor.ssid : wifiEditorSlotSsid));
    const wifiEditorAppendUnavailable = $derived(wifiEditor.priority === null &&
        wifiAppendUnavailable(wifiEditorTargetIndex));
    const wifiScanAppendUnavailable = $derived(wifiScan.selectedNetwork &&
        wifiAppendUnavailable(wifiScanTargetIndex));
    let wifiPoll = null;
    let wifiStatusFetchPromise = null;
    let wifiStatusRunId = 0;
    let wifiToggleInFlight = $state(false);
    let wifiToggleRunId = 0;
    let componentMounted = false;
    let wifiTestRunId = 0;
    const wifiSavedMutationBusy = $derived(wifiToggleInFlight || wifiNetworkActionInFlight ||
        wifiOrdering || wifiScanSavesInFlight > 0 || wifiTestingIndex !== null);
    let SettingsWifiModalComponent = $state(null);
    let wifiModalLoading = $state(false);
    const WIFI_STATUS_ERROR_TEXT = 'Failed to load WiFi status';
    const WIFI_NETWORKS_ERROR_TEXT = 'Failed to load saved WiFi networks';
    const WIFI_NETWORKS_CHANGED_TEXT = 'Saved WiFi networks changed. Review the list before trying again.';
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
            if (wifiTestingIndex !== null || wifiScan.scanning || wifiScanSavesInFlight > 0 ||
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
            settingsRunId += 1;
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
        if (!componentMounted) return false;
        const runId = ++settingsRunId;
        const isCurrent = () => componentMounted && runId === settingsRunId;
        if (!settingsLoaded && !restoring) loading = true;
        try {
            const data = force ? await invalidateDeviceSettings() : await refreshDeviceSettings();
            if (!isCurrent()) return false;
            if (!data) throw new Error('settings request failed');
            // A partial reply must not make pre-restore values saveable again.
            if (restoreReadbackRequired && (
                typeof data.ap_ssid !== 'string' || typeof data.ap_password !== 'string' ||
                !Number.isFinite(data.autoPowerOffMinutes) || !Number.isFinite(data.apTimeoutMinutes)
            )) throw new Error('incomplete restore readback');
            settings = { ...settings, ...data };
            settingsLoaded = true;
            restoreReadbackRequired = false;
            clearMessageText('Failed to load settings');
            clearMessageText('Could not reload settings after restore. Retry settings before saving.');
            return true;
        } catch (e) {
            if (isCurrent()) message = {
                type: 'error',
                text: restoreReadbackRequired
                    ? 'Could not reload settings after restore. Retry settings before saving.'
                    : 'Failed to load settings'
            };
            return false;
        } finally {
            if (isCurrent()) loading = false;
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
                const res = await fetchJsonWithTimeout('/api/wifi/status', {}, timeoutMs);
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

    async function refreshWifiAfterMutation({ includeNetworks = true, afterRestore = false } = {}) {
        const refreshes = [fetchWifiStatus({ forceFresh: true })];
        if (includeNetworks) refreshes.push(fetchSavedWifiNetworks({ afterRestore }));
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

    function retireSavedWifiNetworks() {
        wifiNetworksReadbackRequired = true;
        closeWifiEditor({ force: true });
        closeWifiModal();
    }

    function validWifiSlots(slots) {
        return Array.isArray(slots) && slots.length <= 4 &&
            slots.every((slot) => slot && Number.isInteger(slot.index) &&
                slot.index >= 0 && slot.index < 4 && typeof slot.ssid === 'string') &&
            new Set(slots.map((slot) => slot.index)).size === slots.length;
    }

    async function retrySavedWifiNetworks() {
        if (restoring || wifiNetworksLoading || wifiSavedMutationBusy) return;
        await refreshWifiAfterMutation();
    }

    async function fetchSavedWifiNetworks({ afterRestore = false } = {}) {
        if (!componentMounted) return;
        const runId = ++wifiNetworksRunId;
        const isCurrent = () => componentMounted && runId === wifiNetworksRunId;
        const canResolveRestore = !restoring || afterRestore;
        const settledChanges = new Set(wifiPendingSlotChanges
            .filter((change) => change.settled).map((change) => change.id));
        wifiNetworksLoading = true;
        try {
            const res = await fetchJsonWithTimeout('/api/wifi/networks', { cache: 'no-store' });
            if (!isCurrent()) return;
            if (res.ok && validWifiSlots(res.data?.slots)) {
                const data = res.data;
                savedWifiSlots = data.slots.map((slot, index) => normalizeWifiSlot(slot, index));
                if (canResolveRestore) {
                    wifiNetworksReadbackRequired = false;
                    // Only a current read started after the write settled
                    // can release that slot. Older reads and sibling writes
                    // may still be in flight.
                    wifiPendingSlotChanges = wifiPendingSlotChanges.filter((change) => !settledChanges.has(change.id));
                }
                clearMessageText(WIFI_NETWORKS_ERROR_TEXT);
                return true;
            }
            markSavedWifiNetworksUnknown();
            return false;
        } catch (e) {
            if (isCurrent()) {
                markSavedWifiNetworksUnknown();
                return false;
            }
        } finally {
            if (isCurrent()) wifiNetworksLoading = false;
        }
    }

    function markSavedWifiNetworksUnknown() {
        wifiNetworksReadbackRequired = true;
        message = { type: 'error', text: WIFI_NETWORKS_ERROR_TEXT };
    }

    function wifiSlotSsid(index) {
        return savedWifiSlots.find((slot) => slot.index === index)?.ssid;
    }

    function wifiSlotIndexForSsid(ssid) {
        return ssid ? savedWifiSlots.find((slot) => slot.ssid === ssid)?.index ?? null : null;
    }

    function wifiSaveTargetChanged(index, ssid, expectedSsid) {
        if (index !== null) return wifiSlotSsid(index) !== expectedSsid;
        // A draft that began as a create stays a create. Later readback may
        // invalidate it, but must not turn a retry into a credential update.
        return wifiSlotIndexForSsid(ssid) !== null;
    }

    function wifiSlotChangePending(index) {
        // An unindexed upsert lets firmware choose its target. Only explicit
        // slot actions need to wait for that identity to become known again.
        return index !== null &&
            wifiPendingSlotChanges.some((change) => change.index === index || change.index === null);
    }

    function beginWifiSlotChange(index) {
        const id = ++wifiSlotChangeId;
        wifiPendingSlotChanges.push({ id, index, settled: false });
        invalidateSavedWifiNetworks();
        return id;
    }

    function settleWifiSlotChange(id) {
        wifiPendingSlotChanges.find((change) => change.id === id).settled = true;
    }

    function slotConfigured(slot) {
        return Boolean(slot?.configured || slot?.ssid);
    }

    function isSlotConnected(slot) {
        if (!slotConfigured(slot) || wifiStatus.state !== 'connected') return false;

        const index = currentConnectedSlotIndex();
        if (index !== null) {
            return slot.index === index && (!wifiStatus.connectedSSID || slot.ssid === wifiStatus.connectedSSID);
        }

        return Boolean(wifiStatus.connectedSSID && slot.ssid === wifiStatus.connectedSSID);
    }

    function currentConnectedSlotIndex() {
        const index = wifiStatus.connectedSlotIndex;
        return Number.isInteger(index) && index >= 0 ? index : null;
    }

    function currentConnectedSlot() {
        const index = currentConnectedSlotIndex();
        if (index === null) return null;
        return savedWifiSlots.find(isSlotConnected) || null;
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

    function compareWifiPriority(a, b) {
        return a.priority - b.priority || b.lastConnectedOrder - a.lastConnectedOrder || a.index - b.index;
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
                return compareWifiPriority(a, b);
            }
            return a.index - b.index;
        });
    }

    function hasConfiguredWifiSlots() {
        return savedWifiSlots.some(slotConfigured);
    }

    function nextWifiPriority() {
        return Math.max(-1, ...savedWifiSlots.filter(slotConfigured).map((slot) => slot.priority)) + 1;
    }

    function wifiAppendUnavailable(index) {
        return nextWifiPriority() > 255 && !slotConfigured(savedWifiSlots.find((slot) => slot.index === index));
    }

    function wifiEditorDefaultPriority() {
        return savedWifiSlots.find((slot) => slot.index === wifiEditorTargetIndex && slotConfigured(slot))?.priority ??
            Math.min(255, nextWifiPriority());
    }

    function wifiPriorityUpdates(slot, direction) {
        const configured = savedWifiSlots.filter(slotConfigured).sort(compareWifiPriority);
        const position = configured.findIndex((candidate) =>
            candidate.index === slot.index && candidate.ssid === slot.ssid && candidate.priority === slot.priority);
        const targetPosition = position + direction;
        if (position < 0 || targetPosition < 0 || targetPosition >= configured.length) return [];
        [configured[position], configured[targetPosition]] = [configured[targetPosition], configured[position]];
        const updates = configured.map((candidate, priority) => ({
            index: candidate.index,
            priority,
            expectedSsid: candidate.ssid,
            expectedPriority: candidate.priority
        }));
        return updates.some((update) => update.priority !== update.expectedPriority &&
            wifiSlotChangePending(update.index)) ? [] : updates;
    }

    async function startWifiScan(targetIndex = wifiScan.targetIndex) {
        const rescanning = wifiScan.open && targetIndex === wifiScan.targetIndex;
        if (wifiControlsDisabled || wifiSlotChangePending(targetIndex) || (rescanning && wifiScanSlotChanged)) return;
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
        wifiScanSelectedSlotIndex = wifiSlotIndexForSsid(network.ssid);
        wifiScan = selectWifiScanNetwork(wifiScan, network);
    }

    function postSavedWifiNetwork(body) {
        return fetchWithTimeout('/api/wifi/networks', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        }, undefined, async (response) => ({
            ok: response.ok,
            slotChanged: response.status === 409 &&
                (await response.json().catch(() => null))?.error === 'network_slot_changed'
        }));
    }

    async function saveSelectedScanNetwork() {
        if (
            wifiControlsDisabled ||
            wifiScanSlotChanged ||
            wifiScanAppendUnavailable ||
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
        const pendingIndex = wifiScanTargetIndex;
        if (wifiSlotChangePending(pendingIndex)) return;
        const submittedRunId = wifiScan.runId;
        wifiScan = setWifiScanSaving(wifiScan, true);
        wifiScanSavesInFlight += 1;
        const changeId = beginWifiSlotChange(pendingIndex);

        try {
            const body = {
                ssid,
                // Omission preserves old credentials; an open scan result clears them.
                password: secure ? password : ''
            };
            if (pendingIndex !== null) {
                body.index = pendingIndex;
                body.expectedSsid = targetIndex === null ? ssid : '';
            }

            const res = await postSavedWifiNetwork(body);

            if (!componentMounted) return;
            if (!res.ok) {
                message = { type: 'error', text: res.slotChanged ? WIFI_NETWORKS_CHANGED_TEXT : 'Failed to save WiFi network' };
                return;
            }

            message = { type: 'success', text: `Saved ${ssid}` };
            if (wifiScan.runId === submittedRunId && wifiScan.password === password) {
                closeWifiModal({ force: true });
            }
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to save WiFi network' };
        } finally {
            settleWifiSlotChange(changeId);
            await refreshWifiAfterMutation();
            if (isCurrentWifiScan(wifiScan, submittedRunId)) wifiScan = setWifiScanSaving(wifiScan, false);
            wifiScanSavesInFlight -= 1;
        }
    }

    async function disconnectWifi() {
        if (wifiControlsDisabled) return;
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
        if (wifiControlsDisabled || wifiToggleInFlight) return;

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
            if (runId !== wifiToggleRunId) return;

            // A separate Test or Disconnect can supersede status, while
            // this toggle still owes the catalog read it invalidated.
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
        if (wifiControlsDisabled || wifiSlotChangePending(slotIndex)) return;
        wifiEditorSlotSsid = slotIndex === null ? undefined : seed.ssid ?? '';
        wifiEditorEnteredSlotIndex = null;
        wifiEditor = openWifiEditorState(slotIndex, seed);
    }

    function openWifiEditorForSlot(slot) {
        openWifiEditor(slot.index, {
            mode: 'edit',
            label: slot.label,
            ssid: slot.ssid,
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

    function enterWifiSsid(ssid) {
        if (ssid !== wifiEditor.ssid) wifiEditorEnteredSlotIndex = wifiSlotIndexForSsid(ssid);
        wifiEditor.ssid = ssid;
    }

    async function saveWifiEditor() {
        const body = buildWifiEditorRequest(wifiEditor, wifiEditorSlotSsid ?? '');
        if (wifiControlsDisabled || wifiEditorSlotChanged || wifiEditorAppendUnavailable || !body || wifiNetworkActionInFlight ||
            (wifiOrdering && Object.hasOwn(body, 'priority'))) return;
        const pendingIndex = wifiEditorTargetIndex;
        if (wifiSlotChangePending(pendingIndex)) return;
        if (wifiEditor.index === null && pendingIndex !== null) {
            body.index = pendingIndex;
            body.expectedSsid = body.ssid;
        }

        const submittedDraft = JSON.stringify(wifiEditor);
        wifiNetworkActionsInFlight += 1;
        const changeId = beginWifiSlotChange(pendingIndex);
        try {
            const res = await postSavedWifiNetwork(body);
            if (!componentMounted) return;
            if (!res.ok) {
                message = { type: 'error', text: res.slotChanged ? WIFI_NETWORKS_CHANGED_TEXT : 'Failed to save WiFi network' };
                return;
            }
            message = { type: 'success', text: `Saved ${body.ssid}` };
            if (JSON.stringify(wifiEditor) === submittedDraft) closeWifiEditor({ force: true });
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to save WiFi network' };
        } finally {
            settleWifiSlotChange(changeId);
            await refreshWifiAfterMutation();
            wifiNetworkActionsInFlight -= 1;
        }
    }

    async function deleteWifiSlot(slot) {
        if (wifiControlsDisabled || wifiSlotChangePending(slot.index)) return;
        if (!confirm(`Delete saved WiFi network "${slot.ssid}"?`)) return;

        wifiNetworkActionsInFlight += 1;
        const changeId = beginWifiSlotChange(slot.index);
        try {
            const res = await fetchWithTimeout('/api/wifi/networks/delete', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ index: slot.index, expectedSsid: slot.ssid })
            });
            if (!componentMounted) return;
            if (!res.ok) {
                message = { type: 'error', text: res.status === 409 ? WIFI_NETWORKS_CHANGED_TEXT : 'Failed to delete WiFi network' };
                return;
            }
            message = { type: 'success', text: `Deleted ${slot.ssid}` };
        } catch (e) {
            if (componentMounted) message = { type: 'error', text: 'Failed to delete WiFi network' };
        } finally {
            settleWifiSlotChange(changeId);
            await refreshWifiAfterMutation();
            wifiNetworkActionsInFlight -= 1;
        }
    }

    async function testWifiSlot(slot) {
        if (wifiControlsDisabled || wifiSlotChangePending(slot.index) || wifiTestingIndex !== null) return;

        const runId = ++wifiTestRunId;
        const deadlineMs = Date.now() + WIFI_TEST_TIMEOUT_MS;
        wifiTestingIndex = slot.index;
        try {
            const res = await fetchWithTimeout('/api/wifi/networks/test', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ index: slot.index, expectedSsid: slot.ssid })
            });
            if (runId !== wifiTestRunId) return;
            if (res.status === 409) {
                settleWifiSlotChange(beginWifiSlotChange(slot.index));
                message = { type: 'error', text: WIFI_NETWORKS_CHANGED_TEXT };
                await refreshWifiAfterMutation();
                return;
            }
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
                    const connectedIndex = status.connectedSlotIndex;
                    const connectedSsid = status.connectedSSID;
                    const hasConnectedIndex = Number.isInteger(connectedIndex) && connectedIndex >= 0;
                    // Status can still report a physical link whose saved
                    // slot no longer owns it. Test requires the indexed link.
                    if (!connectedSsid || (connectedSsid === slot.ssid && !hasConnectedIndex)) {
                        message = { type: 'error', text: `Could not confirm connection to ${slot.ssid}` };
                    } else if (connectedSsid !== slot.ssid) {
                        message = {
                            type: 'error',
                            text: `Connection test reached ${connectedSsid}, not ${slot.ssid}`
                        };
                    } else if (connectedIndex !== slot.index) {
                        message = {
                            type: 'error',
                            text: `Connection test reached slot ${connectedIndex + 1}. Expected slot ${slot.index + 1}.`
                        };
                    } else {
                        message = { type: 'success', text: `Connected to ${slot.ssid}` };
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
        if (wifiControlsDisabled || wifiOrdering) return;
        const updates = wifiPriorityUpdates(slot, direction);
        if (updates.length === 0) return;

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
                resultMessage = { type: 'error', text: res.status === 409 ? WIFI_NETWORKS_CHANGED_TEXT : 'Failed to reorder WiFi networks' };
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
        if (!settingsLoaded || saving || restoring) return;

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
        if (!componentMounted || restoring || saving || wifiSavedMutationBusy) return;
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
            // Submission can commit even if its response is lost. Retire every
            // older form read until a complete post-request readback succeeds.
            settingsRunId += 1;
            settingsLoaded = false;
            restoreReadbackRequired = true;
            invalidateSavedWifiNetworks();
            // The restore replaces slot identities. Neither old drafts nor a
            // pre-restore Test/status response may authorize or describe them.
            retireSavedWifiNetworks();
            wifiStatusRunId += 1;
            wifiTestRunId += 1;
            wifiTestingIndex = null;
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
                // Supersede earlier operation feedback, but wait for readback
                // before reporting that the restored settings are ready.
                message = null;
                restoreFile = null;
                restoreSucceeded = true;
            } else {
                message = { type: 'error', text: data.error || 'Failed to restore backup' };
            }
        } catch (e) {
            if (componentMounted) message = {
                type: 'error',
                text: restoreSubmitted
                    ? 'Restore response was lost or unreadable. Check the current settings before continuing.'
                    : 'Failed to read backup file'
            };
        } finally {
            if (restoreSubmitted && componentMounted) {
                const [settingsReloaded] = await Promise.all([
                    fetchSettings({ force: true }),
                    refreshWifiAfterMutation({ afterRestore: true })
                ]);
                if (componentMounted && restoreSucceeded && settingsReloaded &&
                    !wifiNetworksReadbackRequired && message === null) {
                    message = { type: 'success', text: 'Settings restored and reloaded.' };
                }
            }
            restoring = false;
        }
    }
</script>

{#snippet wifiReadbackNotice()}
    {#if wifiNetworksReadbackRequired}
        <div class="surface-note space-y-2" role="status">
            <p>Saved network controls are read-only until the current networks can be loaded.</p>
            <button class="btn btn-outline btn-sm" onclick={retrySavedWifiNetworks}
                disabled={restoring || wifiNetworksLoading || wifiSavedMutationBusy}>
                {wifiNetworksLoading ? 'Loading saved networks…' : 'Retry saved networks'}
            </button>
        </div>
    {/if}
{/snippet}

<div class="page-stack">
    <PageHeader title="Settings" subtitle="Network, power, and backup configuration." />

    <StatusAlert {message} />

    {#if loading}
        <div class="state-loading">
            <span class="loading loading-lg loading-spinner"></span>
        </div>
    {:else}
        {#if !settingsLoaded && !restoring}
            <button class="btn btn-outline" onclick={() => fetchSettings({ force: true })}>
                Retry settings
            </button>
        {/if}

        <fieldset class="surface-card" disabled={saving || restoring || !settingsLoaded}>
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

        {#if !wifiEditor.open && !wifiScan.open}
            {@render wifiReadbackNotice()}
        {/if}

        <fieldset class="surface-card" disabled={wifiControlsDisabled}>
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

                        {#if !wifiNetworksLoading && !wifiNetworksReadbackRequired && !hasConfiguredWifiSlots()}
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
                                                    disabled={wifiSlotChangePending(slot.index)}
                                                >
                                                    Edit
                                                </button>
                                                <button
                                                    class="btn btn-outline btn-xs"
                                                    onclick={() => testWifiSlot(slot)}
                                                    disabled={wifiTestingIndex !== null || wifiSlotChangePending(slot.index)}
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
                                                        wifiPriorityUpdates(slot, -1).length === 0}
                                                >
                                                    ↑
                                                </button>
                                                <button
                                                    class="btn btn-ghost btn-xs"
                                                    aria-label={`Move ${slot.ssid} down`}
                                                    onclick={() => moveWifiSlot(slot, 1)}
                                                    disabled={wifiOrdering || wifiPriorityUpdates(slot, 1).length === 0}
                                                >
                                                    ↓
                                                </button>
                                                <button
                                                    class="btn btn-outline btn-error btn-xs"
                                                    onclick={() => deleteWifiSlot(slot)}
                                                    disabled={wifiSlotChangePending(slot.index)}
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
                                                    disabled={wifiSlotChangePending(slot.index)}
                                                >
                                                    Enter manually
                                                </button>
                                                <button
                                                    class="btn btn-outline btn-xs"
                                                    onclick={() => startWifiScan(slot.index)}
                                                    disabled={wifiSlotChangePending(slot.index)}
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
        </fieldset>

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
                    saveBlocked={wifiControlsDisabled || wifiScanSlotChanged || wifiScanAppendUnavailable ||
                        wifiSlotChangePending(wifiScanTargetIndex)}
                    slotChanged={wifiScanSlotChanged}
                    appendUnavailable={Boolean(wifiScanAppendUnavailable)}
                    recovery={wifiReadbackNotice}
                    onstartWifiScan={startWifiScan}
                    onselectNetwork={selectNetwork}
                    onconnectToNetwork={saveSelectedScanNetwork}
                    oncloseWifiModal={closeWifiModal}
                />
            {:else if wifiModalLoading}
                <dialog use:nativeDialog class="modal" aria-label="Loading WiFi picker"
                    oncancel={(event) => { event.preventDefault(); closeWifiModal(); }}>
                    <div class="surface-modal modal-box max-w-md">
                        <div class="state-loading stack">
                            <span class="loading loading-md loading-spinner"></span>
                            <p class="copy-muted">Loading WiFi modal...</p>
                        </div>
                    </div>
                </dialog>
            {/if}
        {/if}

        {#if wifiEditor.open}
            <dialog use:nativeDialog class="modal"
                aria-label={wifiEditor.mode === 'edit' ? 'Edit Saved Network' : 'Add Saved Network'}
                oncancel={(event) => { event.preventDefault(); closeWifiEditor(); }}>
                <div class="surface-modal modal-box max-w-md">
                    <h3 class="text-lg font-bold">
                        {wifiEditor.mode === 'edit' ? 'Edit Saved Network' : 'Add Saved Network'}
                    </h3>
                    {@render wifiReadbackNotice()}
                    {#if wifiEditorSlotChanged}
                        <p class="surface-note" role="status">This saved network slot changed. Close the editor and select the current slot again.</p>
                    {/if}
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
                                bind:value={() => wifiEditor.ssid, enterWifiSsid}
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
                                bind:value={() => wifiEditor.priority ?? wifiEditorDefaultPriority(),
                                    (priority) => { wifiEditor.priority = priority ?? null; }}
                            />
                            <div class="label">
                                <span class="field-hint"
                                    >0 is tried first; higher numbers are lower priority.</span
                                >
                            </div>
                            {#if wifiEditorAppendUnavailable}
                                <p class="surface-note" role="status">Choose a priority for this network or reorder saved networks first.</p>
                            {/if}
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
                            disabled={wifiControlsDisabled || wifiEditorSlotChanged || wifiEditorAppendUnavailable ||
                                wifiSlotChangePending(wifiEditorTargetIndex) ||
                                wifiNetworkActionInFlight || (wifiOrdering && wifiEditor.priority !== null) || !wifiEditor.ssid}
                        >
                            {#if wifiNetworkActionInFlight}
                                <span class="loading loading-sm loading-spinner"></span>
                            {/if}
                            Save
                        </button>
                    </div>
                </div>
                <button
                    class="modal-backdrop bg-black/50"
                    tabindex="-1"
                    aria-label="Close saved network editor"
                    onclick={closeWifiEditor}
                    disabled={wifiNetworkActionInFlight}
                ></button>
            </dialog>
        {/if}

        <fieldset disabled={saving || restoring || !settingsLoaded}>
            <SettingsAutoPowerOffCard bind:settings />
        </fieldset>

        <button
            class="btn btn-block btn-primary"
            onclick={saveSettings}
            disabled={saving || restoring || !settingsLoaded}
        >
            {#if saving}
                <span class="loading loading-sm loading-spinner"></span>
            {/if}
            Save Settings
        </button>

        <SettingsBackupCard
            {backingUpNow}
            mutationBusy={saving || restoring || backingUpNow || wifiToggleInFlight ||
                wifiNetworkActionInFlight || wifiOrdering || wifiScan.saving}
            onbackupNowToSd={backupNowToSd}
            ondownloadBackup={downloadBackup}
            {restoreFile}
            {restoring}
            restoreDisabled={saving || wifiSavedMutationBusy}
            onfileSelect={handleFileSelect}
            onrestoreBackup={restoreBackup}
        />
    {/if}
</div>
