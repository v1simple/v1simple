<script>
    import { onMount } from 'svelte';
    import {
        applyPreferences,
        DEFAULT_MODE,
        DEFAULT_THEME,
        restorePreferences,
        THEMES
    } from '$lib/theme.js';

    let theme = $state(DEFAULT_THEME);
    let mode = $state(DEFAULT_MODE);
    let compactMenu = $state(null);

    onMount(() => {
        ({ theme, mode } = restorePreferences());
    });

    function selectTheme(nextTheme) {
        ({ theme, mode } = applyPreferences({ theme: nextTheme, mode }));
        if (compactMenu) compactMenu.open = false;
    }

    function toggleMode() {
        ({ theme, mode } = applyPreferences({ theme, mode: mode === 'dark' ? 'light' : 'dark' }));
    }
</script>

<div class="theme-controls theme-controls-desktop" aria-label="Appearance">
    <div class="theme-swatches" role="group" aria-label="Color theme">
        {#each THEMES as option}
            <button
                type="button"
                class="theme-swatch theme-{option.value}"
                class:selected={theme === option.value}
                aria-label="Use {option.label} theme"
                aria-pressed={theme === option.value}
                title={option.label}
                onclick={() => selectTheme(option.value)}
            ></button>
        {/each}
    </div>
    <button
        type="button"
        class="btn btn-ghost btn-square btn-sm theme-mode-button"
        aria-label={mode === 'dark' ? 'Use light mode' : 'Use dark mode'}
        aria-pressed={mode === 'light'}
        title={mode === 'dark' ? 'Use light mode' : 'Use dark mode'}
        onclick={toggleMode}
    >
        {#if mode === 'dark'}
            <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="4"></circle><path d="M12 2v2M12 20v2M4.93 4.93l1.42 1.42M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.42-1.42M17.66 6.34l1.41-1.41"></path></svg>
        {:else}
            <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M20.3 15.7A9 9 0 0 1 8.3 3.7 9 9 0 1 0 20.3 15.7z"></path></svg>
        {/if}
    </button>
</div>

<details class="theme-controls-compact" bind:this={compactMenu}>
    <summary class="btn btn-ghost btn-square btn-sm" aria-label="Choose appearance">
        <span class="theme-current theme-{theme}" aria-hidden="true"></span>
    </summary>
    <div class="theme-popover" role="group" aria-label="Appearance">
        <div class="theme-popover-heading">Color theme</div>
        <div class="theme-choice-grid">
            {#each THEMES as option}
                <button
                    type="button"
                    class="theme-choice"
                    class:selected={theme === option.value}
                    aria-label="Use {option.label} theme"
                    aria-pressed={theme === option.value}
                    onclick={() => selectTheme(option.value)}
                >
                    <span class="theme-swatch theme-{option.value}" aria-hidden="true"></span>
                    <span>{option.label}</span>
                </button>
            {/each}
        </div>
        <button type="button" class="btn btn-ghost btn-sm theme-mode-choice" onclick={toggleMode}>
            {mode === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'}
        </button>
    </div>
</details>
