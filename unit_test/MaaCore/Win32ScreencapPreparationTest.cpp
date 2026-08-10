#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "Controller/Win32ScreencapPreparation.hpp"

namespace
{
enum class Action
{
    SaveCursor,
    SaveWindow,
    MoveCursor,
    AlignWindow,
    ParkWindow,
    SendHover,
    SendLeave,
    Wait,
    Capture,
    Restore,
};

bool record_preparation(
    const asst::Win32ScreencapPlan& plan,
    std::vector<Action>& actions,
    bool save_window_result = true)
{
    return asst::prepare_win32_screencap(
        plan,
        [&]() {
            actions.emplace_back(Action::SaveCursor);
            return true;
        },
        [&]() {
            actions.emplace_back(Action::SaveWindow);
            return save_window_result;
        },
        [&](int, int) {
            actions.emplace_back(Action::MoveCursor);
            return true;
        },
        [&](int, int) {
            actions.emplace_back(Action::AlignWindow);
            return true;
        },
        [&]() {
            actions.emplace_back(Action::ParkWindow);
            return true;
        },
        [&](int, int) {
            actions.emplace_back(Action::SendHover);
            return true;
        },
        [&]() {
            actions.emplace_back(Action::SendLeave);
            return true;
        },
        [&](int) { actions.emplace_back(Action::Wait); });
}
} // namespace

TEST_CASE("Win32 input methods map to explicit screencap positioning strategies")
{
    struct TestCase
    {
        std::string_view name;
        uint64_t method = 0;
        asst::Win32ScreencapInputType expected = asst::Win32ScreencapInputType::None;
    };

    constexpr std::array TestCases = {
        TestCase { "None", 0, asst::Win32ScreencapInputType::None },
        TestCase { "Seize", 1ULL, asst::Win32ScreencapInputType::CursorPos },
        TestCase { "SendMessage", 1ULL << 1, asst::Win32ScreencapInputType::Message },
        TestCase { "PostMessage", 1ULL << 2, asst::Win32ScreencapInputType::Message },
        TestCase { "LegacyEvent", 1ULL << 3, asst::Win32ScreencapInputType::CursorPos },
        TestCase { "PostThreadMessage", 1ULL << 4, asst::Win32ScreencapInputType::None },
        TestCase { "SendMessageWithCursorPos", 1ULL << 5, asst::Win32ScreencapInputType::CursorPos },
        TestCase { "PostMessageWithCursorPos", 1ULL << 6, asst::Win32ScreencapInputType::CursorPos },
        TestCase { "SendMessageWithWindowPos", 1ULL << 7, asst::Win32ScreencapInputType::WindowPos },
        TestCase { "PostMessageWithWindowPos", 1ULL << 8, asst::Win32ScreencapInputType::WindowPos },
    };

    for (const auto& test_case : TestCases) {
        DYNAMIC_SECTION(test_case.name)
        {
            REQUIRE(asst::classify_win32_screencap_input(test_case.method) == test_case.expected);
        }
    }
}

TEST_CASE("Screencap plans use positioning actions instead of pointer contacts")
{
    struct TestCase
    {
        bool main_screen = false;
        asst::Win32ScreencapInputType input = asst::Win32ScreencapInputType::None;
        asst::Win32ScreencapAction action = asst::Win32ScreencapAction::None;
        int target_x = 0;
        int target_y = 0;
        int delay_ms = 0;
    };

    constexpr std::array TestCases = {
        TestCase { false, asst::Win32ScreencapInputType::None, asst::Win32ScreencapAction::None, 0, 0, 0 },
        TestCase { false, asst::Win32ScreencapInputType::Message, asst::Win32ScreencapAction::SendHover, 0, 719, 17 },
        TestCase { true, asst::Win32ScreencapInputType::Message, asst::Win32ScreencapAction::SendHover, 640, 360, 300 },
        TestCase {
            false,
            asst::Win32ScreencapInputType::CursorPos,
            asst::Win32ScreencapAction::MoveCursorAndSendHover,
            0,
            719,
            17,
        },
        TestCase {
            true,
            asst::Win32ScreencapInputType::CursorPos,
            asst::Win32ScreencapAction::MoveCursorAndSendHover,
            640,
            360,
            300,
        },
        TestCase {
            false,
            asst::Win32ScreencapInputType::WindowPos,
            asst::Win32ScreencapAction::ParkWindowAndSendLeave,
            0,
            0,
            17,
        },
        TestCase {
            true,
            asst::Win32ScreencapInputType::WindowPos,
            asst::Win32ScreencapAction::AlignWindowAndSendHover,
            640,
            360,
            300,
        },
    };

    for (const auto& test_case : TestCases) {
        const auto plan = asst::make_win32_screencap_plan(1280, 720, test_case.main_screen, test_case.input);
        REQUIRE(plan.action == test_case.action);
        REQUIRE(plan.target_x == test_case.target_x);
        REQUIRE(plan.target_y == test_case.target_y);
        REQUIRE(plan.settle_delay_ms == test_case.delay_ms);
    }
}

