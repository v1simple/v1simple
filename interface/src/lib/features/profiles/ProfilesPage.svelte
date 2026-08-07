<script>
    import { onMount } from 'svelte';
    import { fetchWithTimeout } from '$lib/utils/poll';
    import PageHeader from '$lib/components/PageHeader.svelte';
    import StatusAlert from '$lib/components/StatusAlert.svelte';
    import ProfileSaveDialog from '$lib/features/profiles/ProfileSaveDialog.svelte';
    import ProfileSavedListCard from '$lib/features/profiles/ProfileSavedListCard.svelte';
    import ProfileSettingsPanel from '$lib/features/profiles/ProfileSettingsPanel.svelte';
    import {
        createDefaultProfileSettings,
        fromApiSettings,
        toApiSettings
    } from '$lib/features/profiles/profileSettingsAdapter';

    let profiles = $state([]);
    let currentProfile = $state(null);
    let loading = $state(true);
    let message = $state(null);
    let showSaveDialog = $state(false);
    let saveName = $state('');
    let saveDescription = $state('');
    let editingSettings = $state(false);
    let editedSettings = $state(null);
    let editDescription = $state('');
    const PROFILE_LOAD_ERROR_TEXT = 'Failed to load profiles';

    function clearMessageText(text) {
        if (message?.text === text) {
            message = null;
        }
    }

    onMount(() => {
        void fetchProfiles();
    });

    async function fetchProfiles() {
        try {
            const res = await fetchWithTimeout('/api/v1/profiles');
            if (res.ok) {
                const data = await res.json();
                profiles = data.profiles || [];
                clearMessageText(PROFILE_LOAD_ERROR_TEXT);
            } else {
                message = { type: 'error', text: PROFILE_LOAD_ERROR_TEXT };
            }
        } catch (e) {
            message = { type: 'error', text: PROFILE_LOAD_ERROR_TEXT };
        } finally {
            loading = false;
        }
    }

    async function saveCurrentProfile() {
        if (!saveName.trim()) {
            message = { type: 'error', text: 'Profile name required' };
            return;
        }

        const settingsToSave =
            editingSettings && editedSettings ? editedSettings : currentProfile?.settings;

        if (!settingsToSave) {
            message = { type: 'error', text: 'No settings to save' };
            return;
        }

        try {
            const payload = {
                name: saveName.trim(),
                description: saveDescription.trim(),
                settings: toApiSettings(settingsToSave)
            };

            const res = await fetchWithTimeout('/api/v1/profile', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(payload)
            });

            if (res.ok) {
                message = { type: 'success', text: `Profile "${saveName}" saved` };
                showSaveDialog = false;
                await fetchProfiles();
            } else {
                const error = await res.text();
                message = { type: 'error', text: `Failed to save: ${error}` };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        }
    }

    function startEditing() {
        if (currentProfile && currentProfile.settings) {
            editedSettings = { ...currentProfile.settings };
            editDescription = currentProfile.description || '';
            editingSettings = true;
        }
    }

    function cancelEditing() {
        editedSettings = null;
        editDescription = '';
        editingSettings = false;
    }

    function openSaveDialog() {
        if (editingSettings && editDescription && !saveDescription) {
            saveDescription = editDescription;
        }
        showSaveDialog = true;
    }

    function closeSaveDialog() {
        showSaveDialog = false;
    }

    async function editProfile(name) {
        message = { type: 'info', text: `Loading ${name}...` };
        try {
            const res = await fetchWithTimeout(`/api/v1/profile?name=${encodeURIComponent(name)}`);
            if (res.ok) {
                const data = await res.json();
                currentProfile = {
                    ...data,
                    settings: fromApiSettings(data.settings || {})
                };
                editedSettings = { ...currentProfile.settings };
                editDescription = data.description || '';
                editingSettings = true;
                message = { type: 'info', text: `Editing ${name}` };
            } else {
                const error = await res.text();
                message = { type: 'error', text: `Failed to load: ${error}` };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        }
    }

    function createNewProfile() {
        currentProfile = {
            available: true,
            draft: true,
            name: '',
            description: '',
            settings: createDefaultProfileSettings()
        };
        editedSettings = createDefaultProfileSettings();
        editDescription = '';
        saveName = '';
        saveDescription = '';
        editingSettings = true;
        message = {
            type: 'info',
            text: 'Creating an offline V1 profile. Save it, then assign it on the Auto-Push page.'
        };
    }

    async function saveEditedProfile() {
        if (!editedSettings || !currentProfile || !currentProfile.name) {
            message = { type: 'error', text: 'No profile loaded to save' };
            return;
        }

        message = { type: 'info', text: `Saving ${currentProfile.name}...` };
        try {
            const payload = {
                name: currentProfile.name,
                description: editDescription.trim(),
                settings: toApiSettings(editedSettings)
            };

            const res = await fetchWithTimeout('/api/v1/profile', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(payload)
            });

            if (res.ok) {
                message = { type: 'success', text: `Profile "${currentProfile.name}" saved` };
                currentProfile = {
                    ...currentProfile,
                    description: editDescription.trim(),
                    settings: { ...editedSettings }
                };
                editingSettings = false;
                editedSettings = null;
                await fetchProfiles();
            } else {
                const error = await res.text();
                message = { type: 'error', text: `Failed to save: ${error}` };
            }
        } catch (e) {
            message = { type: 'error', text: 'Connection error' };
        }
    }

    async function deleteProfile(name) {
        if (!confirm(`Delete profile "${name}"?`)) return;

        try {
            const res = await fetchWithTimeout('/api/v1/profile/delete', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify({ name })
            });
            if (res.ok) {
                profiles = profiles.filter((profile) => profile.name !== name);
                message = { type: 'success', text: 'Profile deleted' };
            } else {
                const errorData = await res.json().catch(() => ({}));
                const error = errorData?.error || errorData?.message;
                message = {
                    type: 'error',
                    text: error ? `Failed to delete: ${error}` : 'Failed to delete'
                };
            }
        } catch (e) {
            message = { type: 'error', text: 'Failed to delete' };
        }
    }
</script>

<div class="page-stack">
    <PageHeader
        title="V1 Profiles"
        subtitle="Create, edit, and save profiles for automatic use during normal operation."
    >
        <div class="badge badge-info">Offline authoring</div>
    </PageHeader>

    <StatusAlert {message} />

    <ProfileSaveDialog
        open={showSaveDialog}
        bind:saveName
        bind:saveDescription
        oncancel={closeSaveDialog}
        onsave={saveCurrentProfile}
    />

    <ProfileSettingsPanel
        {editingSettings}
        {currentProfile}
        bind:editedSettings
        bind:editDescription
        oncancelEditing={cancelEditing}
        onsaveEditedProfile={saveEditedProfile}
        oncreateNewProfile={createNewProfile}
        onstartEditing={startEditing}
        onshowSaveDialog={openSaveDialog}
    />

    <ProfileSavedListCard
        {loading}
        {profiles}
        oneditProfile={editProfile}
        ondeleteProfile={deleteProfile}
    />

    <div class="surface-note copy-muted space-y-1">
        <p><strong>Create:</strong> Build a detector configuration without a V1 connection.</p>
        <p><strong>Edit:</strong> Update or delete saved profiles during maintenance.</p>
        <p>
            <strong>Apply:</strong> Assign a saved profile on the Auto-Push page. It is sent automatically
            after the next matching V1 connection in normal operation.
        </p>
    </div>
</div>
