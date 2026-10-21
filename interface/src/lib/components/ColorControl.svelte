<script>
    import { rgb565ToHex, rgb565ToHexStr } from '#lib/utils/colors.js';

    let {
        id,
        label,
        labelHint = '',
        value = 0,
        ariaLabel = label,
        swatchSize = 'md',
        inputClass = 'input input-xs w-16 font-mono text-xs',
        disabled = false,
        placeholder = 'F800',
        onPick = null,
        onHexChange = null,
        onHexInput = null,
        children
    } = $props();

    const swatchClassBySize = {
        sm: 'color-swatch-btn sm',
        md: 'color-swatch-btn md',
        lg: 'color-swatch-btn lg'
    };

    // Keep unfinished text separate from the numeric color. Readbacks and
    // swatch previews must not replace a focused, uncommitted hex draft.
    let hexDraft = $state(null);

    function handleHexInput(event) {
        hexDraft = event.currentTarget.value;
        onHexInput?.(hexDraft);
    }

    function handleHexChange(event) {
        const raw = event.currentTarget.value;
        hexDraft = null;
        onHexChange?.(raw);
    }
</script>

<div class="field-control">
    <label class="label" for={id}>
        <span class="field-label">{label}</span>
        {#if labelHint}
            <span class="field-hint copy-micro">{labelHint}</span>
        {/if}
    </label>
    <div class="flex items-center gap-2">
        <button
            {id}
            type="button"
            aria-label={ariaLabel}
            class={swatchClassBySize[swatchSize] ?? swatchClassBySize.md}
            style={`background-color: ${rgb565ToHex(value)}`}
            {disabled}
            onclick={onPick}
        ></button>
        <input
            type="text"
            class={inputClass}
            value={hexDraft ?? rgb565ToHexStr(value)}
            title="RGB565 hex (or RGB888)"
            {placeholder}
            {disabled}
            oninput={handleHexInput}
            onchange={handleHexChange}
        />
        {#if children}
            <div class="flex items-center gap-1">
                {@render children()}
            </div>
        {/if}
    </div>
</div>
