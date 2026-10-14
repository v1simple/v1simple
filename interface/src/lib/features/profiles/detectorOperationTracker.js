import { fetchWithTimeout } from '#lib/utils/poll.js';
import {
    DETECTOR_OPERATION_POLL_WINDOW_MS,
    DETECTOR_OPERATION_STATES,
    forgetDetectorOperationId,
    readDetectorOperationId,
    rememberDetectorOperationId,
    validDetectorOperationStatus
} from './detectorOperation';

// Each mounted page owns one tracker. Requests to start an operation and their
// admission checks stay with the page; this follows only the accepted identity.
export function createDetectorOperationTracker({ storage, onStatus, onMessage }) {
    let status = null;
    let timer = null;
    let deadline = 0;
    let failures = 0;
    let epoch = 0;
    let disposed = false;

    function publishStatus(next) {
        status = next;
        onStatus(next);
    }

    function clearPoll() {
        if (timer !== null) clearTimeout(timer);
        timer = null;
    }

    function schedulePoll() {
        if (disposed) return;
        if (Date.now() >= deadline) {
            onMessage({
                type: 'warning',
                text: 'Detector operation status polling paused after five minutes. Reload to resume the exact operation.'
            });
            return;
        }
        clearPoll();
        const delay = Math.min(5000, 750 * (2 ** Math.min(failures, 3)));
        timer = setTimeout(() => void poll(), delay);
    }

    function isCurrent(pollEpoch, operationId) {
        return !disposed && pollEpoch === epoch && readDetectorOperationId(storage) === operationId;
    }

    async function poll() {
        const operationId = readDetectorOperationId(storage);
        if (disposed || !operationId) return;
        const pollEpoch = epoch;
        try {
            const res = await fetchWithTimeout(
                `/api/autopush/status?operationId=${encodeURIComponent(operationId)}`,
                {}, undefined, async (response) => ({
                    status: response.status,
                    ok: response.ok,
                    body: await response.json()
                })
            );
            if (!isCurrent(pollEpoch, operationId)) return;
            if (res.status === 409) {
                publishStatus(null);
                forgetDetectorOperationId(storage);
                onMessage({
                    type: 'error',
                    text: `Saved operation ${operationId} does not match the detector's current operation. No status was reused.`
                });
                return;
            }
            if (!res.ok) {
                failures += 1;
                schedulePoll();
                return;
            }
            const next = res.body;
            if (!validDetectorOperationStatus(next, operationId)) {
                publishStatus(null);
                onMessage({ type: 'error', text: 'Detector operation returned malformed or mismatched status.' });
                return;
            }
            const priorState = status?.operationId === operationId ? status.state : null;
            const priorRank = DETECTOR_OPERATION_STATES.indexOf(priorState);
            const nextRank = DETECTOR_OPERATION_STATES.indexOf(next.state);
            if (priorRank >= 0 && !next.terminal && nextRank < priorRank) {
                onMessage({ type: 'error', text: 'Detector operation status regressed; stale status was rejected.' });
                return;
            }
            publishStatus(next);
            failures = 0;
            if (next.terminal) forgetDetectorOperationId(storage);
            else schedulePoll();
        } catch {
            if (!isCurrent(pollEpoch, operationId)) return;
            // Normal mode intentionally takes Wi-Fi down while the operation runs.
            failures += 1;
            schedulePoll();
        }
    }

    function resetPollWindow() {
        epoch += 1;
        clearPoll();
        deadline = Date.now() + DETECTOR_OPERATION_POLL_WINDOW_MS;
        failures = 0;
    }

    return {
        resume() {
            if (disposed) return;
            resetPollWindow();
            return poll();
        },
        accepted(response) {
            // Keep the resume hint even if the start request completes after
            // navigation. The device has already accepted this operation.
            rememberDetectorOperationId(storage, response.operationId);
            if (disposed) return;
            resetPollWindow();
            publishStatus({
                operationId: response.operationId,
                state: response.state,
                result: 'in_progress',
                terminal: false,
                components: {}
            });
            schedulePoll();
        },
        dispose() {
            disposed = true;
            epoch += 1;
            clearPoll();
        }
    };
}
