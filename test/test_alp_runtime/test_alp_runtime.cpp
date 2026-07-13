/**
 * ALP Runtime Module — Unit Tests
 *
 * Tests gun identification parsing, state machine transitions,
 * heartbeat detection, timeout handling, and checksum validation.
 *
 * All test data uses the 4-byte frame protocol with 7-bit checksum:
 *   checksum = (byte0 + byte1 + byte2) & 0x7F
 *
 * Uses testInjectBytes() to feed raw protocol data without a real UART.
 */

#include <unity.h>

#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/alp/alp_sd_logger.h"
#include "../../src/modules/gps/gps_publishers.cpp"
#include "../../src/modules/alp/alp_sd_logger.cpp"
#include "../../src/modules/alp/alp_runtime_module.cpp"
#include "../../src/modules/system/system_event_bus.h"

// ── Helpers ──────────────────────────────────────────────────────────

static void resetModule() {
    alpRuntimeModule = AlpRuntimeModule();
}

static void beginEnabled() {
    alpRuntimeModule.begin(true);
}

static void beginDisabled() {
    alpRuntimeModule.begin(false);
}

static void inject(const uint8_t* data, size_t len) {
    alpRuntimeModule.testInjectBytes(data, len);
}

static void processAt(uint32_t ms) {
    alpRuntimeModule.process(ms);
}

// ── Alert burst test data (6 x 4-byte frames = 24 bytes) ───────────
// Each frame: byte0 byte1 byte2 checksum
// Alert trigger: 98 00 E3 7B
// Gun fingerprint: CX 00 YY checksum (Frame 4 of the burst)

// PL3 ProLite: byte0=c8, gunCode=d5
static const uint8_t BURST_PL3[] = {
    0x98, 0x00, 0xE3, 0x7B,    // Alert trigger
    0xC8, 0x04, 0xFA, 0x46,    // Static config
    0xC9, 0x00, 0xEC, 0x35,    // Pre-jam marker
    0xC8, 0x00, 0xD5, 0x1D,    // GUN — PL3 ProLite
    0xCA, 0x00, 0xEC, 0x36,    // Constant
    0xC9, 0x46, 0x65, 0x74,    // Mid-jam marker
};

// DragonEye Compact: byte0=c8, gunCode=d6
static const uint8_t BURST_DRAGONEYE[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xC8, 0x00, 0xD6, 0x1E,    // GUN — DragonEye Compact
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// LTI TruSpeed LR: byte0=c9, gunCode=f5
static const uint8_t BURST_TRUSPEED[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xC9, 0x00, 0xF5, 0x3E,    // GUN — LTI TruSpeed LR
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Marksman Ultralyte (LID deploy): byte0=cd, gunCode=d6
static const uint8_t BURST_ULTRALYTE[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCD, 0x00, 0xD6, 0x23,    // GUN — Marksman Ultralyte (jam)
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Marksman Ultralyte (Detect frame): fingerprint is CD 0C 00 → (byte0=cd, byte1=0c)
// From live capture: Ultralyte fire, ALP in observe (detect-only) mode, April 2026
static const uint8_t BURST_ULTRALYTE_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCD, 0x0C, 0x00, 0x59,    // GUN — Ultralyte (observe): (CD, 0C)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// PL3 ProLite (Detect frame): fingerprint is C8 0D 00 → (byte0=c8, byte1=0d)
// From live capture: PL3 fire, ALP in Detect frame, April 2026
static const uint8_t BURST_PL3_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xC8, 0x0D, 0x00, 0x55,    // GUN — PL3 ProLite (observe): (C8, 0D)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// LTI TruSpeed LR (Detect frame): fingerprint is C9 0E 00 → (byte0=c9, byte1=0e)
// From live capture: TruSpeed fire, ALP in Detect frame, April 2026
static const uint8_t BURST_TRUSPEED_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xC9, 0x0E, 0x00, 0x57,    // GUN — TruSpeed LR (observe): (C9, 0E)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Laser Atlanta PL2 (Detect frame): fingerprint is CB 10 00 → (byte0=cb, byte1=10)
// From live capture: PL2 fire, ALP in Detect frame, April 2026
static const uint8_t BURST_PL2_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCB, 0x10, 0x00, 0x5B,    // GUN — Laser Atlanta PL2 (observe): (CB, 10)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Stalker LZ1 (Detect frame): fingerprint is CD 0D 00 → (byte0=cd, byte1=0d)
// From live capture: Stalker LZ1 fire, ALP in Detect frame, April 2026
static const uint8_t BURST_STALKER_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCD, 0x0D, 0x00, 0x5A,    // GUN — Stalker LZ1 (observe): (CD, 0D)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// DragonEye Compact (Detect frame): fingerprint is C8 11 00 → (byte0=c8, byte1=11)
// From live capture: DragonEye Compact fire, ALP in Detect frame, April 2026
static const uint8_t BURST_DRAGONEYE_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xC8, 0x11, 0x00, 0x59,    // GUN — DragonEye Compact (observe): (C8, 11)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Laser Ally (Detect frame): fingerprint is CD 10 00 → (byte0=cd, byte1=10)
// From live capture: Laser Ally fire, ALP in Detect frame, April 2026
static const uint8_t BURST_LASER_ALLY_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCD, 0x10, 0x00, 0x5D,    // GUN — Laser Ally (observe): (CD, 10)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Atlanta Stealth (Detect frame): fingerprint is CE 0C 00 → (byte0=ce, byte1=0c)
// From live capture: Atlanta Stealth fire, ALP in Detect frame, April 2026
static const uint8_t BURST_ATLANTA_DETECT[] = {
    0x98, 0x02, 0x00, 0x1A,    // Alert trigger (Detect frame)
    0xC8, 0x00, 0x04, 0x4C,    // Generic laser-detected (all guns)
    0xC9, 0x19, 0x00, 0x62,
    0xCE, 0x0C, 0x00, 0x5A,    // GUN — Atlanta Stealth (observe): (CE, 0C)
    0xCA, 0x19, 0x00, 0x63,
    0xC9, 0x19, 0x03, 0x65,
};

// Stalker LZ1: byte0=cd, gunCode=eb
static const uint8_t BURST_STALKER[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCD, 0x00, 0xEB, 0x38,    // GUN — Stalker LZ1
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Atlanta Stealth: byte0=ce, gunCode=eb
static const uint8_t BURST_ATLANTA[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCE, 0x00, 0xEB, 0x39,    // GUN — Atlanta Stealth
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Laser Ally: byte0=cd, gunCode=d7
static const uint8_t BURST_LASER_ALLY[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCD, 0x00, 0xD7, 0x24,    // GUN — Laser Ally
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Laser Atlanta PL2: byte0=cb, gunCode=eb
static const uint8_t BURST_PL2[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xCB, 0x00, 0xEB, 0x36,    // GUN — Laser Atlanta PL2
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Unknown gun: ff 00 ff (unknown byte0+gunCode)
static const uint8_t BURST_UNKNOWN[] = {
    0x98, 0x00, 0xE3, 0x7B,
    0xC8, 0x04, 0xFA, 0x46,
    0xC9, 0x00, 0xEC, 0x35,
    0xFF, 0x00, 0xFF, 0x7E,    // GUN — Unknown (valid checksum)
    0xCA, 0x00, 0xEC, 0x36,
    0xC9, 0x46, 0x65, 0x74,
};

// Heartbeat: single B0 00 E6 checksum
static const uint8_t HEARTBEAT_SINGLE[] = { 0xB0, 0x00, 0xE6, 0x16 };

// Heartbeat: paired B8 00 FE checksum
static const uint8_t HEARTBEAT_PAIRED[] = { 0xB8, 0x00, 0xFE, 0x36 };

// Discovery poll: 91 00 12 checksum
static const uint8_t DISCOVERY_POLL[] = { 0x91, 0x00, 0x12, 0x23 };

// Register write with FD terminator at byte2: D0 00 FD checksum
static const uint8_t REG_WRITE_FD[] = { 0xD0, 0x00, 0xFD, 0x4D };

// Register write with FD at byte2 (D3 variant): D3 00 FD checksum
static const uint8_t REG_WRITE_FD_D3[] = { 0xD3, 0x00, 0xFD, 0x50 };

// ── Test setup/teardown ──────────────────────────────────────────────

void setUp() {
    resetModule();
}

void tearDown() {}

// ── Checksum validation tests ────────────────────────────────────────

void test_checksum_calculation() {
    // Known good: B0 02 00 → checksum 0x32
    TEST_ASSERT_EQUAL(0x32, alpChecksum(0xB0, 0x02, 0x00));
    // Known good: 98 00 E3 → checksum 0x7B
    TEST_ASSERT_EQUAL(0x7B, alpChecksum(0x98, 0x00, 0xE3));
    // Known good: B0 01 00 → checksum 0x31
    TEST_ASSERT_EQUAL(0x31, alpChecksum(0xB0, 0x01, 0x00));
    // Known good: C9 1A 04 → checksum 0x67
    TEST_ASSERT_EQUAL(0x67, alpChecksum(0xC9, 0x1A, 0x04));
}

void test_last_valid_frame_ms_ignores_checksum_noise() {
    beginEnabled();

    const uint8_t noisyBytes[] = {0x98, 0x00, 0xE3, 0x00};
    inject(noisyBytes, sizeof(noisyBytes));
    processAt(1234);

    TEST_ASSERT_EQUAL_UINT32(0, alpRuntimeModule.lastValidFrameMs());
}

void test_last_valid_frame_ms_updates_on_valid_frame() {
    beginEnabled();

    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1234);

    TEST_ASSERT_EQUAL_UINT32(1234, alpRuntimeModule.lastValidFrameMs());
}

void test_checksum_validation_pass() {
    TEST_ASSERT_TRUE(alpValidateChecksum(0xB0, 0x02, 0x00, 0x32));
    TEST_ASSERT_TRUE(alpValidateChecksum(0x98, 0x00, 0xE3, 0x7B));
    TEST_ASSERT_TRUE(alpValidateChecksum(0x91, 0x00, 0x12, 0x23));
}

void test_checksum_validation_fail() {
    TEST_ASSERT_FALSE(alpValidateChecksum(0xB0, 0x02, 0x00, 0x33));  // Wrong checksum
    TEST_ASSERT_FALSE(alpValidateChecksum(0x98, 0x00, 0xE3, 0x00));  // Zero checksum
    TEST_ASSERT_FALSE(alpValidateChecksum(0xFF, 0xFF, 0xFF, 0xFF));  // All 0xFF
}

// ── begin() tests ────────────────────────────────────────────────────

void test_begin_disabled_stays_disabled() {
    beginDisabled();
    TEST_ASSERT_EQUAL(AlpState::OFF, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.isEnabled());
}

void test_begin_enabled_goes_idle() {
    beginEnabled();
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.isEnabled());
}

void test_process_noop_when_disabled() {
    beginDisabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::OFF, alpRuntimeModule.getState());
}

// ── Gun lookup table tests ───────────────────────────────────────────

void test_gun_lookup_pl3() {
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpLookupGun(0xC8, 0xD5));
}

void test_gun_lookup_dragoneye() {
    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpLookupGun(0xC8, 0xD6));
}

void test_gun_lookup_truspeed() {
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpLookupGun(0xC9, 0xF5));
}

void test_gun_lookup_pl2() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpLookupGun(0xCB, 0xEB));
}

void test_gun_lookup_ultralyte() {
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpLookupGun(0xCD, 0xD6));
}

// Detect-frame gun lookups — (byte0, byte1) fingerprint from CX YY 00 frames
void test_gun_lookup_ultralyte_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpLookupGunDetect(0xCD, 0x0C));
}

void test_gun_lookup_pl3_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpLookupGunDetect(0xC8, 0x0D));
}

void test_gun_lookup_pl2_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpLookupGunDetect(0xCB, 0x10));
}

void test_gun_lookup_truspeed_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpLookupGunDetect(0xC9, 0x0E));
}

void test_gun_lookup_dragoneye_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpLookupGunDetect(0xC8, 0x11));
}

void test_gun_lookup_laser_ally_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpLookupGunDetect(0xCD, 0x10));
}

void test_gun_lookup_atlanta_stealth_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpLookupGunDetect(0xCE, 0x0C));
}

void test_gun_lookup_stalker_detect() {
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpLookupGunDetect(0xCD, 0x0D));
}

void test_gun_lookup_detect_unknown() {
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpLookupGunDetect(0xFF, 0xFF));
}

void test_gun_lookup_stalker() {
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpLookupGun(0xCD, 0xEB));
}

void test_gun_lookup_laser_ally() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpLookupGun(0xCD, 0xD7));
}

void test_gun_lookup_atlanta_stealth() {
    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpLookupGun(0xCE, 0xEB));
}

void test_gun_lookup_unknown() {
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpLookupGun(0xFF, 0xFF));
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpLookupGun(0x00, 0x00));
}

