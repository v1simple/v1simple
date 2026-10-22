#include "../fixtures/alp_display_runtime.h"

void test_disconnected_alp_end_paints_scan_and_repeated_idle_ticks_do_not_repaint() {
    Runtime r;
    r.live();
    r.silence();
    r.scan();
    r.clearPaint();
    r.pipeline.handleParsed(3212);
    r.pipeline.refreshBlinkTick(3213);
    TEST_ASSERT_TRUE(regionTransfers.empty());
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFlushCount());
    r.feed(3300, 0xB0, 0x03, 0); // IDLE -> LISTENING changes the ALP badge.
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFillScreenCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
    r.clearPaint();
    r.feed(3301, 0xB0, 0x03, 0);
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
    TEST_ASSERT_TRUE(r.pipeline.restoreCurrentOwner(3302));
    r.scan(); // Explicit restoration must repaint a replaced temporary owner.
}

void test_disconnected_persisted_tail_expires_without_any_packet() {
    Runtime r(false, 3);
    r.live();
    r.silence();
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, r.mode);
    TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.clearPaint();
    r.tick(6210);
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFlushCount());
    r.tick(6211);
    r.scan();
    TEST_ASSERT_FALSE(r.latch.isLatched());
    r.clearPaint();
    r.tick(6212);
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFlushCount());
}

void test_scan_refreshes_real_alp_badge_without_repainting_primary_or_unchanged_status() {
    Runtime r;
    r.live();
    r.silence();
    r.scan();
    TEST_ASSERT_EQUAL(AlpState::IDLE, r.display.ut_elementCaches().alp.lastState);

    for (uint8_t mode : {0x03, 0x04}) {
        r.clearPaint();
        r.feed(3300 + mode, 0xB0, mode, 0);
        TEST_ASSERT_EQUAL(AlpState::LISTENING, r.alp.getState());
        TEST_ASSERT_EQUAL(AlpState::LISTENING, r.display.ut_elementCaches().alp.lastState);
        TEST_ASSERT_EQUAL(mode, r.display.ut_elementCaches().alp.lastHbByte1);
        // Keep the existing primary canvas, but deliver changed SCAN status
        // through the full-frame path required by the physical panel.
        TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
        TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFillScreenCount());
        TEST_ASSERT_TRUE(regionTransfers.empty());
        TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
        TEST_ASSERT_FALSE(r.display.ut_alpFreqOverride());
    }

    r.clearPaint();
    r.feed(3400, 0xB0, 0x04, 0);
    r.pipeline.refreshBlinkTick(3401);
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFillScreenCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
}

void test_scan_theme_change_repaints_once_then_returns_to_idle_noop() {
    Runtime r;
    r.pipeline.handleParsed(100);
    r.scan();
    r.clearPaint();
    r.display.ut_bumpPaletteRevision();
    r.pipeline.handleParsed(101);
    r.scan();
    r.clearPaint();
    r.pipeline.handleParsed(102);
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_TRUE(regionTransfers.empty());
}

void test_disconnected_blink_expiry_restores_scan_even_with_stealth_enabled() {
    Runtime r(false, 3);
    r.settings.mutableSettings().stealthEnabled = true;
    r.live();
    r.silence();
    r.clearPaint();
    mockMillis = 6211;
    r.pipeline.refreshBlinkTick(mockMillis);
    r.scan();
    TEST_ASSERT_FALSE(r.display.isStealthScreen());
}

void test_abnormal_listening_hold_and_tail_each_expire_without_packets() {
    Runtime r(false, 3);
    r.live();
    r.feed(250, 0xD0, 0, 0xFD);
    for (uint32_t now = 1000; now <= 5000; now += 1000) r.feed(now, 0xB0, 0x06, 0);
    r.feed(5251, 0xB0, 0x06, 0);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, r.alp.getState());
    TEST_ASSERT_FALSE(r.alp.currentEvent().active);
    TEST_ASSERT_TRUE(r.display.ut_alpHasLaserEvent());
    r.clearPaint();
    r.tick(6250);
    TEST_ASSERT_EQUAL_INT(0, r.display.testCanvas()->getFlushCount());
    r.tick(6251);
    TEST_ASSERT_FALSE(r.display.ut_alpHasLaserEvent());
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.clearPaint();
    r.tick(9251);
    r.scan();
}

