// Exercise policy changes against the real parser, pipeline and audio worker.
// Hardware and task scheduling are modeled; the startup delay is a legal yield
// point, not a measurement of physical sound or RF arrival timing.
#include "../support/audio_pipeline_harness.h"
#include "../../src/modules/speed_mute/speed_mute_module.cpp"

enum class Suppression { SoftMute, Proxy, VoiceDisabled, DetectorVolumeZero, Speed, InTheBox };
static constexpr Suppression suppressions[] = {
    Suppression::SoftMute, Suppression::Proxy, Suppression::VoiceDisabled,
    Suppression::DetectorVolumeZero, Suppression::Speed, Suppression::InTheBox};

struct SuppressionRuntime : PipelineAudio {
    QuietCoordinatorModule quiet;
    SpeedMuteModule speed;
    V1InTheBoxSettings boxSettings;
    uint16_t frequency = 34700;

    explicit SuppressionRuntime(Suppression gate = Suppression::SoftMute) {
        quiet.begin(&ble, &parser);
        speed.begin(true, 25, 3, 0, true);
        speed.update(50, true, mockMillis);
        settings.settings.muteVoiceIfVolZero = gate == Suppression::DetectorVolumeZero;
        if (gate == Suppression::InTheBox) {
            // Configured before admission; the first alert is inside a box.
            boxSettings.bands[3].muteOutside = true;
        }
        quiet.setInTheBoxSettings(boxSettings, ble.sessionGeneration());
        displayMute(false);
        DisplayPipelineDependencies deps;
        deps.displayMode = &mode;
        deps.display = &display;
        deps.parser = &parser;
        deps.settings = &settings;
        deps.ble = &ble;
        deps.alertPersistence = &persistence;
        deps.voice = &voice;
        deps.quiet = &quiet;
        deps.speedMute = &speed;
        pipeline.begin(deps);
    }

    void displayMute(bool softMuted) {
        packet(PACKET_ID_DISPLAY_DATA,
               {0x06, 0x06, 7, 0x22, 0x22, static_cast<uint8_t>(softMuted ? 0x0D : 0x0C), 0, 0x62});
        TEST_ASSERT_EQUAL(softMuted, parser.getDisplayState().softMuted);
    }

    void installCurrentAlert() {
        packet(PACKET_ID_ALERT_DATA,
               {0x11, static_cast<uint8_t>(frequency >> 8), static_cast<uint8_t>(frequency),
                0xAC, 0, 0x22, 0x80});
    }

    void currentAlert() {
        installCurrentAlert();
        quiet.processInTheBox(mockMillis);
        pipeline.handleParsed(mockMillis);
    }

    void apply(Suppression gate, bool active) {
        ++mockMillis;
        switch (gate) {
        case Suppression::SoftMute:
            displayMute(active);
            break;
        case Suppression::Proxy:
            ble.setProxyConnected(active);
            break;
        case Suppression::VoiceDisabled:
            // Settings transport is modeled; policy reads the real setting.
            settings.settings.voiceAlertMode = active ? VOICE_MODE_DISABLED : VOICE_MODE_BAND_ONLY;
            break;
        case Suppression::DetectorVolumeZero:
            packet(PACKET_ID_RESP_ALL_VOLUME, {static_cast<uint8_t>(active ? 0 : 6), 2, 6, 2});
            break;
        case Suppression::Speed:
            // Real policy owners, modeled speed input (no GPS/OBD transport).
            if (!active) mockMillis += 1500; // Respect the owner's transition debounce.
            speed.update(active ? 10 : 50, true, mockMillis);
            TEST_ASSERT_EQUAL(active, speed.getState().muteActive);
            break;
        case Suppression::InTheBox:
            // A real table replacement moves outside/inside the configured
            // box while the earlier announcement remains accepted.
            frequency = active ? 35000 : 34700;
            installCurrentAlert();
            quiet.processInTheBox(mockMillis);
            if (active) {
                TEST_ASSERT_TRUE(quiet.inTheBoxMuteActive());
                TEST_ASSERT_TRUE(ble.lastMuteValue);
            }
            break;
        }
    }
};

static void assertPending(AudioPlaybackReceipt* receipt) {
    TEST_ASSERT_NOT_NULL(receipt);
    TEST_ASSERT_TRUE(audio_playing.load());
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::Pending, receipt->state.load());
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
}

static void assertCancelled(AudioPlaybackReceipt* receipt) {
    TEST_ASSERT_NULL(sdAudioReceipt);
    TEST_ASSERT_FALSE(audio_playing.load());
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelledBeforeStart, receipt->state.load());
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
}