// gunCode collision resolution: cd d6 = Ultralyte, c8 d6 = DragonEye
void test_gun_guncode_collision_d6_resolved_by_byte0() {
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpLookupGun(0xCD, 0xD6));
    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpLookupGun(0xC8, 0xD6));
}

// gunCode collision: cb eb = PL2, cd eb = Stalker, ce eb = Atlanta Stealth
void test_gun_guncode_collision_eb_resolved_by_byte0() {
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpLookupGun(0xCB, 0xEB));
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpLookupGun(0xCD, 0xEB));
    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpLookupGun(0xCE, 0xEB));
}

// ── Alert burst parsing (all 8 guns) ────────────────────────────────

void test_burst_identifies_pl3() {
    beginEnabled();
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(1000u, alpRuntimeModule.lastGunTimestampMs());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_burst_identifies_dragoneye() {
    beginEnabled();
    inject(BURST_DRAGONEYE, sizeof(BURST_DRAGONEYE));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_truspeed() {
    beginEnabled();
    inject(BURST_TRUSPEED, sizeof(BURST_TRUSPEED));
    processAt(3000);

    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_ultralyte() {
    beginEnabled();
    inject(BURST_ULTRALYTE, sizeof(BURST_ULTRALYTE));
    processAt(4000);

    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_ultralyte_observe() {
    beginEnabled();
    inject(BURST_ULTRALYTE_DETECT, sizeof(BURST_ULTRALYTE_DETECT));
    processAt(4500);

    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_truspeed_observe() {
    beginEnabled();
    inject(BURST_TRUSPEED_DETECT, sizeof(BURST_TRUSPEED_DETECT));
    processAt(4700);

    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_dragoneye_observe() {
    beginEnabled();
    inject(BURST_DRAGONEYE_DETECT, sizeof(BURST_DRAGONEYE_DETECT));
    processAt(5200);

    TEST_ASSERT_EQUAL(AlpGunType::DRAGONEYE_COMPACT, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_laser_ally_observe() {
    beginEnabled();
    inject(BURST_LASER_ALLY_DETECT, sizeof(BURST_LASER_ALLY_DETECT));
    processAt(5100);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_atlanta_stealth_observe() {
    beginEnabled();
    inject(BURST_ATLANTA_DETECT, sizeof(BURST_ATLANTA_DETECT));
    processAt(5000);

    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_stalker_observe() {
    beginEnabled();
    inject(BURST_STALKER_DETECT, sizeof(BURST_STALKER_DETECT));
    processAt(4900);

    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_pl2_observe() {
    beginEnabled();
    inject(BURST_PL2_DETECT, sizeof(BURST_PL2_DETECT));
    processAt(4800);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_pl3_observe() {
    beginEnabled();
    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(4600);

    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_stalker() {
    beginEnabled();
    inject(BURST_STALKER, sizeof(BURST_STALKER));
    processAt(5000);

    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_atlanta() {
    beginEnabled();
    inject(BURST_ATLANTA, sizeof(BURST_ATLANTA));
    processAt(6000);

    TEST_ASSERT_EQUAL(AlpGunType::ATLANTA_STEALTH, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_laser_ally() {
    beginEnabled();
    inject(BURST_LASER_ALLY, sizeof(BURST_LASER_ALLY));
    processAt(7000);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ALLY, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_identifies_pl2() {
    beginEnabled();
    inject(BURST_PL2, sizeof(BURST_PL2));
    processAt(8000);

    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.lastIdentifiedGun());
}

void test_burst_unknown_gun_still_alerts() {
    beginEnabled();
    inject(BURST_UNKNOWN, sizeof(BURST_UNKNOWN));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Heartbeat parsing ────────────────────────────────────────────────

void test_heartbeat_transitions_idle_to_listening() {
    beginEnabled();
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());

    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetHeartbeatCount());
}

void test_paired_heartbeat_counted() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    inject(HEARTBEAT_PAIRED, sizeof(HEARTBEAT_PAIRED));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetHeartbeatCount());
}

void test_discovery_poll_transitions_to_listening() {
    beginEnabled();
    inject(DISCOVERY_POLL, sizeof(DISCOVERY_POLL));
    processAt(500);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── Heartbeat timeout ────────────────────────────────────────────────

void test_heartbeat_timeout_returns_to_idle() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    processAt(1000 + AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
}

void test_heartbeat_keeps_listening_if_within_timeout() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    processAt(1000 + AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS - 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── Register write with FD terminator (teardown trigger) ─────────────

void test_fd_terminator_triggers_teardown_from_alert() {
    beginEnabled();

    // Enter ALERT_ACTIVE via alert burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Register write with FD terminator at byte2
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

void test_fd_terminator_d3_triggers_teardown() {
    beginEnabled();

    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // D3 variant with FD at byte2
    inject(REG_WRITE_FD_D3, sizeof(REG_WRITE_FD_D3));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

// ── Teardown timeout ─────────────────────────────────────────────────

void test_teardown_timeout_returns_to_listening() {
    beginEnabled();

    alpRuntimeModule.testSetState(AlpState::TEARDOWN);
    alpRuntimeModule.testSetLastHeartbeat(1000);

    processAt(1000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── ALERT_ACTIVE timeout (no 98 trigger re-arm) ──────────────────────
//
// While in ALERT_ACTIVE, the state machine expects periodic 98 XX XX
// trigger frames to confirm the laser event is ongoing. If none arrive
// within ALERT_ACTIVE_TIMEOUT_MS (15s), the module assumes the event
// has ended and transitions to TEARDOWN. TEARDOWN then times out after
// an additional 5s to LISTENING, closing the session. Together these
// two timeouts provide a 20s-worst-case session close for stuck
// alert-active states.

void test_alert_active_timeout_transitions_to_teardown() {
    beginEnabled();

    // Enter ALERT_ACTIVE via a fresh alert burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Advance past the ALERT_ACTIVE timeout with no new 98 trigger arriving
    processAt(1000 + AlpRuntimeModule::ALERT_ACTIVE_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

void test_alert_active_stays_before_timeout() {
    beginEnabled();

    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Tick just short of the timeout — must still be ALERT_ACTIVE
    processAt(1000 + AlpRuntimeModule::ALERT_ACTIVE_TIMEOUT_MS - 100);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
}

void test_alert_active_rearm_resets_timeout_window() {
    beginEnabled();

    // Initial alert burst at T=1000
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Second 98 trigger arrives just before the first timeout would fire.
    // lastAlertTriggerMs_ should re-arm to this newer timestamp, so a
    // subsequent process() just past (1000 + 15000) should NOT trigger
    // the timeout — the window now runs from the re-arm instead.
    const uint32_t rearmMs = 1000 + AlpRuntimeModule::ALERT_ACTIVE_TIMEOUT_MS - 500;
    const uint8_t rearm[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(rearm, sizeof(rearm));
    processAt(rearmMs);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Just past original-burst + timeout, but well within rearm + timeout.
    processAt(1000 + AlpRuntimeModule::ALERT_ACTIVE_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Now past rearm + timeout — should drop to TEARDOWN.
    processAt(rearmMs + AlpRuntimeModule::ALERT_ACTIVE_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

void test_alert_active_timeout_session_closes_after_teardown() {
    beginEnabled();

    // Open session via alert burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());

    // Advance past ALERT_ACTIVE timeout → TEARDOWN. The session stays
    // open internally for a possible re-arm, and the display projection
    // stays live throughout TEARDOWN — real laser engagements bounce
    // ALERT_ACTIVE↔TEARDOWN every few hundred ms. Display only clears
    // at SESSION_CLOSE (engagementEnd on TEARDOWN→LISTENING).
    processAt(1000 + AlpRuntimeModule::ALERT_ACTIVE_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);

    // Advance past TEARDOWN timeout → LISTENING. Session closes here.
    processAt(1000 + AlpRuntimeModule::ALERT_ACTIVE_TIMEOUT_MS
              + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 200);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().active);
}

// ── Checksum-based resync ───────────────────────────────────────────

void test_resync_discards_garbage_before_heartbeat() {
    beginEnabled();

    // 1 garbage byte + valid 4-byte heartbeat
    const uint8_t data[] = { 0xFF, 0xB0, 0x00, 0xE6, 0x16 };
    inject(data, sizeof(data));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetFrameErrors() > 0);
}

void test_resync_discards_garbage_before_alert_burst() {
    beginEnabled();

    // 2 garbage bytes + full 24-byte alert burst
    uint8_t data[2 + 24];
    data[0] = 0xAA;
    data[1] = 0xBB;
    memcpy(data + 2, BURST_PL3, 24);
    inject(data, sizeof(data));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());
}

// ── Bad checksum rejection ──────────────────────────────────────────

void test_bad_checksum_frame_rejected() {
    beginEnabled();

    // Frame with valid-looking bytes but wrong checksum
    const uint8_t bad[] = { 0xB0, 0x02, 0x00, 0x99 };  // correct cs would be 0x32
    inject(bad, sizeof(bad));
    processAt(1000);

    // Should still be IDLE — frame was rejected
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetHeartbeatCount());
}

// ── Noise window via consecutive bad checksums ──────────────────────

void test_consecutive_bad_checksums_trigger_noise_window() {
    beginEnabled();

    // First get to ALERT_ACTIVE
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Inject enough noise bytes to trigger NOISE_WINDOW
    // Need NOISE_CHECKSUM_THRESHOLD consecutive bad checksums
    // Each bad 4-byte group consumes 1 byte, so we need enough bytes
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); ++i) {
        noise[i] = 0xFF;  // All 0xFF won't pass checksum (sum=0x2FD, cs=0x7D, but byte3=0xFF)
    }
    inject(noise, sizeof(noise));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpState::NOISE_WINDOW, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetNoiseWindowCount());
}

void test_noise_window_ends_on_valid_frame() {
    beginEnabled();

    // Force into NOISE_WINDOW
    alpRuntimeModule.testSetState(AlpState::NOISE_WINDOW);
    alpRuntimeModule.testSetLastHeartbeat(1000);

    // Inject valid heartbeat — should exit noise, enter teardown
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
}

void test_noise_window_exit_preserves_live_session_into_teardown() {
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.currentSession().gun);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);

    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); ++i) {
        noise[i] = 0xFF;
    }
    inject(noise, sizeof(noise));
    processAt(3000);

    TEST_ASSERT_EQUAL(AlpState::NOISE_WINDOW, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetNoiseWindowCount());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);

    inject(hb_idle, sizeof(hb_idle));
    processAt(4000);

    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.currentSession().gun);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.currentEvent().gun);
}

// ── Snapshot ─────────────────────────────────────────────────────────

void test_snapshot_reflects_state() {
    beginEnabled();
    inject(BURST_STALKER, sizeof(BURST_STALKER));
    processAt(5000);

    AlpStatus status = alpRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, status.state);
    TEST_ASSERT_EQUAL(AlpGunType::STALKER_LZ1, status.lastGun);
    TEST_ASSERT_EQUAL(5000u, status.lastGunTimestampMs);
    TEST_ASSERT_EQUAL(1u, status.statusBurstCount);
    TEST_ASSERT_TRUE(status.uartActive);
}

void test_snapshot_default_values() {
    beginEnabled();
    AlpStatus status = alpRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL(AlpState::IDLE, status.state);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, status.lastGun);
    TEST_ASSERT_EQUAL(0u, status.statusBurstCount);
    TEST_ASSERT_EQUAL(0u, status.heartbeatCount);
    TEST_ASSERT_FALSE(status.uartActive);
}

// ── isAlertActive() ──────────────────────────────────────────────────

void test_is_alert_active_during_alert() {
    beginEnabled();
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_TRUE(alpRuntimeModule.isAlertActive());
}

void test_is_not_alert_active_during_listening() {
    beginEnabled();
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_FALSE(alpRuntimeModule.isAlertActive());
}

// ── Gun name strings ─────────────────────────────────────────────────

void test_gun_names_not_null() {
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::UNKNOWN));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::PL3_PROLITE));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::DRAGONEYE_COMPACT));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::LTI_TRUSPEED_LR));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::LASER_ATLANTA_PL2));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::MARKSMAN_ULTRALYTE));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::STALKER_LZ1));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::LASER_ALLY));
    TEST_ASSERT_NOT_NULL(alpGunName(AlpGunType::ATLANTA_STEALTH));
}

void test_state_names_not_null() {
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::OFF));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::IDLE));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::LISTENING));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::ALERT_ACTIVE));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::NOISE_WINDOW));
    TEST_ASSERT_NOT_NULL(alpStateName(AlpState::TEARDOWN));
}

// ── Multiple alert bursts ───────────────────────────────────────────

void test_sequential_bursts_update_gun() {
    beginEnabled();

    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());

    inject(BURST_ULTRALYTE, sizeof(BURST_ULTRALYTE));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Full lifecycle: heartbeat → burst → teardown → listening ─────────

void test_full_alert_lifecycle() {
    beginEnabled();

    // 1. Heartbeats → LISTENING
    inject(HEARTBEAT_SINGLE, sizeof(HEARTBEAT_SINGLE));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // 2. Alert burst → ALERT_ACTIVE + gun ID
    inject(BURST_TRUSPEED, sizeof(BURST_TRUSPEED));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());

    // 3. FD terminator → TEARDOWN
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());

    // 4. Teardown timeout → LISTENING
    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
}

