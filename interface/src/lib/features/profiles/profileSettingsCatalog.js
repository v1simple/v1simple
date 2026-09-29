// Named user-byte controls only; Custom Frequencies keeps its range editor.
// Newer Gen2 thresholds follow src/v1_firmware_compat.h. Absence of a threshold
// does not establish support for unknown, Gen1, or future-major firmware.
export const PROFILE_SETTING_GROUPS = [
    {
        id: 'bands',
        title: 'Detection bands & sensitivity',
        description: 'Choose which signals the detector reports and its sensitivity.',
        fields: [
            {
                key: 'laser',
                label: 'Laser',
                description: 'If ALP and “Disable V1 laser alerts on Auto-Push” are enabled, applying a profile turns V1 laser detection off.'
            },
            { key: 'laserRear', label: 'Rear Laser' },
            { key: 'x', label: 'X Band' },
            { key: 'ka', label: 'Ka Band' },
            { key: 'k', label: 'K Band' },
            { key: 'ku', label: 'Ku Band' },
            {
                key: 'kaSensitivity', label: 'Ka Sensitivity', minFirmware: 41032,
                options: [
                    { value: 1, label: 'Relaxed' },
                    { value: 2, label: 'Original' },
                    { value: 3, label: 'Full' }
                ]
            },
            {
                key: 'kSensitivity', label: 'K Sensitivity', minFirmware: 41037,
                options: [
                    { value: 1, label: 'Relaxed' },
                    { value: 2, label: 'Full' },
                    { value: 3, label: 'Original' }
                ]
            },
            {
                key: 'xSensitivity', label: 'X Sensitivity', minFirmware: 41037,
                options: [
                    { value: 1, label: 'Relaxed' },
                    { value: 2, label: 'Full' },
                    { value: 3, label: 'Original' }
                ]
            }
        ]
    },
    {
        id: 'muting',
        title: 'Muting behavior',
        description: 'Choose how the detector handles muted alerts.',
        fields: [
            { key: 'muteToMuteVolume', label: 'Mute-to-Muted Volume' },
            { key: 'bogeyLockLoud', label: 'Bogey-Lock tone Loud after muting' },
            { key: 'muteXKRear', label: 'Mute Rear X & K alerts' },
            {
                key: 'autoMute', label: 'Auto Mute', ariaLabel: 'X, K, Ku Automute', minFirmware: 41036,
                description: 'Applies to X, K and Ku alerts.',
                options: [
                    { value: 2, label: 'On' },
                    { value: 1, label: 'Advanced' },
                    { value: 3, label: 'Off' }
                ]
            }
        ]
    },
    {
        id: 'photo',
        title: 'Photo Radar',
        description: 'Choose Photo Radar types and their filters.',
        fields: [
            { key: 'photoVerifier', label: 'Photo Verifier', minFirmware: 41037 },
            { key: 'mrct', label: 'MRCT', minFirmware: 41037 },
            { key: 'driveSafe3D', label: 'DriveSafe™ 3D', minFirmware: 41037 },
            { key: 'driveSafe3DHD', label: 'DriveSafe™ 3DHD', minFirmware: 41037 },
            { key: 'redflexHalo', label: 'Redflex® Halo', minFirmware: 41037 },
            { key: 'redflexNK7', label: 'Redflex® NK7', minFirmware: 41037 },
            { key: 'ekin', label: 'Ekin', minFirmware: 41037 },
            { key: 'gatsoRT4', label: 'Gatso RT4', minFirmware: 41039 },
            {
                key: 'photoIntersectionFilter',
                label: 'Photo Radar Intersection Management Filter',
                ariaLabel: 'Intersection Management Filter',
                minFirmware: 41039,
                description: 'Suppresses DriveSafe 3D, DriveSafe 3DHD and Ekin alerts while enabled. Their saved settings are kept.'
            }
        ]
    },
    {
        id: 'filtering',
        title: 'Filtering & startup',
        description: 'Region, filtering, alert priority and detector startup behavior.',
        fields: [
            {
                key: 'euroMode', label: 'Euro Mode',
                description: 'Advanced Logic is unavailable in Euro mode. Changing region resets the detector’s custom frequency table before any profile ranges are applied.'
            },
            { key: 'kVerifier', label: 'K-Verifier (TMF)' },
            { key: 'kaAlwaysPriority', label: 'Ka Always Radar Priority', minFirmware: 41031 },
            { key: 'fastLaserDetect', label: 'Fast Laser Detection', minFirmware: 41031 },
            { key: 'startupSequence', label: 'Startup Sequence', minFirmware: 41035 },
            { key: 'restingDisplay', label: 'Resting Display', minFirmware: 41035 },
            { key: 'bsmPlus', label: 'BSM Plus', minFirmware: 41035 }
        ]
    }
];