TEST_CASE("WindowPos screencap saves and restores window position in order")
{
    SECTION("main screen")
    {
        std::vector<Action> actions;
        const auto plan = asst::make_win32_screencap_plan(1280, 720, true, asst::Win32ScreencapInputType::WindowPos);
        const bool result = asst::execute_win32_screencap(
            plan,
            [&](const auto& preparation) { return record_preparation(preparation, actions); },
            [&]() {
                actions.emplace_back(Action::Capture);
                return true;
            },
            [&]() { actions.emplace_back(Action::Restore); });

        REQUIRE(result);
        REQUIRE(
            actions == std::vector { Action::SaveWindow,
                                     Action::AlignWindow,
                                     Action::SendHover,
                                     Action::Wait,
                                     Action::Capture,
                                     Action::Restore });
    }

    SECTION("other screen")
    {
        std::vector<Action> actions;
        const auto plan = asst::make_win32_screencap_plan(1280, 720, false, asst::Win32ScreencapInputType::WindowPos);
        const bool result = asst::execute_win32_screencap(
            plan,
            [&](const auto& preparation) { return record_preparation(preparation, actions); },
            [&]() {
                actions.emplace_back(Action::Capture);
                return true;
            },
            [&]() { actions.emplace_back(Action::Restore); });

        REQUIRE(result);
        REQUIRE(
            actions == std::vector { Action::SaveWindow,
                                     Action::ParkWindow,
                                     Action::SendLeave,
                                     Action::Wait,
                                     Action::Capture,
                                     Action::Restore });
    }
}

TEST_CASE("Screencap execution restores state on every completion path")
{
    const auto plan = asst::make_win32_screencap_plan(1280, 720, false, asst::Win32ScreencapInputType::WindowPos);

    SECTION("preparation failure skips capture")
    {
        std::vector<Action> actions;
        const bool result = asst::execute_win32_screencap(
            plan,
            [&](const auto& preparation) { return record_preparation(preparation, actions, false); },
            [&]() {
                actions.emplace_back(Action::Capture);
                return true;
            },
            [&]() { actions.emplace_back(Action::Restore); });

        REQUIRE_FALSE(result);
        REQUIRE(actions == std::vector { Action::SaveWindow, Action::Restore });
    }

    SECTION("capture failure still restores")
    {
        std::vector<Action> actions;
        const bool result = asst::execute_win32_screencap(
            plan,
            [&](const auto& preparation) { return record_preparation(preparation, actions); },
            [&]() {
                actions.emplace_back(Action::Capture);
                return false;
            },
            [&]() { actions.emplace_back(Action::Restore); });

        REQUIRE_FALSE(result);
        REQUIRE(actions.back() == Action::Restore);
    }

    SECTION("restore failure does not discard a captured frame")
    {
        bool restore_called = false;
        const bool result = asst::execute_win32_screencap(
            plan,
            [](const auto&) { return true; },
            []() { return true; },
            [&]() {
                restore_called = true;
                return false;
            });

        REQUIRE(result);
        REQUIRE(restore_called);
    }
}

TEST_CASE("Offscreen parking handles negative virtual desktop coordinates")
{
    REQUIRE(asst::calculate_offscreen_window_top(-1080, 720, 8) == -1808);
    REQUIRE(asst::calculate_offscreen_window_top(0, 720, 8) == -728);
}

TEST_CASE("Physical cursor restoration does not overwrite user movement")
{
    REQUIRE(asst::should_restore_cursor_position(100, 100, 640, 360, 640, 360));
    REQUIRE_FALSE(asst::should_restore_cursor_position(100, 100, 640, 360, 700, 400));
    REQUIRE_FALSE(asst::should_restore_cursor_position(640, 360, 640, 360, 640, 360));
}
