/**
 * V1 Profile Manager Implementation
 */

#include "v1_profiles.h"
#include <ArduinoJson.h>
#include <vector>

// Global instance
V1ProfileManager v1ProfileManager;

// Shared CRC32 from settings_backup.cpp (canonical IEEE 802.3 table, check value 0xCBF43926).
extern uint32_t computeCrc32(const uint8_t* data, size_t length);

uint32_t V1ProfileManager::calculateCRC32(const uint8_t* data, size_t length) {
    return computeCrc32(data, length);
}

V1ProfileManager::V1ProfileManager()
    : fs_(nullptr)
    , ready_(false)
    , profileDir_("/v1profiles")
    , currentValid_(false) {
}

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

void V1ProfileManager::recoverInterruptedSaves() {
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
        Serial.printf("[V1Profiles] Removing incomplete temp file: %s\n", fullPath.c_str());
        fs_->remove(fullPath);
    }

    // Check for orphaned .bak files (main file missing after rename)
    for (const String& bak : bakFiles) {
        // Get the corresponding .json filename
        String jsonName = bak.substring(0, bak.length() - 4);  // Remove .bak

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
            Serial.printf("[V1Profiles] RECOVERY: Main file missing, restoring from backup: %s -> %s\n",
                bakPath.c_str(), jsonPath.c_str());
            if (fs_->rename(bakPath, jsonPath)) {
                Serial.println("[V1Profiles] Recovery successful!");
            } else {
                Serial.println("[V1Profiles] Recovery FAILED - backup rename failed");
            }
        }
    }
}

bool V1ProfileManager::begin(fs::FS* filesystem, fs::FS* importFilesystem) {
    if (!filesystem) {
        Serial.println("[V1Profiles] No filesystem provided");;
        return false;
    }

    fs_ = filesystem;

    // Create profiles directory if it doesn't exist
    if (!fs_->exists(profileDir_)) {
        if (!fs_->mkdir(profileDir_)) {
            Serial.println("[V1Profiles] Failed to create profiles directory");
            return false;
        }
        Serial.println("[V1Profiles] Created profiles directory");
    }

    if (importFilesystem && importFilesystem != fs_) {
        size_t migrated = migrateProfilesFrom(importFilesystem);
        if (migrated > 0) {
            Serial.printf("[V1Profiles] Migrated %u profile(s) from secondary filesystem\n",
                          static_cast<unsigned>(migrated));
        }
    }

    // Run startup integrity check - recover any interrupted saves
    recoverInterruptedSaves();

    ready_ = true;
    Serial.println("[V1Profiles] Initialized");
    return true;
}

size_t V1ProfileManager::migrateProfilesFrom(fs::FS* sourceFs) {
    if (!sourceFs || !fs_ || sourceFs == fs_) {
        return 0;
    }
    if (!sourceFs->exists(profileDir_)) {
        return 0;
    }

    File dir = sourceFs->open(profileDir_);
    if (!dir || !dir.isDirectory()) {
        if (dir) {
            dir.close();
        }
        return 0;
    }

    size_t migrated = 0;
    File entry;
    while ((entry = dir.openNextFile())) {
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        String sourceName = basenameFromPath(entry.name());
        if (!sourceName.endsWith(".json") || sourceName.startsWith("_") || sourceName.startsWith(".")) {
            entry.close();
            continue;
        }

        String targetPath = profileDir_ + "/" + sourceName;
        if (fs_->exists(targetPath)) {
            entry.close();
            continue;
        }

        String tmpPath = targetPath + ".tmpimport";
        File out = fs_->open(tmpPath, FILE_WRITE);
        if (!out) {
            Serial.printf("[V1Profiles] Migration skipped (write open failed): %s\n", targetPath.c_str());
            entry.close();
            continue;
        }

        bool ok = true;
        uint8_t buffer[256];
        while (entry.available()) {
            size_t read = entry.read(buffer, sizeof(buffer));
            if (read == 0) {
                break;
            }
            if (out.write(buffer, read) != read) {
                ok = false;
                break;
            }
        }
        out.flush();
        out.close();
        entry.close();

        if (!ok) {
            fs_->remove(tmpPath);
            Serial.printf("[V1Profiles] Migration skipped (copy failed): %s\n", targetPath.c_str());
            continue;
        }
        if (!fs_->rename(tmpPath, targetPath)) {
            fs_->remove(tmpPath);
            Serial.printf("[V1Profiles] Migration skipped (rename failed): %s\n", targetPath.c_str());
            continue;
        }

        migrated++;
        Serial.printf("[V1Profiles] Migrated profile file: %s\n", targetPath.c_str());
    }
    dir.close();
    return migrated;
}

