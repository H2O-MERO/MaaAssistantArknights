#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <tuple>

#include "Task/Fight/ActivitySchedule.h"

namespace
{
using ActivityPeriod = std::tuple<std::string, std::string>;

class TemporaryActivityFile
{
public:
    TemporaryActivityFile(std::initializer_list<ActivityPeriod> periods)
    {
        const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("maa-activity-schedule-test-" + std::to_string(unique_suffix) + ".json");

        std::ofstream output(m_path);
        output << R"({"Official":{"sideStoryStage":{)";
        size_t index = 0;
        for (const auto& [start, expire] : periods) {
            if (index != 0) {
                output << ',';
            }
            output << '"' << index << R"(":{"Activity":{"UtcStartTime":")" << start
                   << R"(","UtcExpireTime":")" << expire << R"(","TimeZone":8}})";
            ++index;
        }
        output << R"(}}})";
    }

    ~TemporaryActivityFile()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

std::chrono::sys_seconds utc_time(int day, int hour, int minute = 0, int second = 0)
{
    using namespace std::chrono;
    return sys_days { year { 2026 } / September / std::chrono::day { static_cast<unsigned>(day) } } +
           hours { hour } + minutes { minute } + seconds { second };
}
} // namespace

TEST_CASE("Activity schedule skips only when current coverage ends before the game week")
{
    const auto now = utc_time(23, 4); // 2026-09-23 12:00 at UTC+8, a Wednesday game day.

    SECTION("Current activity ends before the week")
    {
        TemporaryActivityFile file { { "2026/09/22 04:00:00", "2026/09/24 03:59:59" } };
        const auto schedule = asst::ActivitySchedule::load(file.path(), "Official");
        REQUIRE(schedule);
        REQUIRE(schedule->should_skip_fight(now));
    }

    SECTION("No activity is active now")
    {
        TemporaryActivityFile file { { "2026/09/24 04:00:00", "2026/09/25 03:59:59" } };
        const auto schedule = asst::ActivitySchedule::load(file.path(), "Official");
        REQUIRE(schedule);
        REQUIRE_FALSE(schedule->should_skip_fight(now));
    }

    SECTION("One activity covers the rest of the week")
    {
        TemporaryActivityFile file { { "2026/09/21 04:00:00", "2026/09/28 03:59:59" } };
        const auto schedule = asst::ActivitySchedule::load(file.path(), "Official");
        REQUIRE(schedule);
        REQUIRE_FALSE(schedule->should_skip_fight(now));
    }

    SECTION("Adjacent activities cover the rest of the week")
    {
        TemporaryActivityFile file {
            { "2026/09/22 04:00:00", "2026/09/24 03:59:59" },
            { "2026/09/24 04:00:00", "2026/09/28 03:59:59" },
        };
        const auto schedule = asst::ActivitySchedule::load(file.path(), "Official");
        REQUIRE(schedule);
        REQUIRE_FALSE(schedule->should_skip_fight(now));
    }

    SECTION("A gap before the next activity leaves time for Annihilation")
    {
        TemporaryActivityFile file {
            { "2026/09/22 04:00:00", "2026/09/24 03:59:59" },
            { "2026/09/24 04:00:01", "2026/09/28 03:59:59" },
        };
        const auto schedule = asst::ActivitySchedule::load(file.path(), "Official");
        REQUIRE(schedule);
        REQUIRE(schedule->should_skip_fight(now));
    }
}