// ── Alert trigger as standalone frame ────────────────────────────────

void test_alert_trigger_standalone() {
    beginEnabled();

    // Just the alert trigger frame, no burst following
    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(1000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
    // No gun identified yet — gun comes in a later frame
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
}

// ── Detect-frame alert trigger (98 02 00) ───────────────────────────

void test_detect_trigger_alert_98_02_00() {
    beginEnabled();

    // Detect-frame alert: 98 02 00 (checksum = (98+02+00)&7F = 1A)
    const uint8_t detect_alert[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(detect_alert, sizeof(detect_alert));
    processAt(1000);

    // Should transition to ALERT_ACTIVE — Detect frame alert trigger
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_detect_trigger_rearm_increments_burst_count() {
    beginEnabled();

    // Already in ALERT_ACTIVE from LID-deploy trigger
    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // 98 02 00 while already ALERT_ACTIVE — re-arm counts as a new burst
    const uint8_t detect[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(detect, sizeof(detect));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Other 98 XX YY status frames (not alert triggers) ───────────────

void test_status_frame_98_other() {
    beginEnabled();

    // Some other 98 frame that isn't 00 E3 or 02 00
    // 98 04 10 — checksum = (98+04+10)&7F = 2C
    const uint8_t status[] = { 0x98, 0x04, 0x10, 0x2C };
    inject(status, sizeof(status));
    processAt(1000);

    // Should transition to LISTENING (not ALERT_ACTIVE)
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetStatusBurstCount());
}

// ── Heartbeat byte1 alert detection ─────────────────────────────────

void test_heartbeat_byte1_alert_transitions_to_alert_active() {
    beginEnabled();

    // Get to LISTENING with an idle heartbeat first
    // B0 02 00 — checksum = (B0+02+00)&7F = 32
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // B0 01 00 — byte1=01 means laser detected
    // checksum = (B0+01+00)&7F = 31
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);

    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetAlertDetectedViaHb());
    TEST_ASSERT_EQUAL(0x01, alpRuntimeModule.testGetLastHbByte1());
}

void test_heartbeat_byte1_idle_resolves_alert() {
    beginEnabled();

    // Get to LISTENING
    const uint8_t hb_idle1[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle1, sizeof(hb_idle1));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Alert via heartbeat byte1=01
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Back to idle via heartbeat byte1=03
    // checksum = (B0+03+00)&7F = 33
    const uint8_t hb_idle2[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_idle2, sizeof(hb_idle2));
    processAt(3000);

    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.testGetAlertDetectedViaHb());
}

void test_heartbeat_byte1_b8_does_not_trigger_alert() {
    beginEnabled();

    // B8 frames do NOT carry alert info — only B0 does
    // B8 01 00 — checksum = (B8+01+00)&7F = 39
    const uint8_t hb_b8[] = { 0xB8, 0x01, 0x00, 0x39 };
    inject(hb_b8, sizeof(hb_b8));
    processAt(1000);

    // Should be LISTENING (from IDLE), NOT ALERT_ACTIVE
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetStatusBurstCount());
    TEST_ASSERT_FALSE(alpRuntimeModule.testGetAlertDetectedViaHb());
}

void test_heartbeat_alert_repeated_01_no_double_trigger() {
    beginEnabled();

    // Idle heartbeat first
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    // First alert heartbeat
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // Second alert heartbeat (still 01) — should NOT increment burst count
    inject(hb_alert, sizeof(hb_alert));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_heartbeat_alert_reopens_when_listening_inherits_stale_01() {
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };

    // Establish LISTENING, then enter ALERT_ACTIVE via heartbeat.
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Resolve to TEARDOWN, then receive a TEARDOWN heartbeat that leaves
    // lastHbByte1_ parked at 01. This matches the stuck-Targeted captures
    // seen in alp_3/alp_14/alp_20 after session close.
    inject(hb_idle, sizeof(hb_idle));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    inject(hb_alert, sizeof(hb_alert));
    processAt(4000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0x01, alpRuntimeModule.testGetLastHbByte1());

    // TEARDOWN timeout closes the session and returns to LISTENING.
    processAt(4000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().active);

    // Old behavior stayed stuck in LISTENING because prevByte1 was still 01.
    // The next 01 heartbeat must reopen ALERT_ACTIVE and a fresh session.
    inject(hb_alert, sizeof(hb_alert));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetAlertDetectedViaHb());
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_heartbeat_alert_resume_ignores_same_tick_teardown_housekeeping() {
    beginEnabled();

    const uint8_t hbIdle[] = { 0xB0, 0x03, 0x00, 0x33 };
    const uint8_t hbAlert[] = { 0xB0, 0x01, 0x00, 0x31 };
    const uint8_t hbZero[] = { 0xB0, 0x00, 0x00, 0x30 };

    inject(hbIdle, sizeof(hbIdle));
    processAt(1000);
    inject(hbAlert, sizeof(hbAlert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // Resolve to TEARDOWN, then leave lastHbByte1_ parked at 01.
    inject(hbIdle, sizeof(hbIdle));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    inject(hbAlert, sizeof(hbAlert));
    processAt(4000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0x01, alpRuntimeModule.testGetLastHbByte1());

    // Reproduce the boot-4 churn boundary: TEARDOWN times out back to
    // LISTENING, but the same parser pass still contains queued 01
    // housekeeping followed by a 01->00 clear. This must NOT reopen a
    // phantom mode=01 session.
    inject(hbAlert, sizeof(hbAlert));
    inject(hbAlert, sizeof(hbAlert));
    inject(hbZero, sizeof(hbZero));
    processAt(4000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // A later process tick with steady 01 still reopens correctly.
    inject(hbAlert, sizeof(hbAlert));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_heartbeat_alert_edge_ignores_same_tick_teardown_housekeeping_bounce() {
    beginEnabled();

    const uint8_t hbIdle[] = { 0xB0, 0x03, 0x00, 0x33 };
    const uint8_t hbAlert[] = { 0xB0, 0x01, 0x00, 0x31 };
    const uint8_t hbZero[] = { 0xB0, 0x00, 0x00, 0x30 };

    inject(hbIdle, sizeof(hbIdle));
    processAt(1000);
    inject(hbAlert, sizeof(hbAlert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // Resolve to TEARDOWN on 01->00, then let TEARDOWN housekeeping park
    // lastHbByte1_ back at 01 without reopening the session.
    inject(hbZero, sizeof(hbZero));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    inject(hbAlert, sizeof(hbAlert));
    processAt(4000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(0x01, alpRuntimeModule.testGetLastHbByte1());

    // Reproduce alp_2-ded4f10f.csv @ 38029 ms: TEARDOWN times out back to
    // LISTENING, then queued housekeeping bounces 01->00->01 in the same
    // parser pass. Neither edge is a real new encounter.
    inject(hbZero, sizeof(hbZero));
    inject(hbAlert, sizeof(hbAlert));
    processAt(4000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);

    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetStatusBurstCount());

    // A later process tick with a fresh heartbeat alert still reopens.
    inject(hbAlert, sizeof(hbAlert));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(2u, alpRuntimeModule.testGetStatusBurstCount());
}

void test_heartbeat_alert_then_noise_from_listening() {
    beginEnabled();

    // Idle heartbeat → LISTENING
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Alert heartbeat → ALERT_ACTIVE
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Flood of noise → NOISE_WINDOW
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = 0xFF;
    inject(noise, sizeof(noise));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::NOISE_WINDOW, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.testGetNoiseWindowCount());
}

void test_noise_from_listening_with_hb_alert() {
    beginEnabled();

    // Get to LISTENING with idle heartbeat
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Simulate Detect frame: heartbeat byte1=01 sets flag, then immediate
    // noise flood (speaker alert) before we even process() again.
    // We need to put the alert heartbeat + noise in same inject.
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    // Process to register the alert heartbeat → ALERT_ACTIVE
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetAlertDetectedViaHb());

    // Now noise comes in
    uint8_t noise[64];
    for (size_t i = 0; i < sizeof(noise); ++i) noise[i] = 0xFF;
    inject(noise, sizeof(noise));
    processAt(3000);

    TEST_ASSERT_EQUAL(AlpState::NOISE_WINDOW, alpRuntimeModule.getState());
}

void test_snapshot_includes_hb_byte1() {
    beginEnabled();

    // Heartbeat with byte1=04
    // B0 04 00 — checksum = (B0+04+00)&7F = 34
    const uint8_t hb[] = { 0xB0, 0x04, 0x00, 0x34 };
    inject(hb, sizeof(hb));
    processAt(1000);

    AlpStatus status = alpRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL(0x04, status.lastHbByte1);
}

void test_snapshot_includes_event_direction() {
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);

    AlpStatus status = alpRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(status.laserDirection));
    TEST_ASSERT_EQUAL(0x01, status.directionSampleByte1);
}

void test_teardown_clears_alert_flag() {
    beginEnabled();

    // Alert via heartbeat
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.testGetAlertDetectedViaHb());

    // Resolve via heartbeat byte1=02
    const uint8_t hb_idle2[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle2, sizeof(hb_idle2));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());

    // Teardown timeout → LISTENING, flag should be cleared
    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.testGetAlertDetectedViaHb());
}

// ── Gun cleared on new alert entry (stale gun fix) ─────────────

void test_new_alert_clears_stale_gun_via_98_trigger() {
    beginEnabled();

    // First alert: identify PL3 via burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());

    // Resolve: FD terminator → TEARDOWN → timeout → LISTENING
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    processAt(2000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Second alert: 98 trigger only — no gun frame follows
    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Gun must be UNKNOWN — the previous PL3 must not bleed through
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.lastGunTimestampMs());
}

void test_new_alert_clears_stale_gun_via_heartbeat() {
    beginEnabled();

    // First alert: identify TruSpeed via burst
    inject(BURST_TRUSPEED, sizeof(BURST_TRUSPEED));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpGunType::LTI_TRUSPEED_LR, alpRuntimeModule.lastIdentifiedGun());

    // Resolve via FD terminator → TEARDOWN → timeout → LISTENING
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    processAt(2000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Idle heartbeat to seed lastHbByte1 for the transition detection
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_idle, sizeof(hb_idle));
    processAt(9000);

    // Second alert via heartbeat byte1=01 — no gun frame
    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // Gun must be UNKNOWN — the previous TruSpeed must not bleed through
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.lastIdentifiedGun());
}

void test_new_alert_identifies_fresh_gun_after_clear() {
    beginEnabled();

    // First alert: identify PL3
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.lastIdentifiedGun());

    // Resolve → LISTENING
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(2000);
    processAt(2000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Second alert: full Ultralyte burst — should show Ultralyte, not PL3
    inject(BURST_ULTRALYTE, sizeof(BURST_ULTRALYTE));
    processAt(10000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, alpRuntimeModule.lastIdentifiedGun());
}

// Regression for alp_18.csv in-engagement teardown re-arm.
//
// Observed: PL2 identified at 36.768s via Detect-frame burst → gun shown.
// At 46.352s the heartbeat byte1 dropped 01→02 (alert resolve-to-TEARDOWN)
// and — in the same millisecond — a 98 02 00 re-arm fired, pushing state
// back to ALERT_ACTIVE. Pre-fix, the TEARDOWN→ALERT_ACTIVE transitionTo
// wiped lastGun_, leaving the remaining ~20s of the same engagement
// showing generic "LASER" on the display.
//
// Fix: narrow the clear guard to (LISTENING|IDLE) → ALERT_ACTIVE only.
// Assertion: PL2 must still be identified after the re-arm.
void test_gun_persists_through_teardown_rearm_cycle() {
    beginEnabled();

    // Seed LISTENING with a 02 idle heartbeat (also seeds lastHbByte1_=02
    // so the later 01 transition registers as a real alert flip).
    const uint8_t hb_idle_02_a[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle_02_a, sizeof(hb_idle_02_a));
    processAt(8000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // First alert: Detect-frame PL2 burst → ALERT_ACTIVE + PL2 identified.
    // (Fresh LISTENING → ALERT_ACTIVE transition; Rev 6 clear fires as
    // expected and is immediately re-populated by the CB 10 00 gun frame.)
    inject(BURST_PL2_DETECT, sizeof(BURST_PL2_DETECT));
    processAt(37000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2,
                      alpRuntimeModule.lastIdentifiedGun());

    // Within the same engagement: byte1 flips to 01 (alert confirm via HB).
    // State stays ALERT_ACTIVE — this is the "still firing" heartbeat.
    const uint8_t hb_alert_01[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert_01, sizeof(hb_alert_01));
    processAt(45000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // byte1 01→02: ALERT_ACTIVE → TEARDOWN (end-of-hold edge).
    const uint8_t hb_idle_02_b[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle_02_b, sizeof(hb_idle_02_b));
    processAt(46000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    // Gun must survive the ALERT_ACTIVE → TEARDOWN edge.
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2,
                      alpRuntimeModule.lastIdentifiedGun());

    // 98 02 00 re-fires during TEARDOWN → TEARDOWN → ALERT_ACTIVE (in-engagement
    // re-arm — this is the edge that used to wipe the gun).
    const uint8_t observe_trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(observe_trigger, sizeof(observe_trigger));
    processAt(46500);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());

    // THE ASSERTION. Before the fix this was UNKNOWN.
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2,
                      alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(37000u, alpRuntimeModule.lastGunTimestampMs());
}

// ── AlertSession / V1-shape display projection ──────────────────────
//
// These tests validate the session layer that projects the parser's
// internal state machine into the V1-shape accessors the display
// consumes. The display contract is intentionally live-only: if the
// ALP is not actively alerting, the display accessors must clear even
// if the internal session stays open through a short re-arm gap.

void test_session_closed_by_default() {
    beginEnabled();
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.currentEvent().gun);
}

