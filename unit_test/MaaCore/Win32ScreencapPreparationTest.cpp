#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Controller/Win32ScreencapPreparation.hpp"

namespace
{
enum class ActionType
{
    SaveWindowPosition,
    TouchDown,
    TouchMove,
    TouchUp,
    Wait,
};

struct Action
{
    ActionType type = ActionType::Wait;
    int contact = 0;
};

class ActionRecorder
{
public:
    void save_window_position() { m_actions.emplace_back(ActionType::SaveWindowPosition); }

    bool touch_move(int contact, int, int, int)
    {
        m_actions.emplace_back(ActionType::TouchMove, contact);
        return true;
    }

    bool touch_up(int contact)
    {
        m_actions.emplace_back(ActionType::TouchUp, contact);
        return true;
    }

    void wait(std::chrono::milliseconds) { m_actions.emplace_back(ActionType::Wait); }

    const std::vector<Action>& actions() const { return m_actions; }

private:
    std::vector<Action> m_actions;
};

ActionRecorder record_preparation(const asst::Win32ScreencapPreparation& preparation)
{
    ActionRecorder recorder;
    asst::prepare_win32_screencap(
        preparation,
        [&]() { recorder.save_window_position(); },
        [&](int contact, int x, int y, int pressure) { return recorder.touch_move(contact, x, y, pressure); },
        [&](int contact) { return recorder.touch_up(contact); },
        [&](std::chrono::milliseconds duration) { recorder.wait(duration); });
    return recorder;
}

std::string describe_actions(const std::vector<Action>& actions)
{
    std::ostringstream output;
    for (size_t index = 0; index < actions.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }

        const auto& action = actions[index];
        switch (action.type) {
        case ActionType::SaveWindowPosition:
            output << "save_window_position";
            break;
        case ActionType::TouchDown:
            output << "touch_down(" << action.contact << ')';
            break;
        case ActionType::TouchMove:
            output << "touch_move(" << action.contact << ')';
            break;
        case ActionType::TouchUp:
            output << "touch_up(" << action.contact << ')';
            break;
        case ActionType::Wait:
            output << "wait";
            break;
        }
    }
    return output.str();
}

std::string pointer_contact_error(const std::vector<Action>& actions)
{
    // ControlUnit 的 touch_move/touch_up 必须位于同一 contact 的 touch_down/touch_up 序列中。
    std::unordered_set<int> active_contacts;
    for (const auto& action : actions) {
        switch (action.type) {
        case ActionType::TouchDown:
            if (!active_contacts.emplace(action.contact).second) {
                return "touch_down repeated for active contact " + std::to_string(action.contact);
            }
            break;
        case ActionType::TouchMove:
            if (!active_contacts.contains(action.contact)) {
                return "touch_move used without touch_down for contact " + std::to_string(action.contact);
            }
            break;
        case ActionType::TouchUp:
            if (active_contacts.erase(action.contact) == 0) {
                return "touch_up used without touch_down for contact " + std::to_string(action.contact);
            }
            break;
        case ActionType::SaveWindowPosition:
        case ActionType::Wait:
            break;
        }
    }
    if (!active_contacts.empty()) {
        return "pointer contact remains active after preparation";
    }
    return {};
}
} // namespace

TEST_CASE("Screencap preparation emits complete pointer contacts")
{
    struct TestCase
    {
        std::string_view name;
        bool main_screen_recognition = false;
        bool with_window_pos = false;
    };

    constexpr std::array TestCases = {
        TestCase { "main screen with WindowPos", true, true },
        TestCase { "other screen with WindowPos", false, true },
        TestCase { "main screen without WindowPos", true, false },
        TestCase { "other screen without WindowPos", false, false },
    };

    for (const auto& test_case : TestCases) {
        DYNAMIC_SECTION(test_case.name)
        {
            const auto recorder = record_preparation({
                .screen_width = 1280,
                .screen_height = 720,
                .virtual_screen_height = 1080,
                .main_screen_recognition = test_case.main_screen_recognition,
                .with_window_pos = test_case.with_window_pos,
            });

            const auto error = pointer_contact_error(recorder.actions());
            INFO("Recorded actions: " << describe_actions(recorder.actions()));
            INFO("Pointer protocol error: " << error);
            REQUIRE(error.empty());
        }
    }
}

TEST_CASE("WindowPos movement preserves a restorable window origin")
{
    const auto recorder = record_preparation({
        .screen_width = 1280,
        .screen_height = 720,
        .virtual_screen_height = 1080,
        .main_screen_recognition = true,
        .with_window_pos = true,
    });

    const auto& actions = recorder.actions();
    const auto movement = std::ranges::find(actions, ActionType::TouchMove, &Action::type);
    const auto saved_position = std::ranges::find(actions, ActionType::SaveWindowPosition, &Action::type);

    // 如果实现改为不通过 touch_move 移动窗口，则不需要由这里检查恢复点。
    if (movement == actions.end()) {
        SUCCEED("No WindowPos movement was requested during screencap preparation");
        return;
    }

    INFO("Recorded actions: " << describe_actions(actions));
    INFO("WindowPos touch_move can relocate the window, so its original position must be saved first");
    REQUIRE(saved_position != actions.end());
    REQUIRE(saved_position < movement);
}
