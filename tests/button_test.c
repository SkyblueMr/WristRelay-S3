#include "check.h"
#include "wake/button_logic.h"

static void test_bounce_and_short_press(void)
{
    wr_button_t key;
    wr_button_init(&key, false, 0);

    CHECK(wr_button_step(&key, true, 10000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 15000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 20000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 49999) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 50000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 70000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 75000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 80000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 109999) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 110000) == WR_BUTTON_SHORT);
    CHECK(wr_button_step(&key, false, 150000) == WR_BUTTON_NONE);
}

static void test_long_press_does_not_repeat(void)
{
    wr_button_t key;
    wr_button_init(&key, false, 0);

    CHECK(wr_button_step(&key, true, 10000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 40000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 1239999) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 1240000) == WR_BUTTON_LONG);
    CHECK(wr_button_step(&key, true, 2000000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 2010000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 2040000) == WR_BUTTON_NONE);
}

static void test_starts_with_button_held(void)
{
    wr_button_t key;
    wr_button_init(&key, true, 0);

    CHECK(wr_button_step(&key, true, 2000000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 2010000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 2040000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 2050000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 2080000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 2100000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 2130000) == WR_BUTTON_SHORT);
}

static void test_release_after_long_threshold_between_samples(void)
{
    wr_button_t key;
    wr_button_init(&key, false, 0);
    CHECK(wr_button_step(&key, true, 10000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 40000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, true, 1239999) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 1241000) == WR_BUTTON_NONE);
    CHECK(wr_button_step(&key, false, 1271000) == WR_BUTTON_LONG);
    CHECK(wr_button_step(&key, false, 1500000) == WR_BUTTON_NONE);
}

int main(void)
{
    test_bounce_and_short_press();
    test_long_press_does_not_repeat();
    test_starts_with_button_held();
    test_release_after_long_threshold_between_samples();
    return 0;
}