// ── ownsLaserDisplay() — V1 laser suppression gate ──────────────────
//
// The display pipeline asks this one question: "should V1's BAND_LASER
// alerts be suppressed because ALP is handling laser?" Contract:
//   true  — ALP enabled AND connected (any state except OFF / IDLE)
//   false — ALP disabled, or state is OFF (never started) / IDLE (UART
//           timeout drifted the module into silence). V1 laser falls
//           through as a backup detection channel.

void test_owns_laser_display_false_when_disabled() {
    // Module never begun → OFF → does not own.
    TEST_ASSERT_FALSE(alpRuntimeModule.ownsLaserDisplay());

    // Explicit begin(disabled) → OFF → does not own.
    alpRuntimeModule.begin(false, nullptr);
    TEST_ASSERT_FALSE(alpRuntimeModule.ownsLaserDisplay());
}

void test_owns_laser_display_true_when_listening() {
    // Enabled + first valid heartbeat → LISTENING → owns.
    beginEnabled();
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.ownsLaserDisplay());
}

void test_owns_laser_display_true_during_alert() {
    beginEnabled();
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.ownsLaserDisplay());
}

void test_owns_laser_display_false_when_idle_after_timeout() {
    // LISTENING → IDLE via heartbeat timeout → no longer owns.
    // V1 laser should pass through because the ALP has gone silent.
    beginEnabled();
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_TRUE(alpRuntimeModule.ownsLaserDisplay());

    // Drift past the heartbeat watchdog — IDLE.
    processAt(1000 + AlpRuntimeModule::HEARTBEAT_TIMEOUT_MS + 500);
    TEST_ASSERT_EQUAL(AlpState::IDLE, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.ownsLaserDisplay());
}

void test_session_opens_on_fresh_alert_from_listening() {
    beginEnabled();

    // LISTENING via heartbeat
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());

    // Fresh alert: full PL3 burst
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_FALSE(s.isWarmUp);
    TEST_ASSERT_EQUAL(2000u, s.startMs);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, s.gun);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.currentEvent().gun);
}

void test_session_direction_front_latches_on_targeted_heartbeat() {
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t hb_alert[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert, sizeof(hb_alert));
    processAt(2000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(s.direction));
    TEST_ASSERT_EQUAL(0x01, s.directionSampleByte1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
}

void test_session_direction_rear_latches_on_dli_heartbeat() {
    // Per docs/ALP_PROTOCOL_EVIDENCE.md § Direction anchoring, the
    // direction classifier samples the first non-00 B0 heartbeat *after*
    // a GUN_ID frame. Inject a PL3 gun-ID burst to satisfy the anchor,
    // then sample a DLI (byte1=03) heartbeat which must latch REAR.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    // Gun-ID burst opens the session AND anchors direction sampling.
    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(2000);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.lastIdentifiedGun());

    const uint8_t hb_rear[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_rear, sizeof(hb_rear));
    processAt(3000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            static_cast<uint8_t>(s.direction));
    TEST_ASSERT_EQUAL(0x03, s.directionSampleByte1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
}

void test_session_direction_rear_ignores_pre_gun_sample_until_anchored() {
    // Lock the gun-anchor asymmetry down: REAR heartbeats that arrive
    // before any gun-ID must not classify direction, but the first REAR
    // heartbeat after gun identification must still latch REAR.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t detect_trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(detect_trigger, sizeof(detect_trigger));
    processAt(2000);

    const uint8_t hb_rear[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_rear, sizeof(hb_rear));
    processAt(3000);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN,
                      alpRuntimeModule.currentSession().gun);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            static_cast<uint8_t>(alpRuntimeModule.currentSession().direction));
    TEST_ASSERT_EQUAL(0x00, alpRuntimeModule.currentSession().directionSampleByte1);

    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(4000);

    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            static_cast<uint8_t>(alpRuntimeModule.currentSession().direction));

    inject(hb_rear, sizeof(hb_rear));
    processAt(5000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            static_cast<uint8_t>(s.direction));
    TEST_ASSERT_EQUAL(0x03, s.directionSampleByte1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
}

void test_unknown_direction_pdc_heartbeat_keeps_live_alert_visible() {
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t detect_trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(detect_trigger, sizeof(detect_trigger));
    processAt(2000);

    const uint8_t hb_pdc_only[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_pdc_only, sizeof(hb_pdc_only));
    processAt(3000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            static_cast<uint8_t>(s.direction));
    TEST_ASSERT_EQUAL(0x00, s.directionSampleByte1);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
}

void test_unknown_direction_live_alert_keeps_display_projection_with_gun() {
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(2000);

    const uint8_t hb_pdc_only[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_pdc_only, sizeof(hb_pdc_only));
    processAt(3000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    // Seed heartbeat is now B0 03 (DLI) instead of the mislabeled B0 02.
    // modeAtOpen captures lastHbByte1_ at freshEngagement.
    TEST_ASSERT_EQUAL(0x03, s.modeAtOpen);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, s.gun);

    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.currentEvent().gun);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
    TEST_ASSERT_EQUAL(0x00, s.directionSampleByte1);
}

void test_latched_rear_session_ignores_later_pdc_heartbeat_for_display() {
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    // Gun-ID burst opens the session AND anchors direction sampling per
    // docs/ALP_PROTOCOL_EVIDENCE.md § Direction anchoring. Must precede
    // the REAR heartbeat.
    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(2000);

    const uint8_t hb_rear[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_rear, sizeof(hb_rear));
    processAt(3000);

    const uint8_t hb_pdc_only[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_pdc_only, sizeof(hb_pdc_only));
    processAt(4000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            static_cast<uint8_t>(s.direction));
    TEST_ASSERT_EQUAL(0x03, s.directionSampleByte1);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, s.gun);

    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, alpRuntimeModule.currentEvent().gun);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
}

void test_session_direction_relatches_front_over_rear() {
    // Direction latch is sticky EXCEPT against byte1=01 (Targeted), which
    // is the manufacturer's unambiguous FRONT signal (Targeted = RED LED =
    // laser in front). A REAR latch established by DLI/LID activity must
    // be overridden when the ALP eventually reports Targeted — this is
    // the on-device multi-phase session edge case documented in §5.5 of
    // docs/plans/ALP_LASER_DIRECTION_CLASSIFIER_20260418.md, reproduced
    // on-device in alp_2-f6e0ca7e.csv (PL3 front shot misclassified REAR).
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    // Gun-ID burst opens the session AND anchors direction sampling per
    // docs/ALP_PROTOCOL_EVIDENCE.md § Direction anchoring.
    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(2000);

    const uint8_t hb_rear[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_rear, sizeof(hb_rear));
    processAt(3000);

    // Verify REAR latched before Targeted arrives.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::REAR),
                            static_cast<uint8_t>(alpRuntimeModule.currentSession().direction));

    const uint8_t hb_front[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_front, sizeof(hb_front));
    processAt(4000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(s.direction));
    TEST_ASSERT_EQUAL(0x01, s.directionSampleByte1);
}

void test_session_survives_teardown_rearm_cycle() {
    // alp_18-shape: gun identified once, then an in-engagement TEARDOWN↔ALERT
    // cycle must NOT close the session. Display accessors clear during the
    // TEARDOWN gap and resume when live detection returns.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    inject(BURST_PL2_DETECT, sizeof(BURST_PL2_DETECT));
    processAt(2000);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.currentEvent().gun);

    // byte1 01 → 02 drop: ALERT_ACTIVE → TEARDOWN
    const uint8_t hb_alert_01[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hb_alert_01, sizeof(hb_alert_01));
    processAt(3000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(alpRuntimeModule.currentSession().direction));
    const uint8_t hb_idle_02[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hb_idle_02, sizeof(hb_idle_02));
    processAt(4000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());
    // Session stays open internally AND display projection stays live
    // during TEARDOWN. Real PL3 engagements bounce ALERT_ACTIVE↔TEARDOWN
    // every few hundred ms as laser pulses on/off; clearing the display
    // on each TEARDOWN dropped the alert 115 ms into a 5 s engagement on
    // device (alp_6-61aa53a8.csv @ 43401). The display event stays active
    // across the full session lifetime until SESSION_CLOSE.
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.currentEvent().gun);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));

    // Re-arm via 98 02 00 while in TEARDOWN
    const uint8_t observe_trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(observe_trigger, sizeof(observe_trigger));
    processAt(5000);
    TEST_ASSERT_EQUAL(AlpState::ALERT_ACTIVE, alpRuntimeModule.getState());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.currentEvent().gun);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(alpRuntimeModule.currentSession().direction));
    TEST_ASSERT_EQUAL(1u, alpRuntimeModule.currentSession().rearmCount);
}

void test_session_closes_on_teardown_to_listening() {
    // Full engagement: alert → teardown → timeout → LISTENING → session closed.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());

    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(3000);
    TEST_ASSERT_EQUAL(AlpState::TEARDOWN, alpRuntimeModule.getState());

    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // Session closed. Display accessors must return the "no event" values.
    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_TRUE(s.endMs > 0);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.currentEvent().gun);

    // But lastIdentifiedGun() still reflects the protocol-level cache,
    // for SD logger / diagnostics. The display never reads this.
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.lastIdentifiedGun());
}

// Regression for bug #3: lastGun leaking onto the display during LISTENING.
// Before the session layer, the display read lastIdentifiedGun() directly
// and could render a stale gun name from the previous engagement. With
// session gating, currentEvent().gun returns UNKNOWN between engagements even
// though lastGun_ persists internally.
void test_event_gun_unknown_between_engagements_even_with_stale_lastgun() {
    beginEnabled();

    // First engagement: identify PL3 then close cleanly.
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    inject(BURST_PL3, sizeof(BURST_PL3));
    processAt(2000);
    inject(REG_WRITE_FD, sizeof(REG_WRITE_FD));
    processAt(3000);
    processAt(3000 + AlpRuntimeModule::TEARDOWN_TIMEOUT_MS + 100);
    TEST_ASSERT_EQUAL(AlpState::LISTENING, alpRuntimeModule.getState());

    // lastGun_ is still populated (protocol-level cache). Display must not
    // see it — currentEvent().gun is gated on an active session.
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.currentEvent().gun);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
}

// ── Warm-Up suppression (bug #1) ────────────────────────────────────

void test_warm_up_flagged_when_preamble_in_window() {
    // Canonical cold-boot shape: first heartbeat, F0 preamble at +2s,
    // 98 02 00 trigger shortly after. The session must be flagged
    // Warm-Up and suppressed from display.
    beginEnabled();

    // First valid frame: idle heartbeat. Sets firstFrameMs_ = 1000.
    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    TEST_ASSERT_EQUAL(1000u, alpRuntimeModule.testGetFirstFrameMs());
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetWarmUpPreambleMs());

    // F0 preamble within the 5s window.
    // F0 03 00 — checksum = (F0+03+00)&7F = 73
    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(3000);
    TEST_ASSERT_EQUAL(3000u, alpRuntimeModule.testGetWarmUpPreambleMs());

    // Detect trigger. Opens a session inside the envelope.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(3100);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_TRUE(s.isWarmUp);
    // Display sees nothing — this is the fix for bug #1.
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, alpRuntimeModule.currentEvent().gun);
}

void test_warm_up_unflagged_when_real_gun_identified() {
    // Safety release: a real gun ID during the Warm-Up window must
    // un-declare the session as Warm-Up. The ALP's Warm-Up never
    // emits CX gun frames, so a gun ID is pathognomonic for real.
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);
    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(3000);

    // Now a real PL2 Detect-frame burst arrives inside the envelope.
    // The trigger opens a Warm-Up-flagged session, then the CB 10 00
    // gun frame un-flags it.
    inject(BURST_PL2_DETECT, sizeof(BURST_PL2_DETECT));
    processAt(4000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_FALSE(s.isWarmUp);  // Released by gun ID
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, s.gun);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, alpRuntimeModule.currentEvent().gun);
}

