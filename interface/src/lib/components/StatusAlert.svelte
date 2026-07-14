<script>
    let { message = null, fallbackType = 'info', dismiss = null, busy = false } = $props();

    const TYPE_BY_MESSAGE = {
        error: 'error',
        success: 'success',
        warning: 'warning',
        info: 'info'
    };

    const TYPE_BY_FALLBACK = {
        error: 'error',
        info: 'info',
        warning: 'warning',
        success: 'success'
    };

    function resolveType(type) {
        return TYPE_BY_MESSAGE[type] ?? TYPE_BY_FALLBACK[fallbackType] ?? 'info';
    }
</script>

{#if message}
    <div class="surface-alert alert-{resolveType(message?.type)}" role="status" aria-live="polite">
        {#if busy}
            <span class="loading loading-spinner loading-sm"></span>
        {/if}
        <span>{message?.text ?? message}</span>
        {#if dismiss}
            <button class="btn btn-ghost btn-xs" onclick={dismiss} aria-label="Dismiss message"
                >✕</button
            >
        {/if}
    </div>
{/if}