void test_every_suppression_gate_cancels_at_unlocked_startup_without_a_parsed_frame() {
    for (const auto gate : suppressions) {
        setUp();
        SuppressionRuntime runtime(gate);
        runtime.currentAlert();
        auto* receipt = sdAudioReceipt;
        assertPending(receipt);
        bool transitioned = false;
        audioWorkerDelayHook = [&](TickType_t ticks) {
            if (ticks != pdMS_TO_TICKS(50) || transitioned) return;
            transitioned = true;
            TEST_ASSERT_TRUE(g_mock_semaphore_state.heldHandles.empty());
            assertPending(receipt);
            runtime.apply(gate, true);
            // No handleParsed()/refresh/render edge: these policy inputs may
            // change independently of detector packet presentation.
            runtime.pipeline.servicePendingVoice();
            TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, receipt->state.load());
            TEST_ASSERT_TRUE(receipt->isOwned());
            TEST_ASSERT_TRUE(audio_playing.load());
        };
        runAcceptedJob();
        audioWorkerDelayHook = nullptr;
        TEST_ASSERT_TRUE(transitioned);
        assertCancelled(receipt);
        TEST_ASSERT_FALSE(amp_is_warm.load());
        TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);

        // Service alone is cancellation-only; it must not start replacement
        // audio when the mute disappears. The next current alert can retry.
        runtime.apply(gate, false);
        runtime.pipeline.servicePendingVoice();
        TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
        mockMillis += 101;
        runtime.currentAlert();
        TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
        runAcceptedJob();
        TEST_ASSERT_GREATER_THAN_UINT(0, g_mock_i2s_state.writeCalls);
        for (unsigned i = 0; i < 30; ++i) {
            mockMillis += 100;
            runtime.currentAlert();
        }
        TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    }
}

void test_every_gate_blocks_admission_without_creating_a_worker_job() {
    for (const auto gate : suppressions) {
        setUp();
        SuppressionRuntime runtime(gate);
        // Install the canonical table without presenting it so In-The-Box
        // can inspect the same fresh alert before any voice admission.
        runtime.packet(PACKET_ID_ALERT_DATA,
                       {0x11, static_cast<uint8_t>(runtime.frequency >> 8),
                        static_cast<uint8_t>(runtime.frequency), 0xAC, 0, 0x22, 0x80});
        runtime.apply(gate, true);
        runtime.pipeline.handleParsed(mockMillis);
        TEST_ASSERT_EQUAL_UINT(0, g_mock_task_notify_state.giveCalls);
        TEST_ASSERT_FALSE(audio_playing.load());
    }
}

void test_warm_and_cold_jobs_cancel_before_worker_dispatch() {
    for (const bool warm : {false, true}) {
        setUp();
        SuppressionRuntime runtime;
        amp_is_warm = warm;
        runtime.currentAlert();
        auto* receipt = sdAudioReceipt;
        assertPending(receipt);
        runtime.apply(Suppression::SoftMute, true);
        runtime.pipeline.servicePendingVoice();
        TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, receipt->state.load());
        runAcceptedJob();
        assertCancelled(receipt);
    }
}

void test_parse_path_cancels_without_waiting_for_periodic_service() {
    SuppressionRuntime runtime;
    runtime.currentAlert();
    auto* receipt = sdAudioReceipt;
    runtime.apply(Suppression::SoftMute, true);
    runtime.pipeline.handleParsed(mockMillis);
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, receipt->state.load());
    runAcceptedJob();
    assertCancelled(receipt);
}

void test_suppression_after_clear_still_cancels_the_owned_old_job() {
    for (const auto gate : {Suppression::SoftMute, Suppression::Proxy, Suppression::VoiceDisabled,
                            Suppression::DetectorVolumeZero, Suppression::Speed}) {
        setUp();
        SuppressionRuntime runtime(gate);
        runtime.currentAlert();
        auto* receipt = sdAudioReceipt;
        runtime.clear();
        assertPending(receipt); // A clear itself still retains the brief warning.
        runtime.apply(gate, true);
        runtime.pipeline.servicePendingVoice();
        runAcceptedJob();
        assertCancelled(receipt);
        runtime.apply(gate, false);
        for (unsigned i = 0; i < 5; ++i) {
            mockMillis += 100;
            runtime.pipeline.servicePendingVoice();
            runtime.pipeline.handleParsed(mockMillis);
        }
        TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    }
}