void test_warm_up_not_flagged_without_preamble() {
    // No F0/A8 ever seen → session is never flagged Warm-Up even at cold boot.
    // Seed heartbeat with B0 03 (DLI mode) — the normal quiescent heartbeat.
    // Using B0 02 (Warm-Up) here would trigger the Warm-Up-mode clause, but
    // that isn't what this test is exercising.
    beginEnabled();

    // B0 03 (DLI) is the normal quiescent heartbeat — lastHbByte1_=0x03
    // does not trip the Warm-Up-mode clause (only 0x02 does).
    const uint8_t hb_dli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_dli, sizeof(hb_dli));
    processAt(1000);

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(2000);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_FALSE(s.isWarmUp);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
}

void test_warm_up_preamble_after_window_ignored() {
    // F0 arriving AFTER the 5s preamble window does not arm Warm-Up.
    // (In practice F0 only ever arrives at cold boot; this guards
    // against spurious late F0 frames or unexpected protocol drift.)
    beginEnabled();

    const uint8_t hb_dli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_dli, sizeof(hb_dli));
    processAt(1000);

    // F0 at +6s — outside the 5s window.
    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(7000);
    TEST_ASSERT_EQUAL(0u, alpRuntimeModule.testGetWarmUpPreambleMs());

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(8000);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
}

void test_warm_up_envelope_expires_after_35s() {
    // Preamble observed at boot, but the alert trigger arrives after
    // the 35s envelope. Session must NOT be flagged Warm-Up.
    beginEnabled();

    const uint8_t hb_dli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_dli, sizeof(hb_dli));
    processAt(1000);

    const uint8_t f0_preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(f0_preamble, sizeof(f0_preamble));
    processAt(3000);

    // Alert at +40s — envelope is 35s from firstFrameMs_ (1000) so
    // cutoff is 36000. 41000 > 36000 → not flagged Warm-Up.
    // Re-seed a DLI heartbeat just before the trigger so lastHbByte1_
    // is 0x03 (not 0x02 from any prior Warm-Up probe or noise) at the
    // moment of freshEngagement.
    const uint8_t hb_dli_late[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hb_dli_late, sizeof(hb_dli_late));
    processAt(40000);

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(41000);

    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_TRUE(alpRuntimeModule.hasLaserEvent());
}

void test_warm_up_flagged_when_no_heartbeat_yet() {
    // Regression for on-device boot bug (log: ts=4548, mode=FF, warmUp=0).
    // The ALP cold-booted directly into emitting 98 trigger frames before
    // any B0 heartbeat or F0/A8 preamble was sent. The narrow preamble-
    // based envelope missed this as Warm-Up and the phantom laser event
    // reached the display. With the no-heartbeat clause, a session
    // opening while lastHbByte1_==0xFF is flagged Warm-Up automatically
    // — no real engagement can precede the ALP's first heartbeat.
    resetModule();  // must run with fresh lastHbByte1_==0xFF
    beginEnabled();

    // No heartbeat, no preamble — the ALP's very first frame is a trigger.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(4548);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_TRUE(s.isWarmUp);
    TEST_ASSERT_EQUAL_HEX8(0xFF, s.modeAtOpen);
    // Display must see nothing — isWarmUp suppresses the V1-shape accessors.
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
}

void test_warm_up_flagged_when_heartbeat_mode_is_warm_up() {
    // Regression for on-device bug at boot+54793ms (alp_6-61aa53a8.csv):
    // Long after boot (52s past firstFrameMs, past the 35s preamble
    // envelope window and with a valid heartbeat history so lastHbByte1_
    // != 0xFF), the ALP entered an in-life Warm-Up cycle. The log shows
    // HEARTBEAT B0 02 00 32 (Warm-Up mode) immediately followed by a 98
    // 02 00 1A trigger, and the phantom laser reached the display.
    // The third Warm-Up detector (lastHbByte1_==0x02) catches this.
    resetModule();
    beginEnabled();

    // Seed a DLI heartbeat well past the 35s envelope so
    // (a) noHeartbeatYet is false (we've seen a heartbeat),
    // (b) inEnvelope is false (firstFrameMs_ is far in the past),
    // (c) lastHbByte1_ != 0x02 before the Warm-Up heartbeat arrives.
    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(40000);

    // Later: ALP drops into Warm-Up and emits a trigger in the same tick.
    const uint8_t hbWarmUp[] = { 0xB0, 0x02, 0x00, 0x32 };
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(hbWarmUp, sizeof(hbWarmUp));
    inject(trigger, sizeof(trigger));
    processAt(54793);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_TRUE(s.isWarmUp);
    TEST_ASSERT_EQUAL_HEX8(0x02, s.modeAtOpen);
    // Display must see nothing — isWarmUp suppresses the laser event.
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
}

void test_warm_up_flagged_when_heartbeat_mode_is_idle() {
    // Regression for alp_2-2450a468.csv @ 138473 ms: a bare 98 02 00
    // landed while lastHbByte1_==00, opened a non-warm-up session, and
    // immediately painted a generic LASER with no gun or direction.
    resetModule();
    beginEnabled();

    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(40000);

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(40100);

    const auto& s = alpRuntimeModule.currentSession();
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_TRUE(s.isWarmUp);
    TEST_ASSERT_EQUAL_HEX8(0x00, s.modeAtOpen);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
}

void test_display_window_stays_open_across_byte1_blip_to_idle() {
    // Regression for on-device PL3 engagement (alp_6-61aa53a8.csv @ 43401):
    // Real PL3 laser. SESSION_OPEN + DISPLAY_WINDOW_OPEN at 43401 with
    // byte1=01 (Targeted). 115 ms later, the next B0 heartbeat reports
    // byte1=00 (pulse gap) — the ALP routinely drops back to IDLE between
    // laser pulses. That heartbeat transitioned the state
    // ALERT_ACTIVE→TEARDOWN, and the gated event cleared — display
    // dropped the laser 115 ms into a ~5 s engagement. The session
    // (session_.active) remained true until SESSION_CLOSE at +5130 ms;
    // the event must remain active throughout TEARDOWN until the real
    // engagementEnd (TEARDOWN→LISTENING).
    resetModule();
    beginEnabled();

    // Warm-up defeat: get through any Warm-Up envelope first.
    // Seed lastHbByte1_ to 0x03 (DLI, a non-Warm-Up mode) by injecting
    // an F0 03 heartbeat well past WARM_UP_ENVELOPE_MS so the session
    // will be treated as a real engagement.
    const uint8_t hbDli[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(hbDli, sizeof(hbDli));
    processAt(40000);

    // Real PL3 engagement: 98 trigger enters ALERT_ACTIVE.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(43401);

    // Targeted heartbeat + GUN_ID confirm PL3 ProLite engagement.
    const uint8_t hbTargeted[] = { 0xB0, 0x01, 0x00, 0x31 };
    const uint8_t gunPl3[] = { 0xC8, 0x0D, 0x00, 0x55 };
    inject(hbTargeted, sizeof(hbTargeted));
    inject(gunPl3, sizeof(gunPl3));
    processAt(43402);

    // Display window must be open — real engagement in flight.
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);

    // Pulse gap: byte1 drops to 00, transitioning state to TEARDOWN.
    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(43516);

    // Display window must remain OPEN — session is still alive, TEARDOWN
    // is mid-engagement. This is the bug fix: previously event.active
    // gated only on ALERT_ACTIVE, which cut the display at the first
    // pulse gap.
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
}

void test_boot_self_test_does_not_promote_on_repeat_detect() {
    // Regression for on-device boot rear Warm-Up probe (alp_2-64991486
    // .csv @ 5063/8358 ms). The ALP's post-boot Warm-Up sequence emits
    // two 98 02 00 Detect-trigger frames with B0 02 → B0 04 heartbeats
    // between them:
    //   5063: 98 02 00  (opens session, warmUp=1 via no_heartbeat_yet)
    //   5144: B0 02 00  (Warm-Up mode heartbeat)
    //   5931: B0 04 00  (LID probe burst — 02→04 transition)
    //   8358: 98 02 00  (second Detect trigger)
    // Earlier revisions promoted this Warm-Up via a triggerCount>=2
    // heuristic. The heartbeat 02→04 transition is also a potential
    // promotion path, but it's gated by the boot envelope — within
    // WARM_UP_ENVELOPE_MS of firstFrameMs_ the transition is treated as
    // Warm-Up sequencing, not real engagement. The session must stay
    // suppressed through the entire boot Warm-Up.
    resetModule();
    beginEnabled();

    // No F0/A8 preamble observed in the boot trace — the first ALP
    // frame is the Detect trigger. firstFrameMs_ is set at the trigger.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(5063);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    // B0 02 (Warm-Up heartbeat) — no release, session stays suppressed.
    const uint8_t hbWarm[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hbWarm, sizeof(hbWarm));
    processAt(5144);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);

    // B0 04 (LID test burst during boot) — 02→04 transition. Inside
    // boot envelope the gate blocks release. Session stays suppressed.
    const uint8_t hbLid[] = { 0xB0, 0x04, 0x00, 0x34 };
    inject(hbLid, sizeof(hbLid));
    processAt(5931);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    // Second Detect-trigger in the same session — must NOT release.
    inject(trigger, sizeof(trigger));
    processAt(8358);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL_UINT32(2, alpRuntimeModule.currentSession().triggerCount);
}

void test_warmup_stays_suppressed_on_heartbeat_02_to_03_without_gun() {
    // Regression for alp_2-2450a468.csv @ 103302 / 205835 / 310873 ms:
    // a bare 98 02 00 opens a speculative session under byte1=02, then
    // byte1 transitions to 03 with no gun identification. That 02→03 edge
    // must NOT promote the session to a generic LASER.
    resetModule();
    beginEnabled();

    // Seed an early frame to set firstFrameMs_ so we're well past the
    // boot envelope by the time the session opens.
    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(1000);

    // Prime a byte1=02 heartbeat at rest, well past the boot envelope
    // (1000 + 35000 = 36000; we're at 108321).
    const uint8_t hbWarm[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hbWarm, sizeof(hbWarm));
    processAt(108321);

    // Session opens with byte1=02 at rest → isWarmUp=1 via warmUpMode.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(109072);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    // Heartbeat transitions 02 → 03 with no gun on the session. Stay
    // suppressed — DLI alone is not enough proof for a bare detect-trigger.
    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(109145);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
}

void test_warmup_released_on_heartbeat_02_to_04() {
    // Same contract as 02→03 but for the PDC & LID configuration: when
    // the ALP transitions from byte1=02 (rest/warm-up) to byte1=04 (LID
    // active, above speed limit) in a flagged-Warm-Up session past the
    // boot envelope, the flag must release. PDC & LID units never show
    // byte1=03, so this is the only heartbeat-edge promotion path for
    // them.
    resetModule();
    beginEnabled();

    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(1000);

    const uint8_t hbWarm[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hbWarm, sizeof(hbWarm));
    processAt(108321);

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(109072);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);

    const uint8_t hbLid[] = { 0xB0, 0x04, 0x00, 0x34 };
    inject(hbLid, sizeof(hbLid));
    processAt(109145);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
}

void test_warmup_targeted_blip_inside_boot_envelope_stays_suppressed() {
    // Protocol ref boot Warm-Up sequence includes a single ~800 ms
    // byte1=01 Targeted blip inside the Warm-Up envelope. That blip
    // must not release a speculative session to the display.
    resetModule();
    beginEnabled();

    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(1000);

    const uint8_t preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(preamble, sizeof(preamble));
    processAt(3000);

    const uint8_t hbWarm[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hbWarm, sizeof(hbWarm));
    processAt(5000);

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(5100);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    const uint8_t hbTargeted[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hbTargeted, sizeof(hbTargeted));
    processAt(5900);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
}

void test_warmup_released_on_targeted_heartbeat_from_idle_mode() {
    // If a speculative mode=00 detect-trigger is followed by byte1=01,
    // that's the ALP's universal targeted signal and must release the
    // display immediately once the parser is clearly past the boot
    // Warm-Up envelope.
    resetModule();
    beginEnabled();

    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(1000);

    inject(hbIdle, sizeof(hbIdle));
    processAt(40000);

    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(40100);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    const uint8_t hbTargeted[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hbTargeted, sizeof(hbTargeted));
    processAt(40150);

    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
}

