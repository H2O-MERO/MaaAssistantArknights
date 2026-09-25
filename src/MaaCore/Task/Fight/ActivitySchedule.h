#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace asst
{
class ActivitySchedule
{
public:
    static std::optional<ActivitySchedule> load(
        const std::filesystem::path& path,
        std::string_view client_type);

    [[nodiscard]] bool should_skip_fight(std::chrono::sys_seconds now) const;

private:
    struct Period
    {
        std::chrono::sys_seconds start;
        std::chrono::sys_seconds expire;
    };

    std::vector<Period> m_periods;
    int m_time_zone = 0;
};
} // namespace asst
