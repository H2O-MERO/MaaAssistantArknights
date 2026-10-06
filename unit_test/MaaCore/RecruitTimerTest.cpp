#include <catch2/catch_test_macros.hpp>

#include "Task/Miscellaneous/RecruitTimer.h"

namespace recruit_timer_test
{
int decrement_hour(int minutes)
{
    // 小时减号在 1 小时处绕回上限；9 小时的分钟数必须为 00。
    return minutes / 60 <= 1 ? 540 : minutes - 60;
}
}

TEST_CASE("Recruit timer handles minute borrowing before adjusting minutes")
{
    using asst::recruit_calc::should_decrement_hour;
    REQUIRE(should_decrement_hour(60, 460));
    REQUIRE(should_decrement_hour(540, 460));
    REQUIRE_FALSE(should_decrement_hour(480, 460));
    REQUIRE_FALSE(should_decrement_hour(470, 460));
}

TEST_CASE("Recruit timer reaches every supported time from every current time")
{
    for (int initial = 60; initial <= 540; initial += 10) {
        for (int expected = 60; expected <= 540; expected += 10) {
            INFO("initial=" << initial << " expected=" << expected);
            int current = initial;
            int adjustments = 0;
            while (current != expected && adjustments < 16) {
                if (asst::recruit_calc::should_decrement_hour(current, expected)) {
                    current = recruit_timer_test::decrement_hour(current);
                }
                else {
                    REQUIRE(current > 60);
                    current -= 10;
                }
                ++adjustments;
            }
            REQUIRE(current == expected);
        }
    }
}

TEST_CASE("Recruit timer replans after an unexpected change instead of repeating the old adjustment")
{
    using asst::recruit_calc::should_decrement_hour;
    REQUIRE_FALSE(should_decrement_hour(480, 460));
    // 调整分钟时若界面变成了 06:40，下一步应改为调整小时。
    REQUIRE(should_decrement_hour(400, 460));
}
