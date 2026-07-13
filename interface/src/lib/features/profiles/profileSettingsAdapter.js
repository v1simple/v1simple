const DEFAULT_PROFILE_SETTINGS = Object.freeze({
	// V1UserSettings factory default is all six bytes set to 0xff. Keep the UI
	// defaults aligned with the firmware's byte-level default so an offline
	// profile starts from a valid V1 configuration instead of zero-valued fields.
	x: true,
	k: true,
	ka: true,
	laser: true,
	muteToMuteVolume: false,
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
	photoVerifier: false
});

export function createDefaultProfileSettings() {
	return { ...DEFAULT_PROFILE_SETTINGS };
}

export function fromApiSettings(api = {}) {
	return {
		ka: api.kaBand ?? api.ka ?? false,
		k: api.kBand ?? api.k ?? false,
		x: api.xBand ?? api.x ?? false,
		ku: api.kuBand ?? api.ku ?? false,
		laser: api.laser ?? false,
		euroMode: api.euro ?? api.euroMode ?? false,
		muteToMuteVolume: api.muteToMuteVolume ?? false,
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
		photoVerifier: api.photoVerifier ?? false
	};
}

export function toApiSettings(ui = {}) {
	return {
		xBand: ui.x ?? ui.xBand ?? false,
		kBand: ui.k ?? ui.kBand ?? false,
		kaBand: ui.ka ?? ui.kaBand ?? false,
		laser: ui.laser ?? false,
		kuBand: ui.ku ?? ui.kuBand ?? false,
		euro: ui.euroMode ?? ui.euro ?? false,
		muteToMuteVolume: ui.muteToMuteVolume ?? false,
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
		photoVerifier: ui.photoVerifier ?? false
	};
}