void test_warmup_released_on_lid_deploy() {
    // 98 00 E3 LID-deploy is the ALP firing IR countermeasures — an
    // unambiguous real laser engagement. Any isWarmUp=1 flag held over
    // from SESSION_OPEN must release immediately on this frame.
    resetModule();
    beginEnabled();

    // Arm envelope + heartbeat history.
    const uint8_t preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(preamble, sizeof(preamble));
    processAt(2717);
    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(12149);

    // Open session inside envelope via Detect-trigger → isWarmUp=1.
    const uint8_t detect[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(detect, sizeof(detect));
    processAt(12469);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);

    // LID deploy frame — Warm-Up must release.
    const uint8_t lidDeploy[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(lidDeploy, sizeof(lidDeploy));
    processAt(13000);

    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
}

void test_warmup_stays_suppressed_on_heartbeat_06_to_03_without_gun() {
    // Field captures show 06→03 can occur in speculative sessions with no
    // gun identification. Accept 0x06 as a resting predecessor for the
    // release logic, but do not let 06→03 alone paint a generic LASER.
    resetModule();
    beginEnabled();

    // Set firstFrameMs_ and advance past boot envelope.
    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(1000);

    // Heartbeat enters warm-up mode (byte1=02).
    const uint8_t hbWarm[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hbWarm, sizeof(hbWarm));
    processAt(108321);

    // Open session under warm-up flag.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(109072);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    // Byte1 transitions 02 → 06 (transitional). Still warm-up.
    const uint8_t hb06[] = { 0xB0, 0x06, 0x00, 0x36 };
    inject(hb06, sizeof(hb06));
    processAt(109200);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);

    // Byte1 transitions 06 → 03 with no gun on the session. Stay warm-up.
    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(109350);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
}

void test_warmup_does_not_fallback_release_unknown_session() {
    // Regression for alp_2-2450a468.csv @ 244385 → 246888 ms: a bare
    // detect-trigger sat in byte1=02 with no gun frame, then the 2.5 s
    // fallback promoted it to a generic LASER. Unknown sessions must not
    // auto-promote just because they stayed open.
    resetModule();
    beginEnabled();

    // Set firstFrameMs_ and advance past boot envelope.
    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(1000);

    // Heartbeat enters warm-up mode (byte1=02), past boot envelope.
    const uint8_t hbWarm[] = { 0xB0, 0x02, 0x00, 0x32 };
    inject(hbWarm, sizeof(hbWarm));
    processAt(108321);

    // Open session under warm-up flag via detect trigger.
    const uint8_t trigger[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(trigger, sizeof(trigger));
    processAt(109072);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().isWarmUp);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    // Keep sending warm-up heartbeats (byte1=02) so no confirmation fires.
    // Advance to just before the old fallback window.
    inject(hbWarm, sizeof(hbWarm));
    processAt(109072 + 2499);
    TEST_ASSERT_TRUE_MESSAGE(alpRuntimeModule.currentSession().isWarmUp,
        "should still be warm-up before 2500 ms");
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);

    // Advance past the old fallback threshold. Session must stay suppressed.
    inject(hbWarm, sizeof(hbWarm));
    processAt(109072 + 2501);
    TEST_ASSERT_TRUE_MESSAGE(alpRuntimeModule.currentSession().isWarmUp,
        "unknown session must not auto-release after 2500 ms");
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
}

void test_direction_relatches_front_on_byte1_targeted() {
    // Regression for on-device PL3 engagement (alp_2-f6e0ca7e.csv):
    // byte1 trajectory 04→04→03→...→01. First non-00/02 was 04 (LID) →
    // latched REAR. Later byte1=01 (Targeted) is the manufacturer's
    // FRONT signal but latch-once blocked the update, leaving REAR
    // displayed on what the driver saw as a front shot. Per the LED
    // color rule (Targeted = RED = front), byte1=01 must re-latch FRONT.
    resetModule();
    beginEnabled();

    // Arm module past envelope so the session is a real engagement.
    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(40000);

    // Gun-ID burst opens the session AND anchors direction sampling per
    // docs/ALP_PROTOCOL_EVIDENCE.md § Direction anchoring. Then LID
    // heartbeat latches REAR.
    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    const uint8_t hbLid[] = { 0xB0, 0x04, 0x00, 0x34 };
    inject(hbLid, sizeof(hbLid));
    processAt(40100);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR,
                      alpRuntimeModule.currentSession().direction);

    // Later: byte1 transitions to 01 (Targeted). Must re-latch FRONT.
    const uint8_t hbTargeted[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hbTargeted, sizeof(hbTargeted));
    processAt(50000);

    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT,
                      alpRuntimeModule.currentSession().direction);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT,
                      alpRuntimeModule.currentEvent().direction);
}

void test_direction_rear_latch_not_flipped_by_subsequent_rear() {
    // REAR→REAR re-sampling must be a no-op — only byte1=01 can flip REAR.
    resetModule();
    beginEnabled();

    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(40000);

    // Gun-ID burst opens the session AND anchors direction sampling per
    // docs/ALP_PROTOCOL_EVIDENCE.md § Direction anchoring. Then LID
    // heartbeat latches REAR.
    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    const uint8_t hbLid[] = { 0xB0, 0x04, 0x00, 0x34 };
    inject(hbLid, sizeof(hbLid));
    processAt(40100);

    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR,
                      alpRuntimeModule.currentSession().direction);

    // Another REAR byte1 — must stay REAR (no-op).
    const uint8_t hbDliAgain[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDliAgain, sizeof(hbDliAgain));
    processAt(41000);

    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR,
                      alpRuntimeModule.currentSession().direction);
}

void test_teardown_without_gun_id_does_not_display() {
    // Phantom-session regression: a session that enters ALERT_ACTIVE from
    // noise but never receives a GUN_ID C-frame must not paint laser on
    // the display through the 5s TEARDOWN timeout. Gate TEARDOWN display-
    // active on session_.gun != UNKNOWN.
    // Evidence: alp_6-61aa53a8.csv @ 48531 — mode=01 session opens, flips
    // ALERT_ACTIVE→TEARDOWN almost immediately, never identifies a gun,
    // SESSION_CLOSE at dur=5130 trig=1.
    resetModule();
    beginEnabled();

    // Arm past envelope, seed a Targeted heartbeat so the session opens
    // without the Warm-Up flag (mode=01 path).
    const uint8_t hbTargeted[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hbTargeted, sizeof(hbTargeted));
    processAt(40000);

    // Session opens via Detect-trigger. No gun-ID will follow.
    const uint8_t detect[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(detect, sizeof(detect));
    processAt(48531);

    // Byte1 drops to 00 → ALERT_ACTIVE→TEARDOWN.
    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(48600);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN,
                      alpRuntimeModule.currentSession().gun);
    // Phantom session — no gun identified, must stay off display through
    // TEARDOWN even though session_.active is true.
    TEST_ASSERT_FALSE(alpRuntimeModule.currentEvent().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.hasLaserEvent());
}

// ── alp_5-3fe19956.csv replay regressions ───────────────────────────
//
// These two tests lock in the "laser rear with no rear sensors" and
// "PL3 cleared during alert" regressions on-disk. Each replays the real
// byte stream from the capture byte-for-byte so a future edit that
// reintroduces the behavior fails loudly here.

void test_alp5_session7_dli_without_gun_id_stays_unknown_direction() {
    // alp_5-3fe19956.csv session #7: 16× byte1=03 (DLI) heartbeats across
    // ~15 seconds with ZERO CX GUN_ID frames and ZERO byte1=01 Targeted
    // samples. Before the gun-ID anchor gate, sampleSessionDirection
    // latched REAR on the first 03 and painted "laser rear" on a
    // front-only install. With the anchor gate, direction must stay
    // UNKNOWN: no gun-ID = no authority to classify direction.
    resetModule();
    beginEnabled();

    // Seed past the boot envelope and prime a rest heartbeat so a later
    // Detect trigger doesn't flag the session on the envelope or
    // no_heartbeat_yet clauses — we want a clean non-warm-up session
    // that only fails to classify direction.
    const uint8_t hbRest[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbRest, sizeof(hbRest));
    processAt(1000);

    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(111000);

    // Session #7 opens via Detect-trigger at +111886 ms in the capture.
    const uint8_t detect[] = { 0x98, 0x02, 0x00, 0x1A };
    inject(detect, sizeof(detect));
    processAt(111886);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);

    // Replay 16 byte1=03 heartbeats spread across the session window.
    // No gun-ID burst, no byte1=01 — classifier has nothing to anchor on.
    for (int i = 0; i < 16; ++i) {
        inject(hbDli, sizeof(hbDli));
        processAt(111886 + 100 + i * 900);
    }

    // Gun was never identified — neither session-level nor boot-level.
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN,
                      alpRuntimeModule.lastIdentifiedGun());
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN,
                      alpRuntimeModule.currentSession().gun);

    // Direction must remain UNKNOWN at both session and event surfaces.
    // This is the contract: no gun-ID + no byte1=01 → no direction.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            static_cast<uint8_t>(alpRuntimeModule.currentSession().direction));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::UNKNOWN),
                            static_cast<uint8_t>(alpRuntimeModule.currentEvent().direction));
    TEST_ASSERT_EQUAL_UINT8(0x00,
                            alpRuntimeModule.currentSession().directionSampleByte1);
}

void test_alp5_post_gun_id_reopen_not_flagged_warmup_in_envelope() {
    // alp_5-3fe19956.csv sessions 3/4/5: session #2 identified PL3 at
    // +21037 ms (well inside the 35s boot envelope). After SESSION_CLOSE
    // the ALP continued emitting byte1=01 Targeted heartbeats for 15
    // seconds. Each 00→01 edge reopened a new session — and before the
    // fix, every one of them fell under the envelope clause and was
    // flagged isWarmUp=1, leaving the driver dark during real laser
    // activity. Post-gun-ID sessions in the same boot must NOT be
    // envelope-flagged: the ALP has left its Warm-Up sequence by
    // definition (Warm-Up never produces a CX gun-ID frame).
    resetModule();
    beginEnabled();

    // Preamble inside the Warm-Up window arms the envelope.
    const uint8_t preamble[] = { 0xF0, 0x03, 0x00, 0x73 };
    inject(preamble, sizeof(preamble));
    processAt(2000);
    // Any B0 heartbeat past the preamble seeds lastHbByte1_ so later
    // sessions don't trip the no_heartbeat_yet warm-up clause.
    const uint8_t hbIdle[] = { 0xB0, 0x00, 0x00, 0x30 };
    inject(hbIdle, sizeof(hbIdle));
    processAt(3000);

    // Session #2 opens mid-envelope and identifies PL3 at +21037 ms.
    // Pre-gun-ID envelope flag is expected here — it's the established
    // Warm-Up suppression that the boot_self_test test already covers.
    inject(BURST_PL3_DETECT, sizeof(BURST_PL3_DETECT));
    processAt(12113);
    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE,
                      alpRuntimeModule.currentSession().gun);
    // Gun-ID release-from-warmup inside handleGunCandidate clears the
    // flag on the same session.
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().isWarmUp);

    // Session #2 closes via the ALERT_ACTIVE_TIMEOUT → TEARDOWN →
    // TEARDOWN_TIMEOUT path: no new 98 triggers for 15s kicks TEARDOWN,
    // then another 5s without signals returns to LISTENING and closes
    // the session. lastAlertTriggerMs_ was set at +12113 ms; stepping to
    // +27500 trips ALERT_ACTIVE_TIMEOUT_MS (15000), then +33000 trips
    // TEARDOWN_TIMEOUT_MS (5000).
    processAt(27500);
    processAt(33000);
    TEST_ASSERT_FALSE(alpRuntimeModule.currentSession().active);

    // Still inside the 35s boot envelope (firstFrameMs_ was set at
    // preamble +2000 ms → envelope ends at +37000 ms; we're at +33500).
    // Next byte1=01 heartbeat opens a NEW session on the 00→01 edge.
    // Before the fix this was envelope-flagged warm-up and the display
    // stayed dark for the rest of the Targeted burst.
    const uint8_t hbTargeted[] = { 0xB0, 0x01, 0x00, 0x31 };
    inject(hbTargeted, sizeof(hbTargeted));
    processAt(33500);

    TEST_ASSERT_TRUE(alpRuntimeModule.currentSession().active);
    TEST_ASSERT_FALSE_MESSAGE(alpRuntimeModule.currentSession().isWarmUp,
        "post-gun-ID session in same boot must not be envelope-flagged");
    TEST_ASSERT_TRUE_MESSAGE(alpRuntimeModule.currentEvent().active,
        "display window must open — this is the regression");
    // byte1=01 self-anchors FRONT per the LED color rule.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpLaserDirection::FRONT),
                            static_cast<uint8_t>(alpRuntimeModule.currentSession().direction));
}

// ── ALP SD logger CSV format regression tests ───────────────────────

void test_sd_logger_frame_columns_match_header() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logFrame(1234, "ALERT_DETECT", 0x98, 0x02, 0x00, 0x1A, AlpState::LISTENING);

    TEST_ASSERT_EQUAL_STRING(
        "1234,,ALERT_DETECT,LISTENING,,98,02,00,1A,UNKNOWN,,\n",
        logger.testGetLastLine());
}

void test_sd_logger_heartbeat_includes_checksum_and_direction() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logHeartbeat(5678, 0xB0, 0x03, 0x00, AlpState::ALERT_ACTIVE, "REAR");

    TEST_ASSERT_EQUAL_STRING(
        "5678,,HEARTBEAT,ALERT_ACTIVE,,B0,03,00,33,REAR,,\n",
        logger.testGetLastLine());
}

