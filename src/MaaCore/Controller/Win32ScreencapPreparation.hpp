#pragma once

#include <cstdint>

namespace asst
{
enum class Win32ScreencapInputType
{
    None,
    Message,
    CursorPos,
    WindowPos,
};

enum class Win32ScreencapAction
{
    None,
    SendHover,
    MoveCursorAndSendHover,
    AlignWindowAndSendHover,
    ParkWindowAndSendLeave,
};

constexpr Win32ScreencapInputType classify_win32_screencap_input(uint64_t mouse_method)
{
    constexpr uint64_t Seize = 1ULL;
    constexpr uint64_t SendMessage = 1ULL << 1;
    constexpr uint64_t PostMessage = 1ULL << 2;
    constexpr uint64_t LegacyEvent = 1ULL << 3;
    constexpr uint64_t SendMessageWithCursorPos = 1ULL << 5;
    constexpr uint64_t PostMessageWithCursorPos = 1ULL << 6;
    constexpr uint64_t SendMessageWithWindowPos = 1ULL << 7;
    constexpr uint64_t PostMessageWithWindowPos = 1ULL << 8;

    if ((mouse_method & (SendMessageWithWindowPos | PostMessageWithWindowPos)) != 0) {
        return Win32ScreencapInputType::WindowPos;
    }
    if ((mouse_method & (Seize | LegacyEvent | SendMessageWithCursorPos | PostMessageWithCursorPos)) != 0) {
        return Win32ScreencapInputType::CursorPos;
    }
    if ((mouse_method & (SendMessage | PostMessage)) != 0) {
        return Win32ScreencapInputType::Message;
    }
    return Win32ScreencapInputType::None;
}

struct Win32ScreencapPlan
{
    Win32ScreencapAction action = Win32ScreencapAction::None;
    int target_x = 0;
    int target_y = 0;
    int settle_delay_ms = 0;
};

constexpr Win32ScreencapPlan
    make_win32_screencap_plan(int screen_width, int screen_height, bool main_screen, Win32ScreencapInputType input_type)
{
    if (screen_width <= 0 || screen_height <= 0 || input_type == Win32ScreencapInputType::None) {
        return {};
    }

    const int target_x = main_screen ? screen_width / 2 : 0;
    const int target_y = main_screen ? screen_height / 2 : screen_height - 1;
    const int settle_delay_ms = main_screen ? 300 : 17;

    switch (input_type) {
    case Win32ScreencapInputType::Message:
        return { Win32ScreencapAction::SendHover, target_x, target_y, settle_delay_ms };
    case Win32ScreencapInputType::CursorPos:
        return { Win32ScreencapAction::MoveCursorAndSendHover, target_x, target_y, settle_delay_ms };
    case Win32ScreencapInputType::WindowPos:
        if (main_screen) {
            return { Win32ScreencapAction::AlignWindowAndSendHover, target_x, target_y, settle_delay_ms };
        }
        return { Win32ScreencapAction::ParkWindowAndSendLeave, 0, 0, settle_delay_ms };
    case Win32ScreencapInputType::None:
    default:
        return {};
    }
}

constexpr int64_t calculate_offscreen_window_top(int virtual_screen_top, int window_height, int padding)
{
    return static_cast<int64_t>(virtual_screen_top) - window_height - padding;
}

constexpr bool
    should_restore_cursor_position(int saved_x, int saved_y, int target_x, int target_y, int current_x, int current_y)
{
    return (saved_x != target_x || saved_y != target_y) && current_x == target_x && current_y == target_y;
}

template <
    typename SaveCursor,
    typename SaveWindow,
    typename MoveCursor,
    typename AlignWindow,
    typename ParkWindow,
    typename SendHover,
    typename SendLeave,
    typename Wait>
bool prepare_win32_screencap(
    const Win32ScreencapPlan& plan,
    SaveCursor&& save_cursor,
    SaveWindow&& save_window,
    MoveCursor&& move_cursor,
    AlignWindow&& align_window,
    ParkWindow&& park_window,
    SendHover&& send_hover,
    SendLeave&& send_leave,
    Wait&& wait)
{
    bool prepared = false;
    switch (plan.action) {
    case Win32ScreencapAction::None:
        prepared = true;
        break;
    case Win32ScreencapAction::SendHover:
        prepared = send_hover(plan.target_x, plan.target_y);
        break;
    case Win32ScreencapAction::MoveCursorAndSendHover:
        prepared =
            save_cursor() && move_cursor(plan.target_x, plan.target_y) && send_hover(plan.target_x, plan.target_y);
        break;
    case Win32ScreencapAction::AlignWindowAndSendHover:
        prepared =
            save_window() && align_window(plan.target_x, plan.target_y) && send_hover(plan.target_x, plan.target_y);
        break;
    case Win32ScreencapAction::ParkWindowAndSendLeave:
        prepared = save_window() && park_window() && send_leave();
        break;
    }

    if (prepared && plan.settle_delay_ms > 0) {
        wait(plan.settle_delay_ms);
    }
    return prepared;
}

template <typename Prepare, typename Capture, typename Restore>
bool execute_win32_screencap(const Win32ScreencapPlan& plan, Prepare&& prepare, Capture&& capture, Restore&& restore)
{
    if (!prepare(plan)) {
        restore();
        return false;
    }

    const bool result = capture();
    restore();
    return result;
}
} // namespace asst