void test_explicit_restore_expires_disconnected_tail_and_clears_laser_text() {
    Runtime r(false, 3);
    r.live();
    r.silence();
    r.clearPaint();
    mockMillis = 6211;
    TEST_ASSERT_TRUE(r.pipeline.restoreCurrentOwner(mockMillis));
    r.scan();
}

void test_connection_changes_preserve_live_and_persisted_alp_then_restore_current_owner() {
    Runtime r(false, 3);
    r.live();
    r.ble.setConnected(true);
    TEST_ASSERT_TRUE(r.pipeline.restoreCurrentOwner(220));
    TEST_ASSERT_TRUE(r.display.ut_alpHasLaserEvent());
    r.silence();
    r.ble.setConnected(false);
    TEST_ASSERT_TRUE(r.pipeline.restoreCurrentOwner(3300));
    TEST_ASSERT_EQUAL_STRING("PL3", r.display.ut_alpFreqText());
    r.clearPaint();
    r.tick(6211);
    r.scan();
    r.clearPaint();
    r.ble.setConnected(true);
    r.pipeline.handleParsed(6212);
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_EQUAL_STRING("0", r.display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_EQUAL(BAND_NONE, frequencyBand);
    r.clearPaint();
    r.ble.setConnected(false);
    r.pipeline.handleParsed(6213);
    r.scan();
}

void test_connected_alp_end_returns_to_resting_not_scan() {
    Runtime r(true);
    r.live();
    r.silence();
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, r.mode);
    TEST_ASSERT_EQUAL_INT(1, r.display.testCanvas()->getFlushCount());
    TEST_ASSERT_EQUAL_STRING("0", r.display.ut_elementCaches().topCounter.lastText);
    TEST_ASSERT_EQUAL(BAND_NONE, frequencyBand);
    TEST_ASSERT_FALSE(r.display.ut_alpFreqOverride());
}

void test_stale_v1_rows_cannot_appear_or_speak_while_disconnected() {
    Runtime r(false, 3);
    r.settings.mutableSettings().muteVoiceIfVolZero = false;
    const uint8_t row[]{0xAA, 0xD8, 0xEA, 0x43, 8, 0x11, 0x5E, 0x56, 0xA0, 0, 0x24, 0x80};
    std::vector<uint8_t> packet(row, row + sizeof(row));
    uint8_t checksum = 0;
    for (uint8_t byte : packet) checksum += byte;
    packet.push_back(checksum); packet.push_back(0xAB);
    TEST_ASSERT_TRUE(r.parser.parse(packet.data(), packet.size(), 90));
    TEST_ASSERT_TRUE(r.parser.hasAlerts());
    r.persistence.setPersistedAlert(r.parser.getPriorityAlert());
    r.persistence.startPersistence(90);
    r.live();
    TEST_ASSERT_EQUAL_INT(0, r.display.ut_elementCaches().cards.lastDrawnCount);
    r.silence();
    TEST_ASSERT_EQUAL_INT(0, r.display.ut_elementCaches().cards.lastDrawnCount);
    r.clearPaint();
    r.tick(6211);
    r.scan();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_disconnected_alp_end_paints_scan_and_repeated_idle_ticks_do_not_repaint);
    RUN_TEST(test_disconnected_persisted_tail_expires_without_any_packet);
    RUN_TEST(test_scan_refreshes_real_alp_badge_without_repainting_primary_or_unchanged_status);
    RUN_TEST(test_scan_theme_change_repaints_once_then_returns_to_idle_noop);
    RUN_TEST(test_disconnected_blink_expiry_restores_scan_even_with_stealth_enabled);
    RUN_TEST(test_abnormal_listening_hold_and_tail_each_expire_without_packets);
    RUN_TEST(test_explicit_restore_expires_disconnected_tail_and_clears_laser_text);
    RUN_TEST(test_connection_changes_preserve_live_and_persisted_alp_then_restore_current_owner);
    RUN_TEST(test_connected_alp_end_returns_to_resting_not_scan);
    RUN_TEST(test_stale_v1_rows_cannot_appear_or_speak_while_disconnected);
    return UNITY_END();
}