String V1ProfileManager::profilePath(const String& name) const {
    // Sanitize name for filesystem
    String safeName = name;
    safeName.replace("/", "_");
    safeName.replace("\\", "_");
    safeName.replace("..", "_");
    return profileDir_ + "/" + safeName + ".json";
}

std::vector<String> V1ProfileManager::listProfiles() const {
    std::vector<String> profiles;

    if (!ready_ || !fs_) {
        return profiles;
    }

    File dir = fs_->open(profileDir_);
    if (!dir || !dir.isDirectory()) {
        return profiles;
    }

    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        if (name.endsWith(".json")) {
            // Remove .json extension and path
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0) {
                name = name.substring(lastSlash + 1);
            }
            name = name.substring(0, name.length() - 5);  // Remove .json

            // Filter out system files that aren't user profiles
            if (name.startsWith("_") || name.startsWith(".")) {
                continue;
            }

            profiles.push_back(name);
        }
        entry.close();
    }
    dir.close();

    return profiles;
}

bool V1ProfileManager::loadProfile(const String& name, V1Profile& profile) const {
    if (!ready_ || !fs_) {
        return false;
    }

    String path = profilePath(name);
    String bakPath = path + ".bak";

    File file = fs_->open(path, FILE_READ);
    if (!file) {
        // Try to recover from backup file
        if (fs_->exists(bakPath)) {
            Serial.printf("[V1Profiles] Main file missing, attempting recovery from backup: %s\n", bakPath.c_str());
            // Rename backup to main file
            if (fs_->rename(bakPath, path)) {
                Serial.println("[V1Profiles] Restored profile from backup!");
                file = fs_->open(path, FILE_READ);
            }
        }

        if (!file) {
            Serial.printf("[V1Profiles] Profile not found: %s (no backup available)\n", path.c_str());
            return false;
        }
    }

    // Hard cap JSON size to avoid excessive allocation on small devices
    if (file.size() > 4096) {
        Serial.printf("[V1Profiles] Profile too large (%u bytes), aborting\n", (unsigned)file.size());
        file.close();
        return false;
    }

    // Read file content for CRC validation with RAII-managed storage
    // so all early returns remain leak-safe.
    const size_t fileSize = file.size();
    std::vector<uint8_t> fileContent(fileSize);
    if (fileSize > 0) {
        const size_t bytesRead = file.read(fileContent.data(), fileSize);
        if (bytesRead != fileSize) {
            lastError_ = "Failed to read complete profile file";
            Serial.printf("[V1Profiles] %s (%u/%u bytes)\n",
                          lastError_.c_str(),
                          static_cast<unsigned>(bytesRead),
                          static_cast<unsigned>(fileSize));
            file.close();
            return false;
        }
    }
    file.close();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, fileContent.data(), fileSize);

    if (err) {
        lastError_ = String("JSON parse error: ") + err.c_str();
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return false;
    }

    // Validate CRC32 if present
    if (doc["crc32"].is<uint32_t>()) {
        uint32_t storedCrc = doc["crc32"].as<uint32_t>();

        // Calculate CRC of the 6 settings bytes
        JsonArray bytesArr = doc["bytes"];
        if (bytesArr && bytesArr.size() == 6) {
            uint8_t settingsBytes[6];
            for (int i = 0; i < 6; i++) {
                settingsBytes[i] = bytesArr[i].as<uint8_t>();
            }
            uint32_t computedCrc = calculateCRC32(settingsBytes, 6);
            if (storedCrc != computedCrc) {
                lastError_ = "CRC mismatch - profile file corrupted";
                Serial.printf("[V1Profiles] %s (stored: %08lX, computed: %08lX)\n",
                    lastError_.c_str(),
                    static_cast<unsigned long>(storedCrc),
                    static_cast<unsigned long>(computedCrc));
                return false;
            }
            Serial.println("[V1Profiles] CRC32 validated OK");
        }
    }

    profile.name = name;
    profile.description = doc["description"] | "";
    profile.displayOn = doc["displayOn"] | true;  // Default to on
    profile.mainVolume = doc["mainVolume"] | 0xFF;  // 0xFF = don't change
    profile.mutedVolume = doc["mutedVolume"] | 0xFF;  // 0xFF = don't change

    // Parse settings bytes
    JsonArray bytes = doc["bytes"];
    if (bytes && bytes.size() == 6) {
        for (int i = 0; i < 6; i++) {
            profile.settings.bytes[i] = bytes[i].as<uint8_t>();
        }
    } else {
        // Try individual settings (legacy or human-readable format)
        V1UserSettings& s = profile.settings;
        s.setDefaults();

        if (!doc["xBand"].isNull()) s.setXBandEnabled(doc["xBand"]);
        if (!doc["kBand"].isNull()) s.setKBandEnabled(doc["kBand"]);
        if (!doc["kaBand"].isNull()) s.setKaBandEnabled(doc["kaBand"]);
        if (!doc["laser"].isNull()) s.setLaserEnabled(doc["laser"]);
        if (!doc["kuBand"].isNull()) s.setKuBandEnabled(doc["kuBand"]);
        if (!doc["euro"].isNull()) s.setEuroMode(doc["euro"]);
        if (!doc["kVerifier"].isNull()) s.setKVerifier(doc["kVerifier"]);
        if (!doc["laserRear"].isNull()) s.setLaserRear(doc["laserRear"]);
        if (!doc["customFreqs"].isNull()) s.setCustomFreqs(doc["customFreqs"]);
        if (!doc["kaAlwaysPriority"].isNull()) s.setKaAlwaysPriority(doc["kaAlwaysPriority"]);
        if (!doc["fastLaserDetect"].isNull()) s.setFastLaserDetect(doc["fastLaserDetect"]);
        if (!doc["kaSensitivity"].isNull()) s.setKaSensitivity(doc["kaSensitivity"]);
        if (!doc["kSensitivity"].isNull()) s.setKSensitivity(doc["kSensitivity"]);
        if (!doc["xSensitivity"].isNull()) s.setXSensitivity(doc["xSensitivity"]);
        if (!doc["autoMute"].isNull()) s.setAutoMute(doc["autoMute"]);
        if (!doc["muteToMuteVolume"].isNull()) s.setMuteToMuteVolume(doc["muteToMuteVolume"]);
        if (!doc["bogeyLockLoud"].isNull()) s.setBogeyLockLoud(doc["bogeyLockLoud"]);
        if (!doc["muteXKRear"].isNull()) s.setMuteXKRear(doc["muteXKRear"]);
        if (!doc["startupSequence"].isNull()) s.setStartupSequence(doc["startupSequence"]);
        if (!doc["restingDisplay"].isNull()) s.setRestingDisplay(doc["restingDisplay"]);
        if (!doc["bsmPlus"].isNull()) s.setBsmPlus(doc["bsmPlus"]);
        if (!doc["mrct"].isNull()) s.setMrct(doc["mrct"]);
    }

    Serial.printf("[V1Profiles] Loaded profile: %s\n", name.c_str());
    return true;
}

