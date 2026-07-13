#include <unity.h>

#include "../../src/provider_callback_bindings.h"

namespace {

class MutableTarget {
public:
    void tick(uint32_t delta) {
        total_ += delta;
    }

    bool ready() {
        return total_ > 0;
    }

private:
    uint32_t total_ = 0;
};

class ConstTarget {
public:
    explicit ConstTarget(int base) : base_(base) {}

    int read(int offset) const {
        return base_ + offset;
    }

private:
    int base_ = 0;
};

}  // namespace

void setUp() {}

void tearDown() {}

void test_binds_void_member_with_arguments() {
    MutableTarget target;

    ProviderCallbackBindings::member<MutableTarget, &MutableTarget::tick>(&target, 7u);

    const bool ready = ProviderCallbackBindings::member<MutableTarget, &MutableTarget::ready>(&target);
    TEST_ASSERT_TRUE(ready);
}

void test_binds_non_const_returning_member() {
    MutableTarget target;

    const bool readyBefore =
        ProviderCallbackBindings::member<MutableTarget, &MutableTarget::ready>(&target);
    TEST_ASSERT_FALSE(readyBefore);

    ProviderCallbackBindings::member<MutableTarget, &MutableTarget::tick>(&target, 1u);

    const bool readyAfter =
        ProviderCallbackBindings::member<MutableTarget, &MutableTarget::ready>(&target);
    TEST_ASSERT_TRUE(readyAfter);
}

void test_binds_const_returning_member_with_arguments() {
    ConstTarget target(12);

    const int value =
        ProviderCallbackBindings::member<ConstTarget, &ConstTarget::read>(&target, 5);

    TEST_ASSERT_EQUAL(17, value);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_binds_void_member_with_arguments);
    RUN_TEST(test_binds_non_const_returning_member);
    RUN_TEST(test_binds_const_returning_member_with_arguments);
    return UNITY_END();
}
