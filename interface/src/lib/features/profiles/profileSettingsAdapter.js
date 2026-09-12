const DEFAULT_PROFILE_SETTINGS = Object.freeze({
    // V1UserSettings factory default is all six bytes set to 0xff. Keep the UI
    // defaults aligned with the firmware's byte-level default so an offline
    // profile starts from a valid V1 configuration instead of zero-valued fields.
    x: true,
    k: true,
    ka: true,
    laser: true,
    muteToMuteVolume: true,
    bogeyLockLoud: true,
    muteXKRear: false,
    ku: false,
    euroMode: false,
    kVerifier: true,
    laserRear: true,
    customFreqs: false,
    kaAlwaysPriority: false,
    fastLaserDetect: true,
    kaSensitivity: 3,
    startupSequence: true,
    restingDisplay: true,
    bsmPlus: false,
    autoMute: 3,
    kSensitivity: 3,
    mrct: false,
    xSensitivity: 3,
    driveSafe3D: false,
    driveSafe3DHD: false,
    redflexHalo: false,
    redflexNK7: false,
    ekin: false,
    photoVerifier: false,
    gatsoRT4: false,
    photoIntersectionFilter: false
});

export function createDefaultProfileSettings() {
    return { ...DEFAULT_PROFILE_SETTINGS };
}

export function createDefaultDetectorConfiguration() {
    return {
        userSettings: 'value',
        modePolicy: 'unchanged',
        mode: 1,
        display: 'unchanged',
        volumePolicy: 'unchanged',
        mainVolume: 5,
        mutedVolume: 0,
        bluetoothLed: 'unchanged',
        customFrequencies: 'unchanged'
    };
}

export function fromApiDetectorConfiguration(api = {}) {
    const defaults = createDefaultDetectorConfiguration();
    return {
        ...defaults,
        userSettings: api.userSettings === 'unchanged' ? 'unchanged' : 'value',
        modePolicy: api.mode?.policy === 'value' ? 'value' : 'unchanged',
        mode: Number(api.mode?.value ?? defaults.mode),
        display: ['on', 'off'].includes(api.display) ? api.display : 'unchanged',
        volumePolicy: ['temporary', 'saved'].includes(api.volume?.policy)
            ? api.volume.policy
            : 'unchanged',
        mainVolume: Number(api.volume?.main ?? defaults.mainVolume),
        mutedVolume: Number(api.volume?.muted ?? defaults.mutedVolume)
    };
}

export function toApiDetectorConfiguration(ui = {}) {
    const modePolicy = ui.modePolicy === 'value' ? 'value' : 'unchanged';
    const volumePolicy = ['temporary', 'saved'].includes(ui.volumePolicy)
        ? ui.volumePolicy
        : 'unchanged';
    return {
        userSettings: ui.userSettings === 'unchanged' ? 'unchanged' : 'value',
        mode: modePolicy === 'value'
            ? { policy: 'value', value: Number(ui.mode) }
            : { policy: 'unchanged' },
        display: ['on', 'off'].includes(ui.display) ? ui.display : 'unchanged',
        volume: volumePolicy === 'unchanged'
            ? { policy: 'unchanged' }
            : {
                  policy: volumePolicy,
                  main: Number(ui.mainVolume),
                  muted: Number(ui.mutedVolume)
              },
        bluetoothLed: 'unchanged',
        customFrequencies: 'unchanged'
    };
}

export function detectorConfigurationFromSnapshot(snapshot = {}) {
    const detector = createDefaultDetectorConfiguration();
    const mode = snapshot.observations?.mode;
    if (mode?.available) {
        const modeValue = String(mode.value ?? '');
        if (['A', 'C', 'U'].includes(modeValue)) {
            detector.modePolicy = 'value';
            detector.mode = 1;
        } else if (['l', 'c', 'u'].includes(modeValue)) {
            detector.modePolicy = 'value';
            detector.mode = 2;
        } else if (modeValue === 'L') {
            detector.modePolicy = 'value';
            detector.mode = 3;
        }
    }
    const display = snapshot.observations?.displayOn;
    if (display?.available) detector.display = display.value ? 'on' : 'off';
    const saved = snapshot.observations?.savedVolume;
    const current = snapshot.observations?.currentVolume;
    if (saved?.available) {
        detector.volumePolicy = 'saved';
        detector.mainVolume = Number(saved.main);
        detector.mutedVolume = Number(saved.muted);
    } else if (current?.available) {
        detector.volumePolicy = 'temporary';
        detector.mainVolume = Number(current.main);
        detector.mutedVolume = Number(current.muted);
    }
    return detector;
}