ProfileSaveResult V1ProfileManager::saveProfile(const V1Profile& profile) {
    if (!ready_ || !fs_) {
        lastError_ = "Filesystem not ready";
        Serial.printf("[V1Profiles] Save failed: %s\n", lastError_.c_str());
        return ProfileSaveResult(false, lastError_);
    }

    String path = profilePath(profile.name);
    String tmpPath = path + ".tmp";
    String bakPath = path + ".bak";

    // Step 1: Write to temporary file (don't truncate original yet)
    File file = fs_->open(tmpPath, FILE_WRITE);
    if (!file) {
        lastError_ = "Failed to create temp file: " + tmpPath;
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return ProfileSaveResult(false, lastError_);
    }

    JsonDocument doc;
    const V1UserSettings& s = profile.settings;

    // Store metadata
    doc["name"] = profile.name;
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
        return ProfileSaveResult(false, lastError_);
    }
    if (written != expectedPrettyBytes) {
        lastError_ = "Partial write detected: expected " +
                    String(static_cast<unsigned long>(expectedPrettyBytes)) +
                    " bytes, wrote " +
                    String(static_cast<unsigned long>(written));
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        fs_->remove(tmpPath);
        return ProfileSaveResult(false, lastError_);
    }
    {
        File verify = fs_->open(tmpPath, FILE_READ);
        if (!verify) {
            lastError_ = "Failed to re-open temp file for verification";
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            fs_->remove(tmpPath);
            return ProfileSaveResult(false, lastError_);
        }
        size_t fileSize = verify.size();
        verify.close();
        if (fileSize != written) {
            lastError_ = "Partial write detected: expected " + String(written) + " bytes, got " + String(fileSize);
            Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
            fs_->remove(tmpPath);
            return ProfileSaveResult(false, lastError_);
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
            Serial.printf("[V1Profiles] Warning: Could not create backup: %s\n", bakPath.c_str());
            // Continue anyway - this is not fatal
        } else {
            Serial.printf("[V1Profiles] Created backup: %s\n", bakPath.c_str());
        }
    }

    // Step 5: Rename temp to final
    if (!fs_->rename(tmpPath, path)) {
        lastError_ = "Failed to rename temp to final: " + tmpPath + " -> " + path;
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());

        // Try to restore from backup
        if (fs_->exists(bakPath)) {
            if (fs_->rename(bakPath, path)) {
                Serial.println("[V1Profiles] Restored from backup after failed save");
            }
        }
        fs_->remove(tmpPath);
        return ProfileSaveResult(false, lastError_);
    }

    // Step 6: Remove backup after successful save (optional - keep for extra safety)

    Serial.printf("[V1Profiles] Saved profile: %s (%u bytes, CRC: %08lX)\n",
        profile.name.c_str(), written, static_cast<unsigned long>(crc));
    bumpCatalogRevision();
    return ProfileSaveResult(true);
}

