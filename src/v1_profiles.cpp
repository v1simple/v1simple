/**
 * V1 Profile Manager Implementation
 */

#include "v1_profiles.h"
#include "storage_manager.h"
#include "v1_settings_json.h"
#include <ArduinoJson.h>
#include <vector>

// Shared CRC32 from settings_backup.cpp (canonical IEEE 802.3 table, check value 0xCBF43926).
extern uint32_t computeCrc32(const uint8_t* data, size_t length);

uint32_t V1ProfileManager::calculateCRC32(const uint8_t* data, size_t length) {
    return computeCrc32(data, length);
}

namespace {

class ProfileStorageGuard {
  public:
    ProfileStorageGuard(const V1ProfileManager&, StorageManager* storage, bool usingSd, uint32_t timeoutMs)
        : mutex_(usingSd && storage ? storage->getSDMutex() : nullptr), acquired_(mutex_ == nullptr) {
        if (mutex_) {
            acquired_ = xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
        }
    }
    ~ProfileStorageGuard() {
        if (acquired_ && mutex_) {
            xSemaphoreGive(mutex_);
        }
    }
    bool acquired() const { return acquired_; }

  private:
    SemaphoreHandle_t mutex_;
    bool acquired_;
};

ProfileOperationResult profileResult(ProfileStorageStatus status, const String& error = "") {
    ProfileOperationResult result;
    result.status = status;
    result.error = error;
    return result;
}

struct ProfileSyncState {
    uint32_t version = 0;
    bool deleted = false;
};

String syncMetaPath(const String& profilePath) {
    return profilePath + ".meta";
}

ProfileSyncState readSyncState(fs::FS& filesystem, const String& profilePath) {
    ProfileSyncState state;
    const String metaPath = syncMetaPath(profilePath);
    if (filesystem.exists(metaPath)) {
        File file = filesystem.open(metaPath, FILE_READ);
        JsonDocument doc;
        if (file && !deserializeJson(doc, file)) {
            state.version = doc["version"] | 0u;
            state.deleted = doc["deleted"] | false;
        }
        if (file) file.close();
    }
    if (state.version == 0 && filesystem.exists(profilePath)) {
        state.version = 1; // backward-compatible baseline for pre-metadata files
        state.deleted = false;
    }
    return state;
}

bool writeSyncState(fs::FS& filesystem, const String& profilePath, const ProfileSyncState& state) {
    JsonDocument doc;
    doc["version"] = state.version;
    doc["deleted"] = state.deleted;
    const String metaPath = syncMetaPath(profilePath);
    const String tmpPath = metaPath + ".tmp";
    File file = filesystem.open(tmpPath, FILE_WRITE);
    if (!file) return false;
    const size_t written = serializeJson(doc, file);
    file.flush();
    file.close();
    if (written == 0 || !StorageManager::promoteTempFileWithRollback(filesystem, tmpPath.c_str(), metaPath.c_str())) {
        filesystem.remove(tmpPath);
        return false;
    }
    return true;
}

bool copyProfileFile(fs::FS& source, fs::FS& target, const String& profilePath) {
    File in = source.open(profilePath, FILE_READ);
    if (!in || in.size() == 0 || in.size() > 4096) {
        if (in) in.close();
        return false;
    }
    const String tmpPath = profilePath + ".tmpsync";
    File out = target.open(tmpPath, FILE_WRITE);
    if (!out) {
        in.close();
        return false;
    }
    bool ok = true;
    uint8_t buffer[256];
    while (in.available()) {
        const size_t count = in.read(buffer, sizeof(buffer));
        if (count == 0 || out.write(buffer, count) != count) {
            ok = false;
            break;
        }
    }
    out.flush();
    out.close();
    in.close();
    if (!ok || !StorageManager::promoteTempFileWithRollback(target, tmpPath.c_str(), profilePath.c_str())) {
        target.remove(tmpPath);
        return false;
    }
    return true;
}

void addUniqueName(std::vector<String>& names, const String& candidate) {
    for (const String& existing : names) {
        if (existing == candidate) return;
    }
    names.push_back(candidate);
}

} // namespace

V1ProfileManager::V1ProfileManager()
    : fs_(nullptr), secondaryFs_(nullptr), storage_(nullptr), usingSd_(false), ready_(false),
      profileDir_("/v1profiles"), currentValid_(false) {}

void V1ProfileManager::bumpCatalogRevision() {
    if (catalogRevisionCounter_ == UINT32_MAX) {
        catalogRevisionCounter_ = 1;
        return;
    }
    catalogRevisionCounter_++;
}

static String basenameFromPath(const String& path) {
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash >= 0) {
        return path.substring(lastSlash + 1);
    }
    return path;
}

void V1ProfileManager::recoverInterruptedSavesUnlocked() {
    // Scan for .tmp and .bak files that indicate interrupted saves
    // .tmp = incomplete new save (delete it)
    // .bak without corresponding .json = interrupted rename (restore it)

    File dir = fs_->open(profileDir_);
    if (!dir || !dir.isDirectory()) {
        return;
    }

    std::vector<String> tmpFiles;
    std::vector<String> bakFiles;
    std::vector<String> jsonFiles;

    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        entry.close();

        if (name.endsWith(".tmp")) {
            tmpFiles.push_back(name);
        } else if (name.endsWith(".bak")) {
            bakFiles.push_back(name);
        } else if (name.endsWith(".json")) {
            jsonFiles.push_back(name);
        }
    }
    dir.close();

    // Remove incomplete .tmp files (interrupted during write)
    for (const String& tmp : tmpFiles) {
        String fullPath = profileDir_ + "/" + tmp;
        Serial.println("[V1Profiles] Removing incomplete temp file");
        fs_->remove(fullPath);
    }

    // Check for orphaned .bak files (main file missing after rename)
    for (const String& bak : bakFiles) {
        // Get the corresponding .json filename
        String jsonName = bak.substring(0, bak.length() - 4); // Remove .bak

        // Check if the main .json file exists
        bool hasJson = false;
        for (const String& json : jsonFiles) {
            if (json == jsonName) {
                hasJson = true;
                break;
            }
        }

        if (!hasJson) {
            // Main file missing! Restore from backup
            String bakPath = profileDir_ + "/" + bak;
            String jsonPath = profileDir_ + "/" + jsonName;
            Serial.println("[V1Profiles] RECOVERY: Main file missing, restoring from backup");
            if (fs_->rename(bakPath, jsonPath)) {
                Serial.println("[V1Profiles] Recovery successful!");
            } else {
                Serial.println("[V1Profiles] Recovery FAILED - backup rename failed");
            }
        }
    }
}

