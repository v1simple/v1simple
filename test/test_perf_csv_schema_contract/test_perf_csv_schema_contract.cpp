#include <unity.h>

#include <cctype>
#include <fstream>
#include <string>

namespace {

std::string readTextFile(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// This suite asserts against production SOURCE TEXT, so it must not be
// sensitive to how that source is wrapped. clang-format legitimately
// (a) splits a long string literal into adjacent literals, which the compiler
// concatenates -- PERF_CSV_HEADER is one 8000-char literal -- and (b) wraps a
// declaration that exceeds the column limit.  Neither changes the program, but
// both defeat a raw substring search, and a source-text assertion that stops
// matching anything would pass vacuously and silently stop guarding the schema.
//
// Normalize the way the compiler sees it: concatenate adjacent string literals,
// then collapse whitespace runs. scripts/check_perf_csv_column_contract.py
// already parses PERF_CSV_HEADER as a sequence of adjacent literals for the
// same reason; this keeps the native suite consistent with it.
std::string normalizeSource(const std::string& source) {
    std::string joined;
    joined.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        if (source[i] == '"') {
            // Look ahead: closing quote, only whitespace, then an opening quote
            // => adjacent literals. Drop both quotes to splice them together.
            size_t j = i + 1;
            while (j < source.size() && std::isspace(static_cast<unsigned char>(source[j]))) {
                ++j;
            }
            if (j < source.size() && source[j] == '"' && !joined.empty() && joined.back() != '\\') {
                i = j + 1; // skip the closing and reopening quote
                continue;
            }
        }
        joined.push_back(source[i]);
        ++i;
    }

    std::string collapsed;
    collapsed.reserve(joined.size());
    bool inWhitespace = false;
    for (char c : joined) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!inWhitespace) {
                collapsed.push_back(' ');
                inWhitespace = true;
            }
        } else {
            collapsed.push_back(c);
            inWhitespace = false;
        }
    }
    return collapsed;
}

std::string readNormalizedSource(const char* path) {
    return normalizeSource(readTextFile(path));
}

} // namespace

void setUp() {}
void tearDown() {}

void test_perf_csv_schema_version_matches_current_header() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    // v45 drops unused priority-display-index and proxy-latency telemetry.
    // v44 drops retired secondary-card and manual AP-disable telemetry.
    // v43 drops retired local audio/voice metrics.
    // v42 adds UnionExceedsCap dirty-source diagnostics.
    // v41 adds resting flush/status paint attribution.
    // v40 added partial-flush shape and shadow row-cap diagnostics.
    // v39 added speedGpsSelections (GPS as secondary speed source).
    // v38 added notifyToDisplayMax_ms and notifyToDisplayTotalCount appended at end.
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("static constexpr uint32_t PERF_CSV_SCHEMA_VERSION = 45;"));
}

void test_perf_csv_header_includes_car_mode_alp_silence_metric() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("powerAutoPowerTimerStart,powerAutoPowerTimerCancel,powerAutoPowerTimerExpire,"
                                      "powerCarModeAlpSilenceExpire,powerCriticalWarn"));
}

void test_perf_csv_header_drops_retired_audio_voice_columns() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceQueued"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("cameraVoiceStarted"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("displayVoiceMax_us"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("voiceAnnouncePriority"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("voiceAnnounceDirection"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("voiceAnnounceSecondary"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("voiceAnnounceEscalation"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("voiceDirectionThrottled"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("audioPlayCount"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("audioPlayBusy"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("audioTaskFail"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdVinDetected"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdVehicleFamily"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotValid"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotC_x10"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotAgeMs"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotProfileId"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("obdEotProbeFailures"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("displayLiveFallbackToUsable,obdMax_us,obdConnectCallMax_us,obdSecurityStartCallMax_us,"
                    "obdDiscoveryCallMax_us,obdSubscribeCallMax_us,obdWriteCallMax_us,obdRssiCallMax_us,obdPollErrors,"
                    "obdStaleCount,perfDrop,eventBusDrops"));
}

void test_perf_csv_header_drops_retired_secondary_card_columns() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, source.find("displayCardsOnlyRenderCount"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("displayCardsMax_us"));
    TEST_ASSERT_EQUAL(std::string::npos, source.find("displayCardsMaxUs"));
}

void test_perf_csv_header_drops_unused_priority_display_index_column() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_EQUAL(std::string::npos, source.find("prioritySelectDisplayIndex"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("perfReportMax_us,prioritySelectRowFlag,prioritySelectFirstUsable"));
}