bool V1ProfileManager::deleteProfile(const String& name) {
    if (!ready_ || !fs_) {
        return false;
    }

    String path = profilePath(name);
    String bakPath = path + ".bak";
    bool removedAny = false;
    bool ok = true;

    if (fs_->exists(path)) {
        if (!fs_->remove(path)) {
            ok = false;
        } else {
            removedAny = true;
        }
    }

    if (fs_->exists(bakPath)) {
        if (!fs_->remove(bakPath)) {
            ok = false;
        } else {
            removedAny = true;
        }
    }

    if (ok && removedAny) {
        Serial.printf("[V1Profiles] Deleted profile: %s\n", name.c_str());
        bumpCatalogRevision();
    }
    return ok && removedAny;
}

bool V1ProfileManager::renameProfile(const String& oldName, const String& newName) {
    if (!ready_ || !fs_) {
        lastError_ = "Filesystem not ready";
        return false;
    }

    const String oldPath = profilePath(oldName);
    const String newPath = profilePath(newName);

    // Guard: exact no-op rename should not touch disk or revision state.
    if (oldName == newName) {
        return true;
    }

    V1Profile profile;
    if (!loadProfile(oldName, profile)) {
        return false;
    }

    // Sanitized path collision: update metadata in place without delete/recreate.
    if (oldPath == newPath) {
        profile.name = newName;
        ProfileSaveResult result = saveProfile(profile);
        return result.success;
    }

    // Guard: refuse to overwrite a different existing profile.
    if (fs_->exists(newPath)) {
        lastError_ = "Rename target already exists: " + newName;
        Serial.printf("[V1Profiles] %s\n", lastError_.c_str());
        return false;
    }

    profile.name = newName;
    ProfileSaveResult result = saveProfile(profile);
    if (!result.success) {
        return false;
    }

    if (!deleteProfile(oldName)) {
        Serial.println("[V1Profiles] Warning: rename saved new but failed to delete old");
    }
    return true;
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
    // Try raw bytes first (skip if empty to use individual settings)
    JsonArray bytes = settingsObj["bytes"];
    if (bytes && bytes.size() == 6) {
        for (int i = 0; i < 6; i++) {
            settings.bytes[i] = bytes[i].as<uint8_t>();
        }
        Serial.println("[V1Profiles] Loaded from raw bytes");
        return true;
    }

    // Parse individual settings
    settings.setDefaults();
    Serial.println("[V1Profiles] Parsing individual settings");
    bool anyField = false;

    if (!settingsObj["xBand"].isNull()) { settings.setXBandEnabled(settingsObj["xBand"]); anyField = true; }
    if (!settingsObj["kBand"].isNull()) { settings.setKBandEnabled(settingsObj["kBand"]); anyField = true; }
    if (!settingsObj["kaBand"].isNull()) { settings.setKaBandEnabled(settingsObj["kaBand"]); anyField = true; }
    if (!settingsObj["laser"].isNull()) { settings.setLaserEnabled(settingsObj["laser"]); anyField = true; }
    if (!settingsObj["kuBand"].isNull()) { settings.setKuBandEnabled(settingsObj["kuBand"]); anyField = true; }
    if (!settingsObj["euro"].isNull()) { settings.setEuroMode(settingsObj["euro"]); anyField = true; }
    if (!settingsObj["kVerifier"].isNull()) { settings.setKVerifier(settingsObj["kVerifier"]); anyField = true; }
    if (!settingsObj["laserRear"].isNull()) { settings.setLaserRear(settingsObj["laserRear"]); anyField = true; }
    if (!settingsObj["customFreqs"].isNull()) { settings.setCustomFreqs(settingsObj["customFreqs"]); anyField = true; }
    if (!settingsObj["kaAlwaysPriority"].isNull()) { settings.setKaAlwaysPriority(settingsObj["kaAlwaysPriority"]); anyField = true; }
    if (!settingsObj["fastLaserDetect"].isNull()) { settings.setFastLaserDetect(settingsObj["fastLaserDetect"]); anyField = true; }
    if (!settingsObj["kaSensitivity"].isNull()) { settings.setKaSensitivity(settingsObj["kaSensitivity"]); anyField = true; }
    if (!settingsObj["kSensitivity"].isNull()) { settings.setKSensitivity(settingsObj["kSensitivity"]); anyField = true; }
    if (!settingsObj["xSensitivity"].isNull()) { settings.setXSensitivity(settingsObj["xSensitivity"]); anyField = true; }
    if (!settingsObj["autoMute"].isNull()) { settings.setAutoMute(settingsObj["autoMute"]); anyField = true; }
    if (!settingsObj["muteToMuteVolume"].isNull()) { settings.setMuteToMuteVolume(settingsObj["muteToMuteVolume"]); anyField = true; }
    if (!settingsObj["bogeyLockLoud"].isNull()) { settings.setBogeyLockLoud(settingsObj["bogeyLockLoud"]); anyField = true; }
    if (!settingsObj["muteXKRear"].isNull()) { settings.setMuteXKRear(settingsObj["muteXKRear"]); anyField = true; }
    if (!settingsObj["startupSequence"].isNull()) { settings.setStartupSequence(settingsObj["startupSequence"]); anyField = true; }
    if (!settingsObj["restingDisplay"].isNull()) { settings.setRestingDisplay(settingsObj["restingDisplay"]); anyField = true; }
    if (!settingsObj["bsmPlus"].isNull()) { settings.setBsmPlus(settingsObj["bsmPlus"]); anyField = true; }
    if (!settingsObj["mrct"].isNull()) { settings.setMrct(settingsObj["mrct"]); anyField = true; }
    if (!settingsObj["driveSafe3D"].isNull()) { settings.setDriveSafe3D(settingsObj["driveSafe3D"]); anyField = true; }
    if (!settingsObj["driveSafe3DHD"].isNull()) { settings.setDriveSafe3DHD(settingsObj["driveSafe3DHD"]); anyField = true; }
    if (!settingsObj["redflexHalo"].isNull()) { settings.setRedflexHalo(settingsObj["redflexHalo"]); anyField = true; }
    if (!settingsObj["redflexNK7"].isNull()) { settings.setRedflexNK7(settingsObj["redflexNK7"]); anyField = true; }
    if (!settingsObj["ekin"].isNull()) { settings.setEkin(settingsObj["ekin"]); anyField = true; }
    if (!settingsObj["photoVerifier"].isNull()) { settings.setPhotoVerifier(settingsObj["photoVerifier"]); anyField = true; }

    if (!anyField) {
        Serial.println("[V1Profiles] No settings provided");
        return false;
    }

    Serial.printf("[V1Profiles] After parse - byte0=%02X byte2=%02X\n", settings.bytes[0], settings.bytes[2]);
    Serial.printf("[V1Profiles]   xBand=%d, restingDisplay=%d, bsmPlus=%d\n",
        settings.xBandEnabled(), settings.restingDisplay(), settings.bsmPlus());

    return true;
}
