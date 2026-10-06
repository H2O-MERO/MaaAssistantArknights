#pragma once

namespace asst::recruit_calc
{
constexpr bool should_decrement_hour(int current_minutes, int expected_minutes)
{
    // 分钟减号从 00 分借位，例如 08:00 -> 07:50，需先留出一小时。
    const int intermediate_hour = expected_minutes / 60 + (current_minutes % 60 < expected_minutes % 60);
    return current_minutes / 60 != intermediate_hour;
}
}
