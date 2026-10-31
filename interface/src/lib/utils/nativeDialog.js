// The browser owns modal focus and background inertness.
export function nativeDialog(dialog) {
    const opener = document.activeElement;
    dialog.showModal();
    return {
        destroy() {
            dialog.close();
            // Svelte may remove the dialog before teardown, bypassing native
            // focus return. A replacement dialog keeps focus ownership.
            if (opener?.isConnected && !document.querySelector('dialog[open]')) {
                opener.focus();
            }
        }
    };
}
