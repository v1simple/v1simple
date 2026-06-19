#include <unity.h>

#include "../../src/modules/alp/alp_event_latch.h"
#include "../../src/modules/alp/alp_runtime_module.h"
#include "../../src/modules/alp/alp_event_latch.cpp"

static AlpEventLatch latch;

static AlpLaserEvent makeActiveEvent(AlpGunType gun = AlpGunType::PL3_PROLITE,
                                     AlpLaserDirection direction = AlpLaserDirection::FRONT,
                                     bool lidActive = false,
                                     uint32_t openedAtMs = 1000) {
    AlpLaserEvent event{};
    event.active = true;
    event.gun = gun;
    event.direction = direction;
    event.lidActive = lidActive;
    event.openedAtMs = openedAtMs;
    return event;
}

void setUp() {
    latch.clearLatch();
}

void tearDown() {}

void test_set_event_and_start_persistence_shows_within_window() {
    latch.setEvent(makeActiveEvent());
    latch.startPersistence(2000);

    TEST_ASSERT_TRUE(latch.isLatched());
    TEST_ASSERT_TRUE(latch.shouldShowPersisted(2500, 2000));
    TEST_ASSERT_FALSE(latch.shouldShowPersisted(4000, 2000));
}

void test_clear_latch_clears_immediately() {
    latch.setEvent(makeActiveEvent());
    latch.startPersistence(2000);

    latch.clearLatch();

    TEST_ASSERT_FALSE(latch.isLatched());
    TEST_ASSERT_FALSE(latch.shouldShowPersisted(2500, 2000));
}

void test_new_set_event_resets_persistence_timer() {
    latch.setEvent(makeActiveEvent(AlpGunType::PL3_PROLITE,
                                   AlpLaserDirection::FRONT,
                                   false,
                                   1000));
    latch.startPersistence(2000);
    TEST_ASSERT_TRUE(latch.shouldShowPersisted(2500, 2000));

    latch.setEvent(makeActiveEvent(AlpGunType::MARKSMAN_ULTRALYTE,
                                   AlpLaserDirection::REAR,
                                   true,
                                   3000));
    latch.startPersistence(3500);

    const AlpLaserEvent& latched = latch.latchedEvent();
    TEST_ASSERT_EQUAL(AlpGunType::MARKSMAN_ULTRALYTE, latched.gun);
    TEST_ASSERT_EQUAL(AlpLaserDirection::REAR, latched.direction);
    TEST_ASSERT_TRUE(latch.shouldShowPersisted(5000, 2000));
    TEST_ASSERT_FALSE(latch.shouldShowPersisted(5500, 2000));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_set_event_and_start_persistence_shows_within_window);
    RUN_TEST(test_clear_latch_clears_immediately);
    RUN_TEST(test_new_set_event_resets_persistence_timer);
    return UNITY_END();
}