export function fromApiSettings(api = {}) {
    return {
        baseBytes: Array.isArray(api.bytes) && api.bytes.length === 6 ? [...api.bytes] : undefined,
        ka: api.kaBand ?? api.ka ?? false,
        k: api.kBand ?? api.k ?? false,
        x: api.xBand ?? api.x ?? false,
        ku: api.kuBand ?? api.ku ?? false,
        laser: api.laser ?? false,
        euroMode: api.euro ?? api.euroMode ?? false,
        muteToMuteVolume: api.muteToMuteVolume ?? true,
        kVerifier: api.kVerifier ?? false,
        fastLaserDetect: api.fastLaserDetect ?? false,
        laserRear: api.laserRear ?? false,
        customFreqs: api.customFreqs ?? false,
        kaAlwaysPriority: api.kaAlwaysPriority ?? false,
        kaSensitivity: Number(api.kaSensitivity ?? 0),
        kSensitivity: Number(api.kSensitivity ?? 0),
        xSensitivity: Number(api.xSensitivity ?? 0),
        autoMute: Number(api.autoMute ?? 0),
        bogeyLockLoud: api.bogeyLockLoud ?? false,
        muteXKRear: api.muteXKRear ?? false,
        startupSequence: api.startupSequence ?? false,
        restingDisplay: api.restingDisplay ?? false,
        bsmPlus: api.bsmPlus ?? false,
        mrct: api.mrct ?? false,
        driveSafe3D: api.driveSafe3D ?? false,
        driveSafe3DHD: api.driveSafe3DHD ?? false,
        redflexHalo: api.redflexHalo ?? false,
        redflexNK7: api.redflexNK7 ?? false,
        ekin: api.ekin ?? false,
        photoVerifier: api.photoVerifier ?? false,
        gatsoRT4: api.gatsoRT4 ?? false,
        photoIntersectionFilter: api.photoIntersectionFilter ?? false
    };
}

export function toApiSettings(ui = {}) {
    return {
        ...(Array.isArray(ui.baseBytes) && ui.baseBytes.length === 6
            ? { baseBytes: [...ui.baseBytes] }
            : {}),
        xBand: ui.x ?? ui.xBand ?? false,
        kBand: ui.k ?? ui.kBand ?? false,
        kaBand: ui.ka ?? ui.kaBand ?? false,
        laser: ui.laser ?? false,
        kuBand: ui.ku ?? ui.kuBand ?? false,
        euro: ui.euroMode ?? ui.euro ?? false,
        muteToMuteVolume: ui.muteToMuteVolume ?? true,
        kVerifier: ui.kVerifier ?? false,
        fastLaserDetect: ui.fastLaserDetect ?? false,
        laserRear: ui.laserRear ?? false,
        customFreqs: ui.customFreqs ?? false,
        kaAlwaysPriority: ui.kaAlwaysPriority ?? false,
        kaSensitivity: Number(ui.kaSensitivity ?? 0),
        kSensitivity: Number(ui.kSensitivity ?? 0),
        xSensitivity: Number(ui.xSensitivity ?? 0),
        autoMute: Number(ui.autoMute ?? 0),
        bogeyLockLoud: ui.bogeyLockLoud ?? false,
        muteXKRear: ui.muteXKRear ?? false,
        startupSequence: ui.startupSequence ?? false,
        restingDisplay: ui.restingDisplay ?? false,
        bsmPlus: ui.bsmPlus ?? false,
        mrct: ui.mrct ?? false,
        driveSafe3D: ui.driveSafe3D ?? false,
        driveSafe3DHD: ui.driveSafe3DHD ?? false,
        redflexHalo: ui.redflexHalo ?? false,
        redflexNK7: ui.redflexNK7 ?? false,
        ekin: ui.ekin ?? false,
        photoVerifier: ui.photoVerifier ?? false,
        gatsoRT4: ui.gatsoRT4 ?? false,
        photoIntersectionFilter: ui.photoIntersectionFilter ?? false
    };
}
