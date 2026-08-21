#pragma once

#include <WebServer.h>

#include "wifi_audio_settings_runtime.h"

namespace WifiQuietApiService {

using Runtime = WifiAudioSettingsRuntime;

void handleApiGet(WebServer& server, const Runtime& runtime);
void handleApiSave(WebServer& server, const Runtime& runtime);

} // namespace WifiQuietApiService