void test_clear_and_new_episode_cannot_reuse_cancel_requested_receipt() {
    SuppressionRuntime runtime;
    runtime.settings.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;
    runtime.currentAlert();
    auto* oldReceipt = sdAudioReceipt;
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_34"));
    runtime.clear();
    runtime.frequency = 35500;
    runtime.currentAlert();
    runtime.apply(Suppression::SoftMute, true);
    runtime.pipeline.servicePendingVoice();
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, oldReceipt->state.load());
    // Even unmuting and presenting the new episode before worker cleanup
    // cannot overwrite its immutable clips or receipt ownership.
    runtime.apply(Suppression::SoftMute, false);
    mockMillis += 101;
    runtime.currentAlert();
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_EQUAL_PTR(oldReceipt, sdAudioReceipt);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_34"));
    runAcceptedJob();
    assertCancelled(oldReceipt);
    mockMillis += 101;
    runtime.currentAlert();
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_35"));
    runAcceptedJob();
    for (unsigned i = 0; i < 30; ++i) {
        mockMillis += 100;
        runtime.currentAlert();
    }
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
}

void test_unmute_before_cleanup_does_not_resurrect_cancelled_job() {
    SuppressionRuntime runtime;
    runtime.currentAlert();
    auto* receipt = sdAudioReceipt;
    bool transitioned = false;
    audioWorkerDelayHook = [&](TickType_t ticks) {
        if (ticks != pdMS_TO_TICKS(50) || transitioned) return;
        transitioned = true;
        runtime.apply(Suppression::SoftMute, true);
        runtime.pipeline.servicePendingVoice();
        runtime.apply(Suppression::SoftMute, false);
        runtime.pipeline.servicePendingVoice();
        runtime.currentAlert();
        TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, receipt->state.load());
        TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    };
    runAcceptedJob();
    audioWorkerDelayHook = nullptr;
    TEST_ASSERT_TRUE(transitioned);
    assertCancelled(receipt);
    mockMillis += 101;
    runtime.currentAlert();
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    runAcceptedJob();
}

void test_clear_without_suppression_preserves_brief_warning() {
    SuppressionRuntime runtime;
    runtime.currentAlert();
    auto* receipt = sdAudioReceipt;
    runtime.clear();
    runtime.pipeline.servicePendingVoice();
    assertPending(receipt);
    runAcceptedJob();
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::Started, receipt->state.load());
    TEST_ASSERT_EQUAL_UINT(22, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
}

void test_priority_change_without_clear_preserves_accepted_job_and_announces_new_priority_once() {
    SuppressionRuntime runtime;
    runtime.settings.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;
    runtime.currentAlert();
    const auto lifetime = runtime.parser.alertLifetime();
    runtime.frequency = 35500;
    runtime.currentAlert();
    TEST_ASSERT_EQUAL_UINT32(lifetime, runtime.parser.alertLifetime());
    runtime.pipeline.servicePendingVoice();
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_34"));
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    runAcceptedJob();
    mockMillis += 101;
    runtime.currentAlert();
    TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    mockMillis += 1900; // The retained announcement still owns its normal cooldown.
    runtime.currentAlert();
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_35"));
    runAcceptedJob();
    for (unsigned i = 0; i < 30; ++i) {
        mockMillis += 100;
        runtime.currentAlert();
    }
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
}

void test_all_suppression_gates_preserve_already_started_speech() {
    for (const auto gate : suppressions) {
        setUp();
        SuppressionRuntime runtime(gate);
        runtime.currentAlert();
        auto* receipt = sdAudioReceipt;
        g_mock_i2s_state.onWrite = [&] {
            if (g_mock_i2s_state.writeCalls != 1) return;
            TEST_ASSERT_NULL(sdAudioReceipt);
            TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::Started, receipt->state.load());
            runtime.apply(gate, true);
            runtime.pipeline.servicePendingVoice();
        };
        runAcceptedJob();
        g_mock_i2s_state.onWrite = nullptr;
        TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::Started, receipt->state.load());
        TEST_ASSERT_EQUAL_UINT(22, g_mock_i2s_state.writeCalls);
        TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
        runtime.apply(gate, false);
        mockMillis += 101;
        runtime.currentAlert();
        TEST_ASSERT_EQUAL_UINT(1, g_mock_task_notify_state.giveCalls);
    }
}