void test_perf_csv_header_appends_drive_gate_columns() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("perfDrop,eventBusDrops,wifiHandleClientMax_us,wifiMaintenanceMax_us,wifiStatusCheckMax_us,"
                    "wifiTimeoutCheckMax_us,wifiHeapGuardMax_us,wifiApStaPollMax_us,wifiStopHttpServerMax_us,"
                    "wifiStopStaDisconnectMax_us,wifiStopApDisableMax_us,wifiStopModeOffMax_us,wifiStartPreflightMax_"
                    "us,wifiStartApBringupMax_us,freeDmaMin,largestDmaMin,bleState,subscribeStep,connectInProgress,"
                    "asyncConnectPending,pendingDisconnectCleanup,proxyAdvertising,"
                    "proxyAdvertisingLastTransitionReason,wifiPriorityMode,speedSourceSelected,speedSourceValid,"
                    "speedSelectedMph_x10,speedSelectedAgeMs,speedSourceSwitches,speedNoSourceSelections,"
                    "speedGpsSelections,cycleState,cycleTransitionsTotal,cycleTimeInStateMs,cycleTeardownDurationMs,"
                    "cycleObdRetryAttemptsTotal,cycleWifiManualPhoneKicksTotal,cycleProxyNoClientLatched"));
}

void test_perf_csv_header_includes_connect_burst_attribution_columns() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "bleFirstRxMs,bleFollowupRequestAlertMax_us,bleFollowupRequestVersionMax_us,bleConnectStableCallbackMax_us,"
            "bleProxyStartMax_us,displayGapRecoverMax_us,displayFullRenderCount"));
}

void test_perf_csv_header_includes_partial_flush_region_union_columns() {
    // v36 schema: adjacent to the existing FlashTick redraw-reason counter,
    // the 4 new columns from the region-union partial-flush dispatch must
    // appear in a fixed order so downstream parsers (SD log analyzer,
    // score_hardware_run.py) can locate them positionally.
    // See docs/plans/PARTIAL_FLUSH_REGION_UNION_20260422.md.
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("displayRedrawReasonFlashTickCount,displayRedrawReasonFullFlushForRedrawCount,"
                    "displayRedrawReasonCacheHitSkipFlushCount,displayRedrawReasonUnionExceedsCapCount,"
                    "displayRedrawReasonPartialRegionFlushCount"));
}

void test_perf_csv_header_includes_display_attribution_columns() {
    const std::string source = readNormalizedSource("src/perf_sd_logger.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("displayRestingFlushReasonFullRedrawCount,displayRestingFlushReasonPendingExternalCount,"
                    "displayRestingFlushReasonPaintedCount,displayRestingFlushReasonCacheHitCount,"
                    "displayPersistedFlushReasonFullRedrawCount,displayPersistedFlushReasonPendingExternalCount,"
                    "displayPersistedFlushReasonPaintedCount,displayPersistedFlushReasonCacheHitCount,"
                    "displayStatusVolumePaintCount,displayStatusRssiPaintCount,displayStatusProfilePaintCount,"
                    "displayStatusBatteryPaintCount,displayStatusBleProxyPaintCount,displayStatusWifiPaintCount,"
                    "displayStatusObdPaintCount,displayStatusGpsPaintCount,displayStatusAlpPaintCount,"
                    "displayRedrawReasonFirstRunCount"));

    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "displayFullFlushCount,displayPartialFlushCount,displayPartialFlushAreaPeakPx,"
            "displayPartialFlushAreaTotalPx,displayFlushEquivalentAreaTotalPx,displayFlushMaxAreaPx,"
            "displayPartialFlushLogicalWidthPeakPx,displayPartialFlushLogicalHeightPeakPx,"
            "displayPartialFlushRowCallsPeak,displayPartialFlushPixelsPerRowPeakPx,displayPartialFlushUsPeak_us,"
            "displayPartialFlushWorstUsLogicalWidthPx,displayPartialFlushWorstUsLogicalHeightPx,"
            "displayPartialFlushWorstUsAreaPx,displayPartialFlushWouldFullRows64Count,"
            "displayPartialFlushWouldFullRows128Count,displayPartialFlushWouldFullRows256Count,"
            "displayUnionExceedsCapAreaPeakPx,displayUnionExceedsCapRectCountPeak,"
            "displayUnionExceedsCapAreaPeakSourceMask,displayUnionExceedsCapWithFrequencyCount,"
            "displayUnionExceedsCapWithBandsBarsCount,displayUnionExceedsCapWithArrowsCount,"
            "displayUnionExceedsCapWithStatusCount,displayUnionExceedsCapWithIndicatorsCount,"
            "displayUnionExceedsCapWithExternalCount,displayUnionExceedsCapUnclassifiedCount,displayBaseFrameMax_us,"
            "displayStatusStripMax_us,displayFrequencyMax_us,displayBandsBarsMax_us,displayArrowsIconsMax_us,"
            "displayFlushSubphaseMax_us,displayLiveRenderMax_us,displayRestingRenderMax_us,displayPersistedRenderMax_"
            "us,displayPreviewRenderMax_us,displayRestoreRenderMax_us,displayPreviewFirstRenderMax_us,"
            "displayPreviewSteadyRenderMax_us,alertPersistStarts,alertPersistStartsSkippedActive,"
            "alertPersistStartsSkippedInvalid,alertPersistExpires,alertPersistClears"));
}