void test_sd_logger_heartbeat_sampling_skips_in_window_and_logs_after_interval() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logHeartbeat(1000, 0xB0, 0x03, 0x00, AlpState::LISTENING, "UNKNOWN");
    TEST_ASSERT_EQUAL_STRING(
        "1000,HEARTBEAT,LISTENING,,B0,03,00,33,UNKNOWN,,\n",
        logger.testGetLastLine());

    logger.testClearLastLine();
    logger.logHeartbeat(2500, 0xB0, 0x04, 0x00, AlpState::LISTENING, "UNKNOWN");
    TEST_ASSERT_EQUAL_STRING("", logger.testGetLastLine());

    logger.logHeartbeat(4500, 0xB0, 0x04, 0x00, AlpState::LISTENING, "UNKNOWN");
    TEST_ASSERT_EQUAL_STRING(
        "4500,HEARTBEAT,LISTENING,,B0,04,00,34,UNKNOWN,,\n",
        logger.testGetLastLine());
}

void test_sd_logger_gun_id_lid_deploy_preserves_raw_frame() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logGunIdentified(42, AlpGunType::MARKSMAN_ULTRALYTE,
                            0xCD, 0xD6, false, AlpState::ALERT_ACTIVE);

    TEST_ASSERT_EQUAL_STRING(
        "42,,GUN_ID,ALERT_ACTIVE,,CD,00,D6,23,UNKNOWN,Marksman Ultralyte,lid_deploy\n",
        logger.testGetLastLine());
}

void test_sd_logger_gun_id_detect_preserves_raw_frame() {
    AlpSdLogger logger;
    logger.begin(true, true);

    logger.logGunIdentified(43, AlpGunType::PL3_PROLITE,
                            0xC8, 0x0D, true, AlpState::ALERT_ACTIVE, "FRONT");

    TEST_ASSERT_EQUAL_STRING(
        "43,,GUN_ID,ALERT_ACTIVE,,C8,0D,00,55,FRONT,PL3 ProLite,detect\n",
        logger.testGetLastLine());
}

// ── Event bus publishing tests ──────────────────────────────────────

void test_transitionTo_publishes_alp_state_changed_event() {
    resetModule();
    SystemEventBus bus;
    bus.reset();
    alpRuntimeModule.setEventBus(&bus);
    beginEnabled();

    alpRuntimeModule.testSetState(AlpState::LISTENING);
    alpRuntimeModule.testTransitionTo(AlpState::ALERT_ACTIVE, 1000);

    SystemEvent event;
    bool found_state_enter = false;
    while (bus.consumeByType(SystemEventType::ALP_STATE_CHANGED, event)) {
        if (event.detail == 0x10) {
            found_state_enter = true;
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SystemEventType::ALP_STATE_CHANGED),
                                    static_cast<uint8_t>(event.type));
            TEST_ASSERT_EQUAL_UINT32(1000, event.tsMs);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_state_enter);
}

void test_transitionTo_leaving_alert_active_publishes_0x11() {
    resetModule();
    SystemEventBus bus;
    bus.reset();
    alpRuntimeModule.setEventBus(&bus);
    beginEnabled();

    alpRuntimeModule.testSetState(AlpState::ALERT_ACTIVE);
    alpRuntimeModule.testTransitionTo(AlpState::TEARDOWN, 2000);

    SystemEvent event;
    TEST_ASSERT_TRUE(bus.consumeByType(SystemEventType::ALP_STATE_CHANGED, event));
    TEST_ASSERT_EQUAL_UINT16(0x11, event.detail);
}

void test_session_open_publishes_0x01() {
    resetModule();
    SystemEventBus bus;
    bus.reset();
    alpRuntimeModule.setEventBus(&bus);
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(2000);

    // Should have published session open (0x01) and state transitions
    SystemEvent event;
    bool found_session_open = false;
    while (bus.consumeByType(SystemEventType::ALP_STATE_CHANGED, event)) {
        if (event.detail == 0x01) {
            found_session_open = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_session_open);
}

void test_session_close_publishes_0x02() {
    resetModule();
    SystemEventBus bus;
    bus.reset();
    alpRuntimeModule.setEventBus(&bus);
    beginEnabled();

    const uint8_t hb_idle[] = { 0xB0, 0x03, 0x00, 0x33 };  // DLI
    inject(hb_idle, sizeof(hb_idle));
    processAt(1000);

    const uint8_t trigger[] = { 0x98, 0x00, 0xE3, 0x7B };
    inject(trigger, sizeof(trigger));
    processAt(2000);

    // Transition from TEARDOWN to LISTENING to close the existing session.
    alpRuntimeModule.testSetState(AlpState::TEARDOWN);

    // Clear the bus from earlier events
    SystemEvent tmp;
    while (bus.consume(tmp)) { }

    // Now trigger the closing transition
    alpRuntimeModule.testTransitionTo(AlpState::LISTENING, 6000);

    // Should have published session close (0x02)
    SystemEvent event;
    bool found_session_close = false;
    while (bus.consumeByType(SystemEventType::ALP_STATE_CHANGED, event)) {
        if (event.detail == 0x02) {
            found_session_close = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_session_close);
}

void test_no_bus_wired_is_safe() {
    resetModule();
    alpRuntimeModule.setEventBus(nullptr);
    beginEnabled();

    alpRuntimeModule.testSetState(AlpState::LISTENING);
    // Should not crash even with null bus
    alpRuntimeModule.testTransitionTo(AlpState::ALERT_ACTIVE, 1000);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlpState::ALERT_ACTIVE),
                            static_cast<uint8_t>(alpRuntimeModule.getState()));
}

void test_process_loop_does_not_spam_events() {
    resetModule();
    SystemEventBus bus;
    bus.reset();
    alpRuntimeModule.setEventBus(&bus);
    beginEnabled();

    // Set a stable state
    alpRuntimeModule.testSetState(AlpState::LISTENING);
    alpRuntimeModule.testSetLastHeartbeat(1000);

    uint32_t initialPublishCount = bus.getPublishCount();

    // Run process multiple times without state change
    for (int i = 0; i < 10; i++) {
        processAt(1000 + i * 100);
    }

    uint32_t afterPublishCount = bus.getPublishCount();

    // Should not have published anything (no state change)
    TEST_ASSERT_EQUAL_UINT32(initialPublishCount, afterPublishCount);
}

// ── Phase 2: Atomic event snapshot tests ───────────────────────────────

void test_current_event_inactive_when_session_inactive() {
    resetModule();
    beginEnabled();

    const AlpLaserEvent& ev = alpRuntimeModule.currentEvent();
    TEST_ASSERT_FALSE(ev.active);
    TEST_ASSERT_EQUAL(AlpGunType::UNKNOWN, ev.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::UNKNOWN, ev.direction);
    TEST_ASSERT_FALSE(ev.lidActive);
}

void test_current_event_populated_in_alert_active() {
    resetModule();
    beginEnabled();

    // Open a session with a specific gun and direction
    alpRuntimeModule.testOpenSession(AlpGunType::PL3_PROLITE, false,
                                     AlpLaserDirection::FRONT);
    alpRuntimeModule.testSetState(AlpState::ALERT_ACTIVE);

    // Update the event snapshot
    alpRuntimeModule.testTransitionTo(AlpState::ALERT_ACTIVE, 1000);

    const AlpLaserEvent& ev = alpRuntimeModule.currentEvent();
    TEST_ASSERT_TRUE(ev.active);
    TEST_ASSERT_EQUAL(AlpGunType::PL3_PROLITE, ev.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::FRONT, ev.direction);
}

void test_current_event_opened_at_on_edge() {
    resetModule();
    beginEnabled();

    // Prime lastHbByte1_ with a non-suspicious listening mode so the
    // freshEngagement path does not flag warm-up. A bare detect-trigger
    // opened from byte1=00 is now intentionally suppressed until a real
    // confirmation arrives.
    const uint8_t hbDli[] = { 0xB0, 0x03, 0x00, 0x33 };
    inject(hbDli, sizeof(hbDli));
    processAt(100);  // -> LISTENING, lastHbByte1_ = 0x03

    // Inactive -> active transition should latch openedAtMs
    uint32_t transitionTime = 5000;
    alpRuntimeModule.testTransitionTo(AlpState::ALERT_ACTIVE, transitionTime);

    const AlpLaserEvent& ev = alpRuntimeModule.currentEvent();
    TEST_ASSERT_EQUAL_UINT32(transitionTime, ev.openedAtMs);

    // Run again without changing state — openedAtMs should not change
    alpRuntimeModule.process(transitionTime + 100);
    const AlpLaserEvent& ev2 = alpRuntimeModule.currentEvent();
    TEST_ASSERT_EQUAL_UINT32(transitionTime, ev2.openedAtMs);
}

void test_current_event_lid_active_tracks_hb_byte1() {
    resetModule();
    beginEnabled();

    // Inject a heartbeat with byte1=04 (LID active)
    static const uint8_t HB_LID[] = {0xB0, 0x04, 0xFF, 0x33};  // LID (04)
    alpRuntimeModule.testInjectBytes(HB_LID, sizeof(HB_LID));
    alpRuntimeModule.process(1000);

    const AlpLaserEvent& ev = alpRuntimeModule.currentEvent();
    TEST_ASSERT_TRUE(ev.lidActive);

    // Inject a heartbeat with byte1=03 (DLI active)
    static const uint8_t HB_DLI[] = {0xB0, 0x03, 0xFE, 0x31};  // DLI (03)
    alpRuntimeModule.testInjectBytes(HB_DLI, sizeof(HB_DLI));
    alpRuntimeModule.process(2000);

    const AlpLaserEvent& ev2 = alpRuntimeModule.currentEvent();
    TEST_ASSERT_FALSE(ev2.lidActive);
}

void test_update_current_event_called_before_publish() {
    resetModule();
    SystemEventBus bus;
    bus.reset();
    alpRuntimeModule.setEventBus(&bus);
    beginEnabled();

    alpRuntimeModule.testSetState(AlpState::TEARDOWN);

    // Open a session and transition through the in-engagement re-arm path.
    // TEARDOWN -> ALERT_ACTIVE preserves the existing session payload.
    alpRuntimeModule.testOpenSession(AlpGunType::LASER_ATLANTA_PL2, false,
                                     AlpLaserDirection::REAR);

    // Transition should publish the event
    uint32_t transitionTime = 3000;
    alpRuntimeModule.testTransitionTo(AlpState::ALERT_ACTIVE, transitionTime);

    // The event should be fresh at publish time (no way to verify the exact
    // ordering, but we can verify the event is populated correctly).
    const AlpLaserEvent& ev = alpRuntimeModule.currentEvent();
    TEST_ASSERT_TRUE(ev.active);
    TEST_ASSERT_EQUAL(AlpGunType::LASER_ATLANTA_PL2, ev.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR, ev.direction);
}

// ── logDisplayDecision dedup gate ────────────────────────────────────

void test_log_display_decision_emits_first_call() {
    resetModule();
    beginEnabled();
    alpRuntimeModule.logDisplayDecision(1000, "DISP_V1_EVENT", "active=1 gun=PL3");
    TEST_ASSERT_EQUAL_STRING("DISP_V1_EVENT", alpRuntimeModule.testGetLastDisplayLogEvent());
    TEST_ASSERT_EQUAL_STRING("active=1 gun=PL3", alpRuntimeModule.testGetLastDisplayLogDetail());
    TEST_ASSERT_EQUAL_UINT32(0, alpRuntimeModule.testGetDisplayLogSuppressedCount());
}

void test_log_display_decision_dedups_identical_repeat() {
    resetModule();
    beginEnabled();
    alpRuntimeModule.logDisplayDecision(1000, "DISP_V1_EVENT", "active=1 gun=PL3");
    alpRuntimeModule.logDisplayDecision(1100, "DISP_V1_EVENT", "active=1 gun=PL3");
    alpRuntimeModule.logDisplayDecision(1200, "DISP_V1_EVENT", "active=1 gun=PL3");
    // Two identical repeats should be suppressed.
    TEST_ASSERT_EQUAL_UINT32(2, alpRuntimeModule.testGetDisplayLogSuppressedCount());
}

void test_log_display_decision_detail_change_emits() {
    resetModule();
    beginEnabled();
    alpRuntimeModule.logDisplayDecision(1000, "DISP_V1_EVENT", "active=1 gun=PL3");
    alpRuntimeModule.logDisplayDecision(1100, "DISP_V1_EVENT", "active=1 gun=PL2");
    TEST_ASSERT_EQUAL_STRING("active=1 gun=PL2", alpRuntimeModule.testGetLastDisplayLogDetail());
    TEST_ASSERT_EQUAL_UINT32(0, alpRuntimeModule.testGetDisplayLogSuppressedCount());
}

void test_log_display_decision_event_change_emits() {
    resetModule();
    beginEnabled();
    alpRuntimeModule.logDisplayDecision(1000, "DISP_V1_EVENT", "active=1");
    alpRuntimeModule.logDisplayDecision(1100, "DISP_ALP_EVENT", "active=1");
    TEST_ASSERT_EQUAL_STRING("DISP_ALP_EVENT", alpRuntimeModule.testGetLastDisplayLogEvent());
    TEST_ASSERT_EQUAL_UINT32(0, alpRuntimeModule.testGetDisplayLogSuppressedCount());
}