void test_local_speaker_zero_is_applied_before_every_audio_submission() {
    SuppressionRuntime runtime;
    runtime.currentAlert();
    bool transitioned = false;
    audioWorkerDelayHook = [&](TickType_t ticks) {
        if (ticks != pdMS_TO_TICKS(50) || transitioned) return;
        transitioned = true;
        audio_set_volume(0);
        runtime.pipeline.servicePendingVoice();
    };
    g_mock_i2s_state.onWrite = [&] {
        TEST_ASSERT_TRUE(transitioned);
        TEST_ASSERT_EQUAL_UINT8(0, lastVolumeRegister());
    };
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(22, g_mock_i2s_state.writeCalls);
    // Submission at codec zero is not a claim that these samples are audible.
}

void test_direct_test_speech_does_not_inherit_cancelled_alert_receipt() {
    SuppressionRuntime runtime;
    runtime.currentAlert();
    runtime.apply(Suppression::Proxy, true);
    runtime.pipeline.servicePendingVoice();
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(0, g_mock_i2s_state.writeCalls);
    play_test_voice(); // Receiptless test entrypoint is independent of alert policy.
    TEST_ASSERT_TRUE(audio_playing.load());
    TEST_ASSERT_NULL(sdAudioReceipt);
    runtime.pipeline.servicePendingVoice();
    runAcceptedJob();
    TEST_ASSERT_EQUAL_UINT(22, g_mock_i2s_state.writeCalls);
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
}

static void cancelSelectedActionAndResumeOnce(SuppressionRuntime& runtime,
                                             const std::function<void()>& presentCurrentTable) {
    auto* receipt = sdAudioReceipt;
    TEST_ASSERT_NOT_NULL(receipt);
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::Pending, receipt->state.load());
    const auto acceptedClips = g_sdAudioTaskParams;
    const auto submissions = g_mock_i2s_state.writeCalls;
    const auto jobs = g_mock_task_notify_state.giveCalls;
    runtime.apply(Suppression::SoftMute, true);
    runtime.pipeline.servicePendingVoice();
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelRequested, receipt->state.load());
    runAcceptedJob();
    TEST_ASSERT_EQUAL(AudioPlaybackReceipt::State::CancelledBeforeStart, receipt->state.load());
    TEST_ASSERT_EQUAL_UINT(submissions, g_mock_i2s_state.writeCalls);
    runtime.apply(Suppression::SoftMute, false);
    mockMillis += 101;
    presentCurrentTable();
    TEST_ASSERT_EQUAL_UINT(jobs + 1, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_EQUAL_INT(acceptedClips.numClips, g_sdAudioTaskParams.numClips);
    for (int i = 0; i < acceptedClips.numClips; ++i) {
        TEST_ASSERT_EQUAL_STRING(acceptedClips.filePaths[i], g_sdAudioTaskParams.filePaths[i]);
    }
    runAcceptedJob();
    TEST_ASSERT_GREATER_THAN_UINT(submissions, g_mock_i2s_state.writeCalls);
    for (unsigned i = 0; i < 30; ++i) {
        mockMillis += 100;
        presentCurrentTable();
    }
    TEST_ASSERT_EQUAL_UINT(jobs + 1, g_mock_task_notify_state.giveCalls);
}

void test_direction_action_cancels_and_resumes_without_repeating_priority() {
    SuppressionRuntime runtime;
    runtime.currentAlert();
    runAcceptedJob();
    mockMillis += 100;
    runtime.currentAlert();
    mockMillis += 2000;
    runtime.alert(34700, 0x80);
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_EQUAL_INT(1, g_sdAudioTaskParams.numClips);
    TEST_ASSERT_EQUAL_STRING("/audio/dir_behind.mul", g_sdAudioTaskParams.filePaths[0]);
    cancelSelectedActionAndResumeOnce(runtime, [&] { runtime.alert(34700, 0x80); });
}

static void presentTwoAlerts(SuppressionRuntime& runtime) {
    runtime.packet(PACKET_ID_ALERT_DATA, {0x12, 0x87, 0x8C, 0xAC, 0, 0x22, 0x80});
    runtime.packet(PACKET_ID_ALERT_DATA, {0x22, 0x8A, 0xAC, 0x90, 0, 0x22, 0});
    runtime.pipeline.handleParsed(mockMillis);
}