void test_perf_metrics_exports_render_and_connect_burst_sources() {
    const std::string source = readNormalizedSource("src/perf_snapshot.cpp");

    TEST_ASSERT_FALSE_MESSAGE(source.empty(), "failed to read src/perf_snapshot.cpp");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("flat.dispMaxUs = metrics.displayRenderMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("flat.bleProxyStartMaxUs = metrics.bleProxyStartMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("flat.displayGapRecoverMaxUs = metrics.displayGapRecoverMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("flat.displayPartialFlushAreaPeakPx = metrics.displayPartialFlushAreaPeakPx;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayPartialFlushRowCallsPeak = metrics.displayPartialFlushRowCallsPeak;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find(
            "flat.displayPartialFlushWouldFullRows128Count = metrics.displayPartialFlushWouldFullRows128Count;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayUnionExceedsCapAreaPeakPx = metrics.displayUnionExceedsCapAreaPeakPx;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.displayUnionExceedsCapWithArrowsCount = metrics.displayUnionExceedsCapWithArrowsCount;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("flat.displayFlushSubphaseMaxUs = metrics.displayFlushSubphaseMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("flat.displayPreviewFirstRenderMaxUs = metrics.displayPreviewFirstRenderMaxUs;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.speedSourceSelected = static_cast<uint8_t>(ctx.speedStatus.selectedSource);"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("flat.speedSelectedMph_x10 ="));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("flat.speedSourceSwitches = ctx.speedStatus.sourceSwitches;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("flat.cycleTransitionsTotal = ctx.connectionCycleTransitionsTotal;"));
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        source.find("flat.cycleProxyNoClientLatched = ctx.connectionCycleProxyNoClientLatched ? 1 : 0;"));
}

void test_perf_csv_header_includes_gps_utc_and_observability_columns() {
    const std::string sdLogger = readNormalizedSource("src/perf_sd_logger.cpp");
    const std::string metrics = readNormalizedSource("src/perf_snapshot.cpp");

    TEST_ASSERT_FALSE_MESSAGE(sdLogger.empty(), "failed to read src/perf_sd_logger.cpp");
    TEST_ASSERT_FALSE_MESSAGE(metrics.empty(), "failed to read src/perf_snapshot.cpp");

    // UTC column at position 2 (after millis)
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sdLogger.find("millis,utc,rx,"));
    // GPS observability columns appended at end of header
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          sdLogger.find("gpsSentencesOk,gpsSentencesChecksumFail,gpsSentencesUnknown,"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, sdLogger.find("gpsHasFix,gpsStableHasFix,gpsEnableTransitions"));
    // UTC helper present in row writer
    TEST_ASSERT_NOT_EQUAL(
        std::string::npos,
        sdLogger.find("appendCsvUtcField(line, lineBufferLen, offset, snapshot.utcEpochMs, snapshot.utcValid)"));
    // GPS fields populated in perf_snapshot.cpp. These were written against
    // hand-aligned assignments; clang-format collapses that padding, and the
    // reader normalizes whitespace, so the expectation is the single-space form.
    TEST_ASSERT_NOT_EQUAL(std::string::npos, metrics.find("flat.gpsSentencesOk = gpsStatus.sentencesParsed;"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, metrics.find("flat.gpsEnableTransitions = gpsStatus.enableTransitions;"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_perf_csv_schema_version_matches_current_header);
    RUN_TEST(test_perf_csv_header_includes_car_mode_alp_silence_metric);
    RUN_TEST(test_perf_csv_header_drops_retired_audio_voice_columns);
    RUN_TEST(test_perf_csv_header_drops_retired_secondary_card_columns);
    RUN_TEST(test_perf_csv_header_drops_unused_priority_display_index_column);
    RUN_TEST(test_perf_csv_header_appends_drive_gate_columns);
    RUN_TEST(test_perf_csv_header_includes_connect_burst_attribution_columns);
    RUN_TEST(test_perf_csv_header_includes_partial_flush_region_union_columns);
    RUN_TEST(test_perf_csv_header_includes_display_attribution_columns);
    RUN_TEST(test_perf_metrics_exports_render_and_connect_burst_sources);
    RUN_TEST(test_perf_csv_header_includes_gps_utc_and_observability_columns);
    return UNITY_END();
}
