export const THEMES = [
    { value: 'amethyst', label: 'Amethyst' },
    { value: 'observiply', label: 'Observiply' },
    { value: 'ocean', label: 'Ocean' },
    { value: 'forest', label: 'Forest' },
    { value: 'sunset', label: 'Sunset' },
    { value: 'ruby', label: 'Ruby' }
];

export const DEFAULT_THEME = 'amethyst';
export const DEFAULT_MODE = 'dark';
export const THEME_STORAGE_KEY = 'v1simple:theme';
export const MODE_STORAGE_KEY = 'v1simple:colorMode';

const allowedThemes = new Set(THEMES.map(({ value }) => value));
const allowedModes = new Set(['dark', 'light']);

export function validatedPreferences(storage = localStorage) {
    const storedTheme = storage.getItem(THEME_STORAGE_KEY);
    const storedMode = storage.getItem(MODE_STORAGE_KEY);
    return {
        theme: allowedThemes.has(storedTheme) ? storedTheme : DEFAULT_THEME,
        mode: allowedModes.has(storedMode) ? storedMode : DEFAULT_MODE
    };
}

export function applyPreferences(
    preferences,
    { root = document.documentElement, storage = localStorage, documentRef = document } = {}
) {
    const theme = allowedThemes.has(preferences?.theme) ? preferences.theme : DEFAULT_THEME;
    const mode = allowedModes.has(preferences?.mode) ? preferences.mode : DEFAULT_MODE;

    root.dataset.theme = theme;
    root.classList.remove('dark', 'light');
    root.classList.add(mode);
    root.style.colorScheme = mode;
    storage.setItem(THEME_STORAGE_KEY, theme);
    storage.setItem(MODE_STORAGE_KEY, mode);

    const themeColor = documentRef.querySelector('meta[name="theme-color"]');
    themeColor?.setAttribute('content', mode === 'dark' ? '#17151a' : '#f8f7f9');
    return { theme, mode };
}

export function restorePreferences(options) {
    return applyPreferences(validatedPreferences(options?.storage), options);
}
