#pragma once

#include <chrono>

namespace asst
{
struct Win32ScreencapPreparation
{
    int screen_width = 0;
    int screen_height = 0;
    int virtual_screen_height = 0;
    bool main_screen_recognition = false;
    bool with_window_pos = false;
};

template <typename SaveWindowPosition, typename TouchMove, typename TouchUp, typename Wait>
void prepare_win32_screencap(
    const Win32ScreencapPreparation& preparation,
    SaveWindowPosition&& save_window_position,
    TouchMove&& touch_move,
    TouchUp&& touch_up,
    Wait&& wait)
{
    if (preparation.screen_height <= 0) {
        return;
    }

    if (preparation.main_screen_recognition) {
        // 主界面情况下鼠标移动到窗口中心，等待主界面的视差动画，300ms
        touch_move(0, preparation.screen_width / 2, preparation.screen_height / 2, 0);
        if (preparation.with_window_pos) {
            touch_up(0);
        }
        wait(std::chrono::milliseconds(300));
    }
    else if (preparation.with_window_pos) {
        // 在 WindowPos 输入模式下，非主界面识别把窗口移到屏幕外
        save_window_position();
        touch_move(0, 0, preparation.screen_height + preparation.virtual_screen_height + 100, 0);
        touch_up(0);
    }
    else {
        // 其他输入模式下把鼠标移到窗口左下角
        touch_move(0, 0, preparation.screen_height - 1, 0);
    }
}
} // namespace asst
