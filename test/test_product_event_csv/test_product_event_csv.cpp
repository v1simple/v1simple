#include <unity.h>

#include <cstring>

#include "../../src/modules/event_log/product_event_csv.h"
#include "../../src/modules/event_log/product_event_csv.cpp"

void setUp() {}
void tearDown() {}

void test_schema_header_and_v1_rows_are_exact() {
    TEST_ASSERT_EQUAL_STRING("# product_event_schema=1\nms,source,event,id,sequence,item,count,payload\n",
                             kProductEventSchemaHeader);

    ProductEvent event{};
    event.ms = 8351;
    event.source = ProductEventSource::V1;
    event.kind = ProductEventKind::BEGIN;
    event.id = 12;
    event.sequence = 1;
    event.data.v1.count = 2;
    event.data.v1.alerts[0] = ProductV1Alert{34567, BAND_K, DIR_FRONT, 4, 0, 1};
    event.data.v1.alerts[1] = ProductV1Alert{34700, BAND_KA, DIR_FRONT, 2, 0, 0};

    char row[256];
    TEST_ASSERT_EQUAL_UINT32(2, productEventRowCount(event));
    TEST_ASSERT_NOT_EQUAL(0, serializeProductEventRow(event, 0, row, sizeof(row)));
    TEST_ASSERT_EQUAL_STRING("8351,V1,BEGIN,12,1,0,2,band=K;freq=34567;dir=F;front=4;rear=0;priority=1\n", row);
    TEST_ASSERT_NOT_EQUAL(0, serializeProductEventRow(event, 1, row, sizeof(row)));
    TEST_ASSERT_EQUAL_STRING("8351,V1,BEGIN,12,1,1,2,band=Ka;freq=34700;dir=F;front=2;rear=0;priority=0\n", row);
}

void test_alp_sparse_and_gap_rows_are_exact() {
    ProductEvent detect{};
    detect.ms = 11925;
    detect.source = ProductEventSource::ALP;
    detect.kind = ProductEventKind::DETECT;
    detect.id = 7;
    detect.sequence = 2;
    detect.data.alp.state = ProductAlpState::TARGETED;
    detect.data.alp.direction = 1;
    detect.data.alp.raw[0] = 0x98;
    detect.data.alp.raw[1] = 0x02;
    detect.data.alp.raw[2] = 0x00;

    char row[256];
    TEST_ASSERT_NOT_EQUAL(0, serializeProductEventRow(detect, 0, row, sizeof(row)));
    TEST_ASSERT_EQUAL_STRING("11925,ALP,DETECT,7,2,0,1,state=TARGETED;raw=980200;dir=F\n", row);

    ProductEvent gap{};
    gap.ms = 15180;
    gap.source = ProductEventSource::SYS;
    gap.kind = ProductEventKind::GAP;
    gap.sequence = 1;
    gap.data.gap = ProductGapData{4, 14910, 15180};
    TEST_ASSERT_NOT_EQUAL(0, serializeProductEventRow(gap, 0, row, sizeof(row)));
    TEST_ASSERT_EQUAL_STRING("15180,SYS,GAP,0,1,0,1,lost=4;first_ms=14910;last_ms=15180\n", row);
}

void test_maximum_v1_table_serializes_without_overflow() {
    ProductEvent event{};
    event.source = ProductEventSource::V1;
    event.kind = ProductEventKind::CHANGE_EVENT;
    event.data.v1.count = kProductEventMaxV1Alerts;
    for (size_t i = 0; i < kProductEventMaxV1Alerts; ++i) {
        event.data.v1.alerts[i] = ProductV1Alert{99999, BAND_KU, DIR_REAR, 8, 8, static_cast<uint8_t>(i == 14)};
    }
    char row[256];
    for (size_t i = 0; i < kProductEventMaxV1Alerts; ++i) {
        TEST_ASSERT_NOT_EQUAL(0, serializeProductEventRow(event, i, row, sizeof(row)));
        TEST_ASSERT_NULL(std::strchr(row, '\r'));
    }
    char shortRow[32];
    TEST_ASSERT_EQUAL_UINT32(0, serializeProductEventRow(event, 0, shortRow, sizeof(shortRow)));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_schema_header_and_v1_rows_are_exact);
    RUN_TEST(test_alp_sparse_and_gap_rows_are_exact);
    RUN_TEST(test_maximum_v1_table_serializes_without_overflow);
    return UNITY_END();
}
