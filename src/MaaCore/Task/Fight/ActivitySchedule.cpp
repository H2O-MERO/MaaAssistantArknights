#include "ActivitySchedule.h"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <string>

#include <meojson/json.hpp>

namespace
{
std::optional<std::chrono::sys_seconds> parse_activity_time(const std::string& text, int time_zone)
{
    int year_value = 0;
    int month_value = 0;
    int day_value = 0;
    int hour_value = 0;
    int minute_value = 0;
    int second_value = 0;
    const auto parse_number = [&text](size_t offset, size_t length, int& value) {
        const char* begin = text.data() + offset;
        const char* end = begin + length;
        const auto [ptr, error] = std::from_chars(begin, end, value);
        return error == std::errc {} && ptr == end;
    };
    if (text.size() != 19 || text[4] != '/' || text[7] != '/' || text[10] != ' ' || text[13] != ':' ||
        text[16] != ':' || !parse_number(0, 4, year_value) || !parse_number(5, 2, month_value) ||
        !parse_number(8, 2, day_value) || !parse_number(11, 2, hour_value) ||
        !parse_number(14, 2, minute_value) || !parse_number(17, 2, second_value) || time_zone < -23 ||
        time_zone > 23 || hour_value < 0 || hour_value > 23 ||
        minute_value < 0 || minute_value > 59 || second_value < 0 || second_value > 59) {
        return std::nullopt;
    }

    const std::chrono::year_month_day date {
        std::chrono::year { year_value },
        std::chrono::month { static_cast<unsigned>(month_value) },
        std::chrono::day { static_cast<unsigned>(day_value) },
    };
    if (!date.ok()) {
        return std::nullopt;
    }

    const auto local_time = std::chrono::sys_days { date } + std::chrono::hours { hour_value } +
                            std::chrono::minutes { minute_value } + std::chrono::seconds { second_value };
    return std::chrono::time_point_cast<std::chrono::seconds>(local_time - std::chrono::hours { time_zone });
}
} // namespace

std::optional<asst::ActivitySchedule> asst::ActivitySchedule::load(
    const std::filesystem::path& path,
    std::string_view client_type)
{
    auto root_opt = json::open(path, true, true);
    if (!root_opt) {
        return std::nullopt;
    }

    const std::string client_key = client_type == "Bilibili" ? "Official" : std::string(client_type);
    auto client_opt = root_opt->find<json::object>(client_key);
    if (!client_opt) {
        return std::nullopt;
    }

    auto side_story_opt = client_opt->find<json::object>("sideStoryStage");
    if (!side_story_opt) {
        return std::nullopt;
    }

    ActivitySchedule schedule;
    int schedule_time_zone = std::numeric_limits<int>::max();
    for (const auto& [_, side_story] : *side_story_opt) {
        auto activity_opt = side_story.find<json::object>("Activity");
        if (!activity_opt) {
            continue;
        }

        const int time_zone = activity_opt->get("TimeZone", std::numeric_limits<int>::max());
        const std::string start_text = activity_opt->get("UtcStartTime", std::string());
        const std::string expire_text = activity_opt->get("UtcExpireTime", std::string());
        auto start = parse_activity_time(start_text, time_zone);
        auto expire = parse_activity_time(expire_text, time_zone);
        if (!start || !expire || *start > *expire) {
            continue;
        }

        if (schedule_time_zone == std::numeric_limits<int>::max()) {
            schedule_time_zone = time_zone;
        }
        else if (schedule_time_zone != time_zone) {
            continue;
        }

        schedule.m_periods.emplace_back(*start, *expire);
    }

    if (schedule.m_periods.empty()) {
        return schedule;
    }

    schedule.m_time_zone = schedule_time_zone;
    std::ranges::sort(schedule.m_periods, {}, &Period::start);
    return schedule;
}

bool asst::ActivitySchedule::should_skip_fight(std::chrono::sys_seconds now) const
{
    constexpr auto ActivityBoundaryTolerance = std::chrono::seconds { 1 };
    constexpr auto GameDayStart = std::chrono::hours { 4 };

    std::vector<Period> current_periods;
    std::ranges::copy_if(m_periods, std::back_inserter(current_periods), [now](const Period& period) {
        return period.start <= now && now <= period.expire;
    });
    if (current_periods.empty()) {
        return false;
    }

    const auto game_time = now + std::chrono::hours { m_time_zone } - GameDayStart;
    const auto game_day = std::chrono::floor<std::chrono::days>(game_time);
    const int current_weekday = std::chrono::weekday { game_day }.c_encoding();
    int days_until_next_monday = (1 - current_weekday + 7) % 7;
    if (days_until_next_monday == 0) {
        days_until_next_monday = 7;
    }

    const auto next_week_start_game_time = game_day + std::chrono::days { days_until_next_monday };
    const auto next_week_start = std::chrono::time_point_cast<std::chrono::seconds>(
        next_week_start_game_time - std::chrono::hours { m_time_zone } + GameDayStart);
    const auto continuous_coverage_deadline = next_week_start - ActivityBoundaryTolerance;

    auto covered_until = std::ranges::max(current_periods, {}, &Period::expire).expire;
    if (covered_until >= continuous_coverage_deadline) {
        return false;
    }

    for (const auto& period : m_periods) {
        if (period.start <= now || period.start >= next_week_start) {
            continue;
        }
        if (period.start - covered_until > ActivityBoundaryTolerance) {
            return true;
        }
        if (period.expire > covered_until) {
            covered_until = period.expire;
            if (covered_until >= continuous_coverage_deadline) {
                return false;
            }
        }
    }

    return covered_until < continuous_coverage_deadline;
}