void test_secondary_action_cancels_and_resumes_without_repeating_priority() {
    SuppressionRuntime runtime;
    runtime.settings.settings.voiceAlertMode = VOICE_MODE_BAND_FREQ;
    runtime.settings.settings.voiceDirectionEnabled = false;
    runtime.settings.settings.announceSecondaryAlerts = true;
    runtime.settings.settings.secondaryKa = true;
    presentTwoAlerts(runtime);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_34"));
    runAcceptedJob();
    mockMillis += 1600;
    presentTwoAlerts(runtime);
    TEST_ASSERT_EQUAL_UINT(2, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_NOT_NULL(strstr(g_sdAudioTaskParams.filePaths[1], "tens_35"));
    cancelSelectedActionAndResumeOnce(runtime, [&] { presentTwoAlerts(runtime); });
}

static void presentEscalationTable(SuppressionRuntime& runtime, uint8_t secondaryStrength) {
    runtime.packet(PACKET_ID_ALERT_DATA, {0x13, 0x87, 0x8C, 0xAC, 0, 0x22, 0x80});
    runtime.packet(PACKET_ID_ALERT_DATA, {0x23, 0x8A, 0xAC, 0, secondaryStrength, 0x82, 0});
    runtime.packet(PACKET_ID_ALERT_DATA, {0x33, 0x5E, 0x56, 0x90, 0, 0x44, 0});
    runtime.pipeline.handleParsed(mockMillis);
}

void test_escalation_action_cancels_and_resumes_full_breakdown_once() {
    SuppressionRuntime runtime;
    runtime.settings.settings.voiceAlertMode = VOICE_MODE_FREQ_ONLY;
    runtime.settings.settings.voiceDirectionEnabled = true;
    runtime.settings.settings.announceBogeyCount = true;
    runtime.settings.settings.announceSecondaryAlerts = true;
    runtime.settings.settings.secondaryKa = true;
    runtime.settings.settings.secondaryK = false;
    presentEscalationTable(runtime, 0x90);
    runAcceptedJob();
    mockMillis += 1600;
    presentEscalationTable(runtime, 0x90);
    runAcceptedJob();
    mockMillis += 100;
    presentEscalationTable(runtime, 0x90);
    mockMillis += 600;
    presentEscalationTable(runtime, 0x9E);
    mockMillis += 600;
    presentEscalationTable(runtime, 0x9E);
    TEST_ASSERT_EQUAL_UINT(3, g_mock_task_notify_state.giveCalls);
    TEST_ASSERT_EQUAL_INT(12, g_sdAudioTaskParams.numClips);
    TEST_ASSERT_EQUAL_STRING("/audio/digit_3.mul", g_sdAudioTaskParams.filePaths[4]);
    TEST_ASSERT_EQUAL_STRING("/audio/dir_ahead.mul", g_sdAudioTaskParams.filePaths[7]);
    TEST_ASSERT_EQUAL_STRING("/audio/dir_behind.mul", g_sdAudioTaskParams.filePaths[9]);
    TEST_ASSERT_EQUAL_STRING("/audio/dir_side.mul", g_sdAudioTaskParams.filePaths[11]);
    cancelSelectedActionAndResumeOnce(runtime, [&] { presentEscalationTable(runtime, 0x9E); });
}

int main() {
    std::filesystem::copy(std::filesystem::path(PROJECT_DIR) / "tools" / "freq_audio" / "mulaw",
                          audioRoot / "audio", std::filesystem::copy_options::recursive);
    UNITY_BEGIN();
    RUN_TEST(test_every_suppression_gate_cancels_at_unlocked_startup_without_a_parsed_frame);
    RUN_TEST(test_every_gate_blocks_admission_without_creating_a_worker_job);
    RUN_TEST(test_warm_and_cold_jobs_cancel_before_worker_dispatch);
    RUN_TEST(test_parse_path_cancels_without_waiting_for_periodic_service);
    RUN_TEST(test_suppression_after_clear_still_cancels_the_owned_old_job);
    RUN_TEST(test_clear_and_new_episode_cannot_reuse_cancel_requested_receipt);
    RUN_TEST(test_unmute_before_cleanup_does_not_resurrect_cancelled_job);
    RUN_TEST(test_clear_without_suppression_preserves_brief_warning);
    RUN_TEST(test_priority_change_without_clear_preserves_accepted_job_and_announces_new_priority_once);
    RUN_TEST(test_all_suppression_gates_preserve_already_started_speech);
    RUN_TEST(test_local_speaker_zero_is_applied_before_every_audio_submission);
    RUN_TEST(test_direct_test_speech_does_not_inherit_cancelled_alert_receipt);
    RUN_TEST(test_direction_action_cancels_and_resumes_without_repeating_priority);
    RUN_TEST(test_secondary_action_cancels_and_resumes_without_repeating_priority);
    RUN_TEST(test_escalation_action_cancels_and_resumes_full_breakdown_once);
    const int result = UNITY_END();
    std::filesystem::remove_all(audioRoot);
    return result;
}