void test_log_display_decision_state_transition_clears_dedup() {
    // A state-driven event that textually repeats across a state transition
    // must still re-emit — the transition is semantically new even if the
    // string is identical. transitionTo() clears the dedup cache.
    resetModule();
    beginEnabled();
    alpRuntimeModule.logDisplayDecision(1000, "DISP_V1_EVENT", "active=1");
    alpRuntimeModule.testTransitionTo(AlpState::ALERT_ACTIVE, 1100);
    alpRuntimeModule.logDisplayDecision(1200, "DISP_V1_EVENT", "active=1");
    // Both emitted (second one is NOT suppressed because transitionTo cleared the cache).
    TEST_ASSERT_EQUAL_UINT32(0, alpRuntimeModule.testGetDisplayLogSuppressedCount());
}

void test_log_display_decision_null_event_is_noop() {
    resetModule();
    beginEnabled();
    alpRuntimeModule.logDisplayDecision(1000, nullptr, "detail");
    TEST_ASSERT_EQUAL_STRING("", alpRuntimeModule.testGetLastDisplayLogEvent());
    TEST_ASSERT_EQUAL_UINT32(0, alpRuntimeModule.testGetDisplayLogSuppressedCount());
}

// ── Runner ───────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Checksum tests
    RUN_TEST(test_checksum_calculation);
    RUN_TEST(test_checksum_validation_pass);
    RUN_TEST(test_checksum_validation_fail);
    RUN_TEST(test_last_valid_frame_ms_ignores_checksum_noise);
    RUN_TEST(test_last_valid_frame_ms_updates_on_valid_frame);

    // begin() tests
    RUN_TEST(test_begin_disabled_stays_disabled);
    RUN_TEST(test_begin_enabled_goes_idle);
    RUN_TEST(test_process_noop_when_disabled);

    // Gun lookup table
    RUN_TEST(test_gun_lookup_pl3);
    RUN_TEST(test_gun_lookup_dragoneye);
    RUN_TEST(test_gun_lookup_truspeed);
    RUN_TEST(test_gun_lookup_pl2);
    RUN_TEST(test_gun_lookup_ultralyte);
    RUN_TEST(test_gun_lookup_ultralyte_detect);
    RUN_TEST(test_gun_lookup_pl2_detect);
    RUN_TEST(test_gun_lookup_pl3_detect);
    RUN_TEST(test_gun_lookup_atlanta_stealth_detect);
    RUN_TEST(test_gun_lookup_dragoneye_detect);
    RUN_TEST(test_gun_lookup_laser_ally_detect);
    RUN_TEST(test_gun_lookup_stalker_detect);
    RUN_TEST(test_gun_lookup_truspeed_detect);
    RUN_TEST(test_gun_lookup_detect_unknown);
    RUN_TEST(test_gun_lookup_stalker);
    RUN_TEST(test_gun_lookup_laser_ally);
    RUN_TEST(test_gun_lookup_atlanta_stealth);
    RUN_TEST(test_gun_lookup_unknown);
    RUN_TEST(test_gun_guncode_collision_d6_resolved_by_byte0);
    RUN_TEST(test_gun_guncode_collision_eb_resolved_by_byte0);

    // Alert burst parsing (all 8 guns + Detect frame variants)
    RUN_TEST(test_burst_identifies_pl3);
    RUN_TEST(test_burst_identifies_dragoneye);
    RUN_TEST(test_burst_identifies_truspeed);
    RUN_TEST(test_burst_identifies_ultralyte);
    RUN_TEST(test_burst_identifies_ultralyte_observe);
    RUN_TEST(test_burst_identifies_pl2_observe);
    RUN_TEST(test_burst_identifies_pl3_observe);
    RUN_TEST(test_burst_identifies_atlanta_stealth_observe);
    RUN_TEST(test_burst_identifies_dragoneye_observe);
    RUN_TEST(test_burst_identifies_laser_ally_observe);
    RUN_TEST(test_burst_identifies_stalker_observe);
    RUN_TEST(test_burst_identifies_truspeed_observe);
    RUN_TEST(test_burst_identifies_stalker);
    RUN_TEST(test_burst_identifies_atlanta);
    RUN_TEST(test_burst_identifies_laser_ally);
    RUN_TEST(test_burst_identifies_pl2);
    RUN_TEST(test_burst_unknown_gun_still_alerts);

    // Heartbeat parsing
    RUN_TEST(test_heartbeat_transitions_idle_to_listening);
    RUN_TEST(test_paired_heartbeat_counted);
    RUN_TEST(test_discovery_poll_transitions_to_listening);

    // Timeouts
    RUN_TEST(test_heartbeat_timeout_returns_to_idle);
    RUN_TEST(test_heartbeat_keeps_listening_if_within_timeout);
    RUN_TEST(test_teardown_timeout_returns_to_listening);
    RUN_TEST(test_alert_active_timeout_transitions_to_teardown);
    RUN_TEST(test_alert_active_stays_before_timeout);
    RUN_TEST(test_alert_active_rearm_resets_timeout_window);
    RUN_TEST(test_alert_active_timeout_session_closes_after_teardown);

    // Register write teardown trigger
    RUN_TEST(test_fd_terminator_triggers_teardown_from_alert);
    RUN_TEST(test_fd_terminator_d3_triggers_teardown);

    // Checksum-based resync
    RUN_TEST(test_resync_discards_garbage_before_heartbeat);
    RUN_TEST(test_resync_discards_garbage_before_alert_burst);
    RUN_TEST(test_bad_checksum_frame_rejected);

    // Noise window
    RUN_TEST(test_consecutive_bad_checksums_trigger_noise_window);
    RUN_TEST(test_noise_window_ends_on_valid_frame);
    RUN_TEST(test_noise_window_exit_preserves_live_session_into_teardown);

    // Snapshot
    RUN_TEST(test_snapshot_reflects_state);
    RUN_TEST(test_snapshot_default_values);

    // Alert status
    RUN_TEST(test_is_alert_active_during_alert);
    RUN_TEST(test_is_not_alert_active_during_listening);

    // String helpers
    RUN_TEST(test_gun_names_not_null);
    RUN_TEST(test_state_names_not_null);

    // Multi-burst
    RUN_TEST(test_sequential_bursts_update_gun);

    // Full lifecycle
    RUN_TEST(test_full_alert_lifecycle);

    // Standalone frames
    RUN_TEST(test_alert_trigger_standalone);

    // Observe-mode alert (98 02 00)
    RUN_TEST(test_detect_trigger_alert_98_02_00);
    RUN_TEST(test_detect_trigger_rearm_increments_burst_count);
    RUN_TEST(test_status_frame_98_other);

    // Heartbeat byte1 alert detection
    RUN_TEST(test_heartbeat_byte1_alert_transitions_to_alert_active);
    RUN_TEST(test_heartbeat_byte1_idle_resolves_alert);
    RUN_TEST(test_heartbeat_byte1_b8_does_not_trigger_alert);
    RUN_TEST(test_heartbeat_alert_repeated_01_no_double_trigger);
    RUN_TEST(test_heartbeat_alert_reopens_when_listening_inherits_stale_01);
    RUN_TEST(test_heartbeat_alert_resume_ignores_same_tick_teardown_housekeeping);
    RUN_TEST(test_heartbeat_alert_edge_ignores_same_tick_teardown_housekeeping_bounce);
    RUN_TEST(test_heartbeat_alert_then_noise_from_listening);
    RUN_TEST(test_noise_from_listening_with_hb_alert);
    RUN_TEST(test_snapshot_includes_hb_byte1);
    RUN_TEST(test_snapshot_includes_event_direction);
    RUN_TEST(test_teardown_clears_alert_flag);

    // Gun cleared on new alert entry (stale gun fix, Rev 6)
    RUN_TEST(test_new_alert_clears_stale_gun_via_98_trigger);
    RUN_TEST(test_new_alert_clears_stale_gun_via_heartbeat);
    RUN_TEST(test_new_alert_identifies_fresh_gun_after_clear);
    // Gun persists across in-engagement teardown↔alert cycling (alp_18 fix)
    RUN_TEST(test_gun_persists_through_teardown_rearm_cycle);

    // AlertSession / V1-shape display projection
    RUN_TEST(test_session_closed_by_default);
    // ownsLaserDisplay() — V1 laser suppression gate
    RUN_TEST(test_owns_laser_display_false_when_disabled);
    RUN_TEST(test_owns_laser_display_true_when_listening);
    RUN_TEST(test_owns_laser_display_true_during_alert);
    RUN_TEST(test_owns_laser_display_false_when_idle_after_timeout);
    RUN_TEST(test_session_opens_on_fresh_alert_from_listening);
    RUN_TEST(test_session_direction_front_latches_on_targeted_heartbeat);
    RUN_TEST(test_session_direction_rear_latches_on_dli_heartbeat);
    RUN_TEST(test_session_direction_rear_ignores_pre_gun_sample_until_anchored);
    RUN_TEST(test_unknown_direction_pdc_heartbeat_keeps_live_alert_visible);
    RUN_TEST(test_unknown_direction_live_alert_keeps_display_projection_with_gun);
    RUN_TEST(test_latched_rear_session_ignores_later_pdc_heartbeat_for_display);
    RUN_TEST(test_session_direction_relatches_front_over_rear);
    RUN_TEST(test_session_survives_teardown_rearm_cycle);
    RUN_TEST(test_session_closes_on_teardown_to_listening);
    RUN_TEST(test_event_gun_unknown_between_engagements_even_with_stale_lastgun);

    // Warm-Up suppression (bug #1)
    RUN_TEST(test_warm_up_flagged_when_preamble_in_window);
    RUN_TEST(test_warm_up_unflagged_when_real_gun_identified);
    RUN_TEST(test_warm_up_not_flagged_without_preamble);
    RUN_TEST(test_warm_up_preamble_after_window_ignored);
    RUN_TEST(test_warm_up_envelope_expires_after_35s);
    RUN_TEST(test_warm_up_flagged_when_no_heartbeat_yet);
    RUN_TEST(test_warm_up_flagged_when_heartbeat_mode_is_warm_up);
    RUN_TEST(test_warm_up_flagged_when_heartbeat_mode_is_idle);
    RUN_TEST(test_display_window_stays_open_across_byte1_blip_to_idle);
    RUN_TEST(test_boot_self_test_does_not_promote_on_repeat_detect);
    RUN_TEST(test_warmup_released_on_lid_deploy);
    RUN_TEST(test_warmup_stays_suppressed_on_heartbeat_02_to_03_without_gun);
    RUN_TEST(test_warmup_targeted_blip_inside_boot_envelope_stays_suppressed);
    RUN_TEST(test_warmup_released_on_targeted_heartbeat_from_idle_mode);
    RUN_TEST(test_warmup_stays_suppressed_on_heartbeat_06_to_03_without_gun);
    RUN_TEST(test_warmup_does_not_fallback_release_unknown_session);
    RUN_TEST(test_teardown_without_gun_id_does_not_display);
    RUN_TEST(test_alp5_session7_dli_without_gun_id_stays_unknown_direction);
    RUN_TEST(test_alp5_post_gun_id_reopen_not_flagged_warmup_in_envelope);

    // ALP SD logger CSV format
    RUN_TEST(test_sd_logger_frame_columns_match_header);
    RUN_TEST(test_sd_logger_heartbeat_includes_checksum_and_direction);
    RUN_TEST(test_sd_logger_gun_id_lid_deploy_preserves_raw_frame);
    RUN_TEST(test_sd_logger_gun_id_detect_preserves_raw_frame);

    // Event bus publishing
    RUN_TEST(test_transitionTo_publishes_alp_state_changed_event);
    RUN_TEST(test_transitionTo_leaving_alert_active_publishes_0x11);
    RUN_TEST(test_session_open_publishes_0x01);
    RUN_TEST(test_session_close_publishes_0x02);
    RUN_TEST(test_no_bus_wired_is_safe);
    RUN_TEST(test_process_loop_does_not_spam_events);

    // Phase 2: Atomic event snapshot
    RUN_TEST(test_current_event_inactive_when_session_inactive);
    RUN_TEST(test_current_event_populated_in_alert_active);
    RUN_TEST(test_current_event_opened_at_on_edge);
    RUN_TEST(test_current_event_lid_active_tracks_hb_byte1);
    RUN_TEST(test_update_current_event_called_before_publish);

    // logDisplayDecision dedup gate
    RUN_TEST(test_log_display_decision_emits_first_call);
    RUN_TEST(test_log_display_decision_dedups_identical_repeat);
    RUN_TEST(test_log_display_decision_detail_change_emits);
    RUN_TEST(test_log_display_decision_event_change_emits);
    RUN_TEST(test_log_display_decision_state_transition_clears_dedup);
    RUN_TEST(test_log_display_decision_null_event_is_noop);

    return UNITY_END();
}