bool V1ProfileManager::begin(StorageManager& storage) {
    storage_ = &storage;
    usingSd_ = storage.isSDCard();
    return begin(storage.getFilesystem(), storage.getLittleFS());
}

bool V1ProfileManager::begin(fs::FS* filesystem, fs::FS* importFilesystem) {
    if (!filesystem) {
        Serial.println("[V1Profiles] No filesystem provided");
        ;
        return false;
    }

    fs_ = filesystem;
    secondaryFs_ = importFilesystem != filesystem ? importFilesystem : nullptr;

    ProfileStorageGuard guard(*this, storage_, usingSd_, 500);
    if (!guard.acquired()) {
        lastError_ = "Profile storage busy during initialization";
        Serial.println("[V1Profiles] BUSY: initialization could not acquire SD mutex");
        return false;
    }

    // Create profiles directory if it doesn't exist
    if (!fs_->exists(profileDir_)) {
        if (!fs_->mkdir(profileDir_)) {
            Serial.println("[V1Profiles] Failed to create profiles directory");
            return false;
        }
        Serial.println("[V1Profiles] Created profiles directory");
    }

    if (importFilesystem && importFilesystem != fs_) {
        size_t migrated = reconcileProfilesFrom(importFilesystem);
        if (migrated > 0) {
            Serial.printf("[V1Profiles] Migrated %u profile(s) from secondary filesystem\n",
                          static_cast<unsigned>(migrated));
        }
    }

    // Run startup integrity check - recover any interrupted saves
    recoverInterruptedSavesUnlocked();

    ready_ = true;
    Serial.println("[V1Profiles] Initialized");
    return true;
}

size_t V1ProfileManager::reconcileProfilesFrom(fs::FS* sourceFs) {
    if (!sourceFs || !fs_ || sourceFs == fs_) {
        return 0;
    }
    if (!sourceFs->exists(profileDir_)) {
        return 0;
    }

    if (!fs_->exists(profileDir_)) fs_->mkdir(profileDir_);

    std::vector<String> names;
    auto collect = [&](fs::FS& filesystem) {
        File dir = filesystem.open(profileDir_);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return;
        }
        File entry;
        while ((entry = dir.openNextFile())) {
            String filename = basenameFromPath(entry.name());
            entry.close();
            String candidate;
            if (filename.endsWith(".json.meta")) {
                candidate = filename.substring(0, filename.length() - 10);
            } else if (filename.endsWith(".json")) {
                candidate = filename.substring(0, filename.length() - 5);
            } else {
                continue;
            }
            String canonical;
            if (canonicalizeProfileName(candidate, canonical) == ProfileNameStatus::Valid && canonical == candidate) {
                addUniqueName(names, canonical);
            }
        }
        dir.close();
    };
    collect(*sourceFs);
    collect(*fs_);

    size_t reconciled = 0;
    for (const String& name : names) {
        const String path = profilePath(name);
        ProfileSyncState sourceState = readSyncState(*sourceFs, path);
        ProfileSyncState targetState = readSyncState(*fs_, path);

        bool sourceWins = sourceState.version > targetState.version;
        if (sourceState.version == targetState.version && sourceState.deleted != targetState.deleted) {
            sourceWins = sourceState.deleted; // equal-generation deletion always wins
        }
        fs::FS* winner = sourceWins ? sourceFs : fs_;
        fs::FS* loser = sourceWins ? fs_ : sourceFs;
        ProfileSyncState winningState = sourceWins ? sourceState : targetState;
        ProfileSyncState losingState = sourceWins ? targetState : sourceState;
        if (winningState.version == 0) continue;

        const bool stateDiffers = winningState.version != losingState.version ||
                                  winningState.deleted != losingState.deleted ||
                                  (!winningState.deleted && !loser->exists(path)) ||
                                  (winningState.deleted && loser->exists(path));
        if (stateDiffers) {
            bool applied = true;
            if (winningState.deleted) {
                if (loser->exists(path)) applied = loser->remove(path);
            } else {
                applied = winner->exists(path) && copyProfileFile(*winner, *loser, path);
            }
            if (!applied || !writeSyncState(*loser, path, winningState)) {
                Serial.printf("[V1Profiles] RECONCILE failed name='%s' path='%s'\n", name.c_str(), path.c_str());
                continue;
            }
            reconciled++;
        }
        if (winningState.deleted && winner->exists(path)) {
            winner->remove(path);
        }
        // Materialize metadata for legacy winners so later edits/deletions have
        // an explicit ordering basis on both filesystems.
        writeSyncState(*winner, path, winningState);
        writeSyncState(*loser, path, winningState);
    }
    return reconciled;
}

String V1ProfileManager::profilePath(const String& name) const {
    return profileDir_ + "/" + name + ".json";
}

ProfileListResult V1ProfileManager::listProfilesUnlocked() const {
    ProfileListResult result;
    if (!ready_ || !fs_) {
        result.status = ProfileStorageStatus::IoError;
        result.error = "Profile filesystem not ready";
        return result;
    }

    File dir = fs_->open(profileDir_);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        result.status = ProfileStorageStatus::IoError;
        result.error = "Failed to enumerate profile directory";
        return result;
    }

    std::vector<String> collisionKeys;
    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        entry.close();
        if (name.endsWith(".json")) {
            // Remove .json extension and path
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0) {
                name = name.substring(lastSlash + 1);
            }
            name = name.substring(0, name.length() - 5); // Remove .json

            // Filter out system files that aren't user profiles
            String canonical;
            if (canonicalizeProfileName(name, canonical) != ProfileNameStatus::Valid || canonical != name) {
                continue;
            }
            if (readSyncState(*fs_, profilePath(canonical)).deleted) {
                continue;
            }
            const String collisionKey = profileCanonicalCollisionKey(canonical);
            bool collision = false;
            for (const String& existingKey : collisionKeys) {
                if (existingKey == collisionKey) {
                    collision = true;
                    break;
                }
            }
            if (collision) {
                dir.close();
                result.status = ProfileStorageStatus::Corrupt;
                result.error = "Canonical profile-name collision in catalog";
                return result;
            }
            collisionKeys.push_back(collisionKey);
            result.profiles.push_back(canonical);
        }
    }
    dir.close();
    result.status = ProfileStorageStatus::Success;
    result.genuinelyEmpty = result.profiles.empty();
    return result;
}

ProfileListResult V1ProfileManager::listProfilesResult(uint32_t timeoutMs) const {
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        ProfileListResult result;
        result.status = ProfileStorageStatus::Busy;
        result.error = "Profile storage busy";
        return result;
    }
    return listProfilesUnlocked();
}

std::vector<String> V1ProfileManager::listProfiles() const {
    return listProfilesResult().profiles;
}

ProfileOperationResult V1ProfileManager::loadProfileUnlocked(const String& name, V1Profile& profile,
                                                              bool allowTransactionRecovery) const {
    if (!ready_ || !fs_) {
        return profileResult(ProfileStorageStatus::IoError, "Profile filesystem not ready");
    }

    String path = profilePath(name);
    String bakPath = path + ".bak";

    // A committed tombstone is authoritative even if stale bytes remain after
    // an interrupted delete. This prevents later enumeration or reconciliation
    // from resurrecting a profile whose deletion was already recorded.
    if (readSyncState(*fs_, path).deleted) {
        Serial.printf("[V1Profiles] NOT_FOUND name='%s' path='%s' tombstoned=true\n", name.c_str(), path.c_str());
        return profileResult(ProfileStorageStatus::NotFound, "Profile not found");
    }

    const bool mainExists = fs_->exists(path);
    File file = mainExists ? fs_->open(path, FILE_READ) : File();
    if (!file) {
        // Try to recover from backup file
        if (allowTransactionRecovery && fs_->exists(bakPath)) {
            Serial.printf("[V1Profiles] RECOVERY name='%s' path='%s' restoring transaction backup\n", name.c_str(),
                          path.c_str());
            // Rename backup to main file
            if (fs_->rename(bakPath, path)) {
                Serial.printf("[V1Profiles] RECOVERY name='%s' path='%s' restored\n", name.c_str(), path.c_str());
                file = fs_->open(path, FILE_READ);
            }
        }

        if (!file) {
            const ProfileStorageStatus status = mainExists ? ProfileStorageStatus::IoError : ProfileStorageStatus::NotFound;
            lastError_ = status == ProfileStorageStatus::NotFound ? "Profile not found" : "Profile open failed";
            Serial.printf("[V1Profiles] %s name='%s' path='%s'\n",
                          status == ProfileStorageStatus::NotFound ? "NOT_FOUND" : "IO_ERROR", name.c_str(),
                          path.c_str());
            return profileResult(status, lastError_);
        }
    }

    // Hard cap JSON size to avoid excessive allocation on small devices
    if (file.size() > 4096) {
        Serial.printf("[V1Profiles] Profile too large (%u bytes), aborting\n", (unsigned)file.size());
        file.close();
        return profileResult(ProfileStorageStatus::Corrupt, "Profile file exceeds size limit");
    }

    // Read file content for CRC validation with RAII-managed storage
    // so all early returns remain leak-safe.
    const size_t fileSize = file.size();
    std::vector<uint8_t> fileContent(fileSize);
    if (fileSize > 0) {
        const size_t bytesRead = file.read(fileContent.data(), fileSize);
        if (bytesRead != fileSize) {
            lastError_ = "Failed to read complete profile file";
            Serial.printf("[V1Profiles] %s (%u/%u bytes)\n", lastError_.c_str(), static_cast<unsigned>(bytesRead),
                          static_cast<unsigned>(fileSize));
            file.close();
            return profileResult(ProfileStorageStatus::IoError, lastError_);
        }
    }
    file.close();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, fileContent.data(), fileSize);

    if (err) {
        lastError_ = String("JSON parse error: ") + err.c_str();
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return profileResult(ProfileStorageStatus::Corrupt, lastError_);
    }

    const JsonVariantConst rawBytes = doc["bytes"];
    const bool hasRawBytes = !rawBytes.isUnbound();
    uint8_t rawSettingsBytes[V1SettingsJson::kSettingsByteCount];
    if (hasRawBytes) {
        if (!V1SettingsJson::parseRawBytes(rawBytes, rawSettingsBytes)) {
            lastError_ = "Invalid settings bytes";
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            return profileResult(ProfileStorageStatus::Corrupt, lastError_);
        }
    }

    // Validate CRC32 if present
    if (doc["crc32"].is<uint32_t>()) {
        uint32_t storedCrc = doc["crc32"].as<uint32_t>();

        // Calculate CRC of the 6 settings bytes
        if (hasRawBytes) {
            uint32_t computedCrc = calculateCRC32(rawSettingsBytes, V1SettingsJson::kSettingsByteCount);
            if (storedCrc != computedCrc) {
                lastError_ = "CRC mismatch - profile file corrupted";
                Serial.printf("[V1Profiles] %s (stored: %08lX, computed: %08lX)\n", lastError_.c_str(),
                              static_cast<unsigned long>(storedCrc), static_cast<unsigned long>(computedCrc));
                return profileResult(ProfileStorageStatus::Corrupt, lastError_);
            }
            Serial.println("[V1Profiles] CRC32 validated OK");
        }
    }

    profile.name = name;
    profile.description = doc["description"] | "";
    profile.displayOn = doc["displayOn"] | true;     // Default to on
    profile.mainVolume = doc["mainVolume"] | 0xFF;   // 0xFF = don't change
    profile.mutedVolume = doc["mutedVolume"] | 0xFF; // 0xFF = don't change

    // Parse settings bytes
    if (hasRawBytes) {
        for (size_t i = 0; i < V1SettingsJson::kSettingsByteCount; i++) {
            profile.settings.bytes[i] = rawSettingsBytes[i];
        }
    } else {
        // Try individual settings (legacy or human-readable format)
        V1UserSettings& s = profile.settings;
        s.setDefaults();

        if (!doc["xBand"].isNull())
            s.setXBandEnabled(doc["xBand"]);
        if (!doc["kBand"].isNull())
            s.setKBandEnabled(doc["kBand"]);
        if (!doc["kaBand"].isNull())
            s.setKaBandEnabled(doc["kaBand"]);
        if (!doc["laser"].isNull())
            s.setLaserEnabled(doc["laser"]);
        if (!doc["kuBand"].isNull())
            s.setKuBandEnabled(doc["kuBand"]);
        if (!doc["euro"].isNull())
            s.setEuroMode(doc["euro"]);
        if (!doc["kVerifier"].isNull())
            s.setKVerifier(doc["kVerifier"]);
        if (!doc["laserRear"].isNull())
            s.setLaserRear(doc["laserRear"]);
        if (!doc["customFreqs"].isNull())
            s.setCustomFreqs(doc["customFreqs"]);
        if (!doc["kaAlwaysPriority"].isNull())
            s.setKaAlwaysPriority(doc["kaAlwaysPriority"]);
        if (!doc["fastLaserDetect"].isNull())
            s.setFastLaserDetect(doc["fastLaserDetect"]);
        if (!doc["kaSensitivity"].isNull())
            s.setKaSensitivity(doc["kaSensitivity"]);
        if (!doc["kSensitivity"].isNull())
            s.setKSensitivity(doc["kSensitivity"]);
        if (!doc["xSensitivity"].isNull())
            s.setXSensitivity(doc["xSensitivity"]);
        if (!doc["autoMute"].isNull())
            s.setAutoMute(doc["autoMute"]);
        if (!doc["muteToMuteVolume"].isNull())
            s.setMuteToMuteVolume(doc["muteToMuteVolume"]);
        if (!doc["bogeyLockLoud"].isNull())
            s.setBogeyLockLoud(doc["bogeyLockLoud"]);
        if (!doc["muteXKRear"].isNull())
            s.setMuteXKRear(doc["muteXKRear"]);
        if (!doc["startupSequence"].isNull())
            s.setStartupSequence(doc["startupSequence"]);
        if (!doc["restingDisplay"].isNull())
            s.setRestingDisplay(doc["restingDisplay"]);
        if (!doc["bsmPlus"].isNull())
            s.setBsmPlus(doc["bsmPlus"]);
        if (!doc["mrct"].isNull())
            s.setMrct(doc["mrct"]);
    }

    Serial.printf("[V1Profiles] LOAD success name='%s' path='%s'\n", name.c_str(), path.c_str());
    return profileResult(ProfileStorageStatus::Success);
}

ProfileOperationResult V1ProfileManager::loadProfileResult(const String& rawName, V1Profile& profile,
                                                           uint32_t timeoutMs) const {
    String canonical;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(rawName, canonical);
    if (nameStatus != ProfileNameStatus::Valid) {
        return profileResult(ProfileStorageStatus::InvalidName, profileNameStatusMessage(nameStatus));
    }
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        lastError_ = "Profile storage busy";
        Serial.printf("[V1Profiles] BUSY name='%s' path='%s'\n", canonical.c_str(), profilePath(canonical).c_str());
        return profileResult(ProfileStorageStatus::Busy, lastError_);
    }
    return loadProfileUnlocked(canonical, profile);
}

bool V1ProfileManager::loadProfile(const String& name, V1Profile& profile) const {
    return loadProfileResult(name, profile).success();
}

ProfileSaveResult V1ProfileManager::saveProfileUnlocked(const V1Profile& profile, const String& canonicalName) {
    if (!ready_ || !fs_) {
        lastError_ = "Filesystem not ready";
        Serial.printf("[V1Profiles] Save failed: %s\n", lastError_.c_str());
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    const ProfileListResult catalog = listProfilesUnlocked();
    if (!catalog.success()) {
        return ProfileSaveResult(catalog.status, catalog.error);
    }
    const String newKey = profileCanonicalCollisionKey(canonicalName);
    for (const String& existing : catalog.profiles) {
        if (existing != canonicalName && profileCanonicalCollisionKey(existing) == newKey) {
            lastError_ = "Profile name collides with existing canonical name";
            Serial.printf("[V1Profiles] INVALID_NAME name='%s' path='%s' reason=collision\n", canonicalName.c_str(),
                          profilePath(canonicalName).c_str());
            return ProfileSaveResult(ProfileStorageStatus::InvalidName, lastError_);
        }
    }

    String path = profilePath(canonicalName);
    String tmpPath = path + ".tmp";
    String bakPath = path + ".bak";
    const ProfileSyncState activeState = readSyncState(*fs_, path);
    const ProfileSyncState secondaryState = secondaryFs_ ? readSyncState(*secondaryFs_, path) : ProfileSyncState{};
    ProfileSyncState committedState;
    committedState.version = std::max(activeState.version, secondaryState.version) + 1u;
    committedState.deleted = false;

    // Step 1: Write to temporary file (don't truncate original yet)
    File file = fs_->open(tmpPath, FILE_WRITE);
    if (!file) {
        lastError_ = "Failed to create temp file";
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    JsonDocument doc;
    const V1UserSettings& s = profile.settings;

    // Store metadata
    doc["name"] = canonicalName;
    doc["description"] = profile.description;
    doc["displayOn"] = profile.displayOn;
    doc["mainVolume"] = profile.mainVolume;
    doc["mutedVolume"] = profile.mutedVolume;

    // Store raw bytes for exact restoration
    JsonArray bytes = doc["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }

    // Also store human-readable settings
    doc["xBand"] = s.xBandEnabled();
    doc["kBand"] = s.kBandEnabled();
    doc["kaBand"] = s.kaBandEnabled();
    doc["laser"] = s.laserEnabled();
    doc["kuBand"] = s.kuBandEnabled();
    doc["euro"] = s.euroMode();
    doc["kVerifier"] = s.kVerifier();
    doc["laserRear"] = s.laserRear();
    doc["customFreqs"] = s.customFreqs();
    doc["kaAlwaysPriority"] = s.kaAlwaysPriority();
    doc["fastLaserDetect"] = s.fastLaserDetect();
    doc["kaSensitivity"] = s.kaSensitivity();
    doc["kSensitivity"] = s.kSensitivity();
    doc["xSensitivity"] = s.xSensitivity();
    doc["autoMute"] = s.autoMute();
    doc["muteToMuteVolume"] = s.muteToMuteVolume();
    doc["bogeyLockLoud"] = s.bogeyLockLoud();
    doc["muteXKRear"] = s.muteXKRear();
    doc["startupSequence"] = s.startupSequence();
    doc["restingDisplay"] = s.restingDisplay();
    doc["bsmPlus"] = s.bsmPlus();
    doc["mrct"] = s.mrct();
    doc["driveSafe3D"] = s.driveSafe3D();
    doc["driveSafe3DHD"] = s.driveSafe3DHD();
    doc["redflexHalo"] = s.redflexHalo();
    doc["redflexNK7"] = s.redflexNK7();
    doc["ekin"] = s.ekin();
    doc["photoVerifier"] = s.photoVerifier();
    doc["gatsoRT4"] = s.gatsoRT4();
    doc["photoIntersectionFilter"] = s.photoIntersectionFilter();

    // Calculate and store CRC32 of the settings bytes for integrity checking
    uint32_t crc = calculateCRC32(s.bytes, 6);
    doc["crc32"] = crc;

    const size_t expectedPrettyBytes = measureJsonPretty(doc);
    size_t written = serializeJsonPretty(doc, file);

    // Step 2: Flush to ensure data is written to SD before closing
    file.flush();
    file.close();

    // Step 3: Verify write succeeded and file size matches
    if (written == 0) {
        lastError_ = "Serialization failed - no data written";
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        fs_->remove(tmpPath);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    if (written != expectedPrettyBytes) {
        lastError_ = "Partial write detected: expected " + String(static_cast<unsigned long>(expectedPrettyBytes)) +
                     " bytes, wrote " + String(static_cast<unsigned long>(written));
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        fs_->remove(tmpPath);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }
    {
        File verify = fs_->open(tmpPath, FILE_READ);
        if (!verify) {
            lastError_ = "Failed to re-open temp file for verification";
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            fs_->remove(tmpPath);
            return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
        }
        size_t fileSize = verify.size();
        verify.close();
        if (fileSize != written) {
            lastError_ = "Partial write detected: expected " + String(written) + " bytes, got " + String(fileSize);
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            fs_->remove(tmpPath);
            return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
        }
    }

    // Step 4: Create backup of existing file before replacement
    if (fs_->exists(path)) {
        // Remove old backup if exists
        if (fs_->exists(bakPath)) {
            fs_->remove(bakPath);
        }
        // Rename current to backup (for rollback capability)
        if (!fs_->rename(path, bakPath)) {
            Serial.println("[V1Profiles] Warning: Could not create backup");
            // Continue anyway - this is not fatal
        } else {
            Serial.println("[V1Profiles] Created backup");
        }
    }

    // Step 5: Rename temp to final
    if (!fs_->rename(tmpPath, path)) {
        lastError_ = "Failed to rename temp to final";
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());

        // Try to restore from backup
        if (fs_->exists(bakPath)) {
            if (fs_->rename(bakPath, path)) {
                Serial.println("[V1Profiles] Restored from backup after failed save");
            }
        }
        fs_->remove(tmpPath);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    // Step 6: prove the promoted final file is readable and its CRC is valid.
    V1Profile verified;
    // Do not let ordinary interrupted-transaction recovery consume the backup
    // while validating a just-promoted candidate. The save transaction owns
    // rollback until final-file verification completes.
    const ProfileOperationResult verifyResult = loadProfileUnlocked(canonicalName, verified, false);
    if (!verifyResult.success() || memcmp(verified.settings.bytes, profile.settings.bytes, 6) != 0) {
        lastError_ = verifyResult.success() ? "Final profile verification mismatch" : verifyResult.error;
        fs_->remove(path);
        if (fs_->exists(bakPath)) {
            fs_->rename(bakPath, path);
        }
        Serial.printf("[V1Profiles] VERIFY_FAILED name='%s' path='%s' reason='%s'\n", canonicalName.c_str(),
                      path.c_str(), lastError_.c_str());
        return ProfileSaveResult(verifyResult.success() ? ProfileStorageStatus::Corrupt : verifyResult.status,
                                 lastError_);
    }

    if (!writeSyncState(*fs_, path, committedState)) {
        lastError_ = "Failed to persist profile reconciliation metadata";
        fs_->remove(path);
        if (fs_->exists(bakPath)) fs_->rename(bakPath, path);
        return ProfileSaveResult(ProfileStorageStatus::IoError, lastError_);
    }

    if (fs_->exists(bakPath)) {
        fs_->remove(bakPath);
    }

    if (secondaryFs_) {
        if (!secondaryFs_->exists(profileDir_)) secondaryFs_->mkdir(profileDir_);
        if (!copyProfileFile(*fs_, *secondaryFs_, path) || !writeSyncState(*secondaryFs_, path, committedState)) {
            Serial.printf("[V1Profiles] WARN: secondary mirror deferred name='%s' path='%s'\n", canonicalName.c_str(),
                          path.c_str());
        }
    }

    Serial.printf("[V1Profiles] SAVE success name='%s' path='%s' bytes=%u crc=%08lX\n", canonicalName.c_str(),
                  path.c_str(), written, static_cast<unsigned long>(crc));
    bumpCatalogRevision();
    return ProfileSaveResult(ProfileStorageStatus::Success);
}

ProfileSaveResult V1ProfileManager::saveProfile(const V1Profile& profile) {
    String canonical;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(profile.name, canonical);
    if (nameStatus != ProfileNameStatus::Valid) {
        lastError_ = profileNameStatusMessage(nameStatus);
        Serial.printf("[V1Profiles] INVALID_NAME requested='%s' reason='%s'\n", profile.name.c_str(), lastError_.c_str());
        return ProfileSaveResult(ProfileStorageStatus::InvalidName, lastError_);
    }
    ProfileStorageGuard guard(*this, storage_, usingSd_, 250);
    if (!guard.acquired()) {
        lastError_ = "Profile storage busy";
        Serial.printf("[V1Profiles] BUSY name='%s' path='%s'\n", canonical.c_str(), profilePath(canonical).c_str());
        return ProfileSaveResult(ProfileStorageStatus::Busy, lastError_);
    }
    V1Profile canonicalProfile = profile;
    canonicalProfile.name = canonical;
    return saveProfileUnlocked(canonicalProfile, canonical);
}

ProfileOperationResult V1ProfileManager::deleteProfileUnlocked(const String& name) {
    if (!ready_ || !fs_) {
        return profileResult(ProfileStorageStatus::IoError, "Profile filesystem not ready");
    }

    String path = profilePath(name);
    String bakPath = path + ".bak";
    const ProfileSyncState activeState = readSyncState(*fs_, path);
    const ProfileSyncState secondaryState = secondaryFs_ ? readSyncState(*secondaryFs_, path) : ProfileSyncState{};
    ProfileSyncState tombstone;
    tombstone.version = std::max(activeState.version, secondaryState.version) + 1u;
    tombstone.deleted = true;
    const bool activeExists = fs_->exists(path);
    const bool activeBakExists = fs_->exists(bakPath);
    const bool secondaryExists = secondaryFs_ && secondaryFs_->exists(path);
    const String secondaryBak = path + ".bak";
    const bool secondaryBakExists = secondaryFs_ && secondaryFs_->exists(secondaryBak);
    const bool removedAny = activeExists || activeBakExists || secondaryExists || secondaryBakExists;

    if (!removedAny) {
        return profileResult(ProfileStorageStatus::NotFound, "Profile not found");
    }

    // Persist deletion intent before removing data. If power is lost after this
    // point, list/load treat any leftover bytes as deleted and reconciliation
    // propagates the higher-generation tombstone.
    if (!writeSyncState(*fs_, path, tombstone) ||
        (secondaryFs_ && !writeSyncState(*secondaryFs_, path, tombstone))) {
        return profileResult(ProfileStorageStatus::IoError, "Failed to persist profile deletion metadata");
    }

    bool ok = true;

    if (activeExists) {
        if (!fs_->remove(path)) {
            ok = false;
        }
    }

    if (activeBakExists) {
        if (!fs_->remove(bakPath)) {
            ok = false;
        }
    }

    if (secondaryFs_) {
        if (secondaryExists) {
            ok = secondaryFs_->remove(path) && ok;
        }
        if (secondaryBakExists) ok = secondaryFs_->remove(secondaryBak) && ok;
    }

    if (ok && removedAny) {
        Serial.printf("[V1Profiles] DELETE success name='%s' path='%s'\n", name.c_str(), path.c_str());
        bumpCatalogRevision();
    }
    if (!ok) {
        return profileResult(ProfileStorageStatus::IoError, "Failed to remove profile file");
    }
    return profileResult(ProfileStorageStatus::Success);
}

ProfileOperationResult V1ProfileManager::deleteProfileResult(const String& rawName, uint32_t timeoutMs) {
    String canonical;
    const ProfileNameStatus nameStatus = canonicalizeProfileName(rawName, canonical);
    if (nameStatus != ProfileNameStatus::Valid) {
        return profileResult(ProfileStorageStatus::InvalidName, profileNameStatusMessage(nameStatus));
    }
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        Serial.printf("[V1Profiles] BUSY name='%s' path='%s'\n", canonical.c_str(), profilePath(canonical).c_str());
        return profileResult(ProfileStorageStatus::Busy, "Profile storage busy");
    }
    return deleteProfileUnlocked(canonical);
}

bool V1ProfileManager::deleteProfile(const String& name) {
    return deleteProfileResult(name).success();
}

bool V1ProfileManager::renameProfile(const String& oldName, const String& newName) {
    if (!ready_ || !fs_) {
        lastError_ = "Filesystem not ready";
        return false;
    }

    String canonicalOld;
    String canonicalNew;
    const ProfileNameStatus oldStatus = canonicalizeProfileName(oldName, canonicalOld);
    const ProfileNameStatus newStatus = canonicalizeProfileName(newName, canonicalNew);
    if (oldStatus != ProfileNameStatus::Valid || newStatus != ProfileNameStatus::Valid) {
        lastError_ = profileNameStatusMessage(oldStatus != ProfileNameStatus::Valid ? oldStatus : newStatus);
        return false;
    }

    ProfileStorageGuard guard(*this, storage_, usingSd_, 250);
    if (!guard.acquired()) {
        lastError_ = "Profile storage busy";
        return false;
    }

    const String oldPath = profilePath(canonicalOld);
    const String newPath = profilePath(canonicalNew);

    // Guard: exact no-op rename should not touch disk or revision state.
    if (canonicalOld == canonicalNew) {
        return true;
    }

    V1Profile profile;
    if (!loadProfileUnlocked(canonicalOld, profile).success()) {
        return false;
    }

    // Guard: refuse to overwrite a different existing profile.
    if (fs_->exists(newPath)) {
        lastError_ = "Rename target already exists";
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return false;
    }

    profile.name = canonicalNew;
    ProfileSaveResult result = saveProfileUnlocked(profile, canonicalNew);
    if (!result.success) {
        return false;
    }

    if (!deleteProfileUnlocked(canonicalOld).success()) {
        Serial.println("[V1Profiles] Warning: rename saved new but failed to delete old");
        return false;
    }
    return true;
}

ProfileOperationResult V1ProfileManager::snapshotProfiles(std::vector<V1Profile>& profiles, uint32_t timeoutMs) const {
    profiles.clear();
    ProfileStorageGuard guard(*this, storage_, usingSd_, timeoutMs);
    if (!guard.acquired()) {
        return profileResult(ProfileStorageStatus::Busy, "Profile storage busy");
    }
    const ProfileListResult catalog = listProfilesUnlocked();
    if (!catalog.success()) {
        return profileResult(catalog.status, catalog.error);
    }
    profiles.reserve(catalog.profiles.size());
    for (const String& name : catalog.profiles) {
        V1Profile profile;
        const ProfileOperationResult loaded = loadProfileUnlocked(name, profile);
        if (!loaded.success()) {
            profiles.clear();
            return loaded;
        }
        profiles.push_back(profile);
    }
    return profileResult(ProfileStorageStatus::Success);
}

void V1ProfileManager::setCurrentSettings(const uint8_t* bytes) {
    memcpy(currentSettings_.bytes, bytes, 6);
    currentValid_ = true;
}

String V1ProfileManager::settingsToJson(const V1UserSettings& s) const {
    JsonDocument doc;

    // Raw bytes
    JsonArray bytes = doc["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }

    // Human-readable
    doc["xBand"] = s.xBandEnabled();
    doc["kBand"] = s.kBandEnabled();
    doc["kaBand"] = s.kaBandEnabled();
    doc["laser"] = s.laserEnabled();
    doc["kuBand"] = s.kuBandEnabled();
    doc["euro"] = s.euroMode();
    doc["kVerifier"] = s.kVerifier();
    doc["laserRear"] = s.laserRear();
    doc["customFreqs"] = s.customFreqs();
    doc["kaAlwaysPriority"] = s.kaAlwaysPriority();
    doc["fastLaserDetect"] = s.fastLaserDetect();
    doc["kaSensitivity"] = s.kaSensitivity();
    doc["kSensitivity"] = s.kSensitivity();
    doc["xSensitivity"] = s.xSensitivity();
    doc["autoMute"] = s.autoMute();
    doc["muteToMuteVolume"] = s.muteToMuteVolume();
    doc["bogeyLockLoud"] = s.bogeyLockLoud();
    doc["muteXKRear"] = s.muteXKRear();
    doc["startupSequence"] = s.startupSequence();
    doc["restingDisplay"] = s.restingDisplay();
    doc["bsmPlus"] = s.bsmPlus();
    doc["mrct"] = s.mrct();
    doc["driveSafe3D"] = s.driveSafe3D();
    doc["driveSafe3DHD"] = s.driveSafe3DHD();
    doc["redflexHalo"] = s.redflexHalo();
    doc["redflexNK7"] = s.redflexNK7();
    doc["ekin"] = s.ekin();
    doc["photoVerifier"] = s.photoVerifier();
    doc["gatsoRT4"] = s.gatsoRT4();
    doc["photoIntersectionFilter"] = s.photoIntersectionFilter();

    String output;
    serializeJson(doc, output);
    return output;
}

String V1ProfileManager::profileToJson(const V1Profile& profile) const {
    JsonDocument doc;
    doc["name"] = profile.name;
    doc["description"] = profile.description;
    doc["displayOn"] = profile.displayOn;
    doc["mainVolume"] = profile.mainVolume;
    doc["mutedVolume"] = profile.mutedVolume;

    JsonObject settings = doc["settings"].to<JsonObject>();
    const V1UserSettings& s = profile.settings;

    JsonArray bytes = settings["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }

    settings["xBand"] = s.xBandEnabled();
    settings["kBand"] = s.kBandEnabled();
    settings["kaBand"] = s.kaBandEnabled();
    settings["laser"] = s.laserEnabled();
    settings["kuBand"] = s.kuBandEnabled();
    settings["euro"] = s.euroMode();
    settings["kVerifier"] = s.kVerifier();
    settings["laserRear"] = s.laserRear();
    settings["customFreqs"] = s.customFreqs();
    settings["kaAlwaysPriority"] = s.kaAlwaysPriority();
    settings["fastLaserDetect"] = s.fastLaserDetect();
    settings["kaSensitivity"] = s.kaSensitivity();
    settings["kSensitivity"] = s.kSensitivity();
    settings["xSensitivity"] = s.xSensitivity();
    settings["autoMute"] = s.autoMute();
    settings["muteToMuteVolume"] = s.muteToMuteVolume();
    settings["bogeyLockLoud"] = s.bogeyLockLoud();
    settings["muteXKRear"] = s.muteXKRear();
    settings["startupSequence"] = s.startupSequence();
    settings["restingDisplay"] = s.restingDisplay();
    settings["bsmPlus"] = s.bsmPlus();
    settings["mrct"] = s.mrct();
    settings["driveSafe3D"] = s.driveSafe3D();
    settings["driveSafe3DHD"] = s.driveSafe3DHD();
    settings["redflexHalo"] = s.redflexHalo();
    settings["redflexNK7"] = s.redflexNK7();
    settings["ekin"] = s.ekin();
    settings["photoVerifier"] = s.photoVerifier();
    settings["gatsoRT4"] = s.gatsoRT4();
    settings["photoIntersectionFilter"] = s.photoIntersectionFilter();

    String output;
    serializeJson(doc, output);
    return output;
}

bool V1ProfileManager::jsonToSettings(const String& json, V1UserSettings& settings) const {
    if (json.length() > 4096) {
        Serial.println("[V1Profiles] JSON too large, rejecting");
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[V1Profiles] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // Check if settings are nested inside a "settings" object
    JsonObject settingsObj = doc["settings"].as<JsonObject>();
    if (settingsObj.isNull()) {
        // Settings are at root level
        settingsObj = doc.as<JsonObject>();
    }

    return jsonToSettings(settingsObj, settings);
}

bool V1ProfileManager::jsonToSettings(const JsonObject& settingsObj, V1UserSettings& settings) const {
    // Try raw bytes first. A present raw field must be a strict six-byte array;
    // only an absent field falls back to individual settings.
    const JsonVariantConst rawBytes = settingsObj["bytes"];
    if (!rawBytes.isUnbound()) {
        if (!V1SettingsJson::parseRawBytes(rawBytes, settings.bytes)) {
            Serial.println("[V1Profiles] Invalid raw settings bytes");
            return false;
        }
        Serial.println("[V1Profiles] Loaded from raw bytes");
        return true;
    }

    // Parse individual settings
    settings.setDefaults();
    Serial.println("[V1Profiles] Parsing individual settings");
    bool anyField = false;

    if (!settingsObj["xBand"].isNull()) {
        settings.setXBandEnabled(settingsObj["xBand"]);
        anyField = true;
    }
    if (!settingsObj["kBand"].isNull()) {
        settings.setKBandEnabled(settingsObj["kBand"]);
        anyField = true;
    }
    if (!settingsObj["kaBand"].isNull()) {
        settings.setKaBandEnabled(settingsObj["kaBand"]);
        anyField = true;
    }
    if (!settingsObj["laser"].isNull()) {
        settings.setLaserEnabled(settingsObj["laser"]);
        anyField = true;
    }
    if (!settingsObj["kuBand"].isNull()) {
        settings.setKuBandEnabled(settingsObj["kuBand"]);
        anyField = true;
    }
    if (!settingsObj["euro"].isNull()) {
        settings.setEuroMode(settingsObj["euro"]);
        anyField = true;
    }
    if (!settingsObj["kVerifier"].isNull()) {
        settings.setKVerifier(settingsObj["kVerifier"]);
        anyField = true;
    }
    if (!settingsObj["laserRear"].isNull()) {
        settings.setLaserRear(settingsObj["laserRear"]);
        anyField = true;
    }
    if (!settingsObj["customFreqs"].isNull()) {
        settings.setCustomFreqs(settingsObj["customFreqs"]);
        anyField = true;
    }
    if (!settingsObj["kaAlwaysPriority"].isNull()) {
        settings.setKaAlwaysPriority(settingsObj["kaAlwaysPriority"]);
        anyField = true;
    }
    if (!settingsObj["fastLaserDetect"].isNull()) {
        settings.setFastLaserDetect(settingsObj["fastLaserDetect"]);
        anyField = true;
    }
    if (!settingsObj["kaSensitivity"].isNull()) {
        settings.setKaSensitivity(settingsObj["kaSensitivity"]);
        anyField = true;
    }
    if (!settingsObj["kSensitivity"].isNull()) {
        settings.setKSensitivity(settingsObj["kSensitivity"]);
        anyField = true;
    }
    if (!settingsObj["xSensitivity"].isNull()) {
        settings.setXSensitivity(settingsObj["xSensitivity"]);
        anyField = true;
    }
    if (!settingsObj["autoMute"].isNull()) {
        settings.setAutoMute(settingsObj["autoMute"]);
        anyField = true;
    }
    if (!settingsObj["muteToMuteVolume"].isNull()) {
        settings.setMuteToMuteVolume(settingsObj["muteToMuteVolume"]);
        anyField = true;
    }
    if (!settingsObj["bogeyLockLoud"].isNull()) {
        settings.setBogeyLockLoud(settingsObj["bogeyLockLoud"]);
        anyField = true;
    }
    if (!settingsObj["muteXKRear"].isNull()) {
        settings.setMuteXKRear(settingsObj["muteXKRear"]);
        anyField = true;
    }
    if (!settingsObj["startupSequence"].isNull()) {
        settings.setStartupSequence(settingsObj["startupSequence"]);
        anyField = true;
    }
    if (!settingsObj["restingDisplay"].isNull()) {
        settings.setRestingDisplay(settingsObj["restingDisplay"]);
        anyField = true;
    }
    if (!settingsObj["bsmPlus"].isNull()) {
        settings.setBsmPlus(settingsObj["bsmPlus"]);
        anyField = true;
    }
    if (!settingsObj["mrct"].isNull()) {
        settings.setMrct(settingsObj["mrct"]);
        anyField = true;
    }
    if (!settingsObj["driveSafe3D"].isNull()) {
        settings.setDriveSafe3D(settingsObj["driveSafe3D"]);
        anyField = true;
    }
    if (!settingsObj["driveSafe3DHD"].isNull()) {
        settings.setDriveSafe3DHD(settingsObj["driveSafe3DHD"]);
        anyField = true;
    }
    if (!settingsObj["redflexHalo"].isNull()) {
        settings.setRedflexHalo(settingsObj["redflexHalo"]);
        anyField = true;
    }
    if (!settingsObj["redflexNK7"].isNull()) {
        settings.setRedflexNK7(settingsObj["redflexNK7"]);
        anyField = true;
    }
    if (!settingsObj["ekin"].isNull()) {
        settings.setEkin(settingsObj["ekin"]);
        anyField = true;
    }
    if (!settingsObj["photoVerifier"].isNull()) {
        settings.setPhotoVerifier(settingsObj["photoVerifier"]);
        anyField = true;
    }
    if (!settingsObj["gatsoRT4"].isNull()) {
        settings.setGatsoRT4(settingsObj["gatsoRT4"]);
        anyField = true;
    }
    if (!settingsObj["photoIntersectionFilter"].isNull()) {
        settings.setPhotoIntersectionFilter(settingsObj["photoIntersectionFilter"]);
        anyField = true;
    }

    if (!anyField) {
        Serial.println("[V1Profiles] No settings provided");
        return false;
    }

    Serial.printf("[V1Profiles] After parse - byte0=%02X byte2=%02X\n", settings.bytes[0], settings.bytes[2]);
    Serial.printf("[V1Profiles]   xBand=%d, restingDisplay=%d, bsmPlus=%d\n", settings.xBandEnabled(),
                  settings.restingDisplay(), settings.bsmPlus());

    return true;
}
