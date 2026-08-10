#pragma once

#ifdef _WIN32

#include <memory>
#include <mutex>
#include <string>

#include "MaaUtils/SafeWindows.hpp"

#include "Common/AsstMsg.h"
#include "ControllerAPI.h"
#include "InstHelper.h"
#include "Win32ControlUnitLoader.h"
#include "Win32ScreencapPreparation.hpp"

namespace asst
{
class Assistant;

class Win32Controller : public ControllerAPI, private InstHelper
{
public:
    Win32Controller(const AsstCallback& callback, Assistant* inst);
    virtual ~Win32Controller() override;

    Win32Controller(const Win32Controller&) = delete;
    Win32Controller& operator=(const Win32Controller&) = delete;
    Win32Controller(Win32Controller&&) = delete;
    Win32Controller& operator=(Win32Controller&&) = delete;

    // 绑定到窗口（替代 connect）
    bool attach(
        void* hwnd,
        Win32ScreencapMethod screencap_method,
        Win32InputMethod mouse_method,
        Win32InputMethod keyboard_method);

public: // ControllerAPI 接口
    virtual bool connect(const std::string& adb_path, const std::string& address, const std::string& config) override;
    virtual bool inited() const noexcept override;

    virtual const std::string& get_uuid() const override;

    virtual size_t get_pipe_data_size() const noexcept override { return 0; }

    virtual size_t get_version() const noexcept override { return 0; }

    virtual bool screencap(cv::Mat& image_payload, bool allow_reconnect = false) override;

    virtual bool start_game(const std::string& client_type) override;
    virtual bool stop_game(const std::string& client_type) override;

    virtual bool click(const Point& p) override;
    virtual bool input(const std::string& text) override;
    virtual bool swipe(
        const Point& p1,
        const Point& p2,
        int duration = 0,
        bool extra_swipe = false,
        double slope_in = 1,
        double slope_out = 1,
        bool with_pause = false) override;

    virtual bool inject_input_event(const InputEvent& event) override;

    virtual bool press_esc() override;
    virtual void set_main_screen_recognition(bool on) override;
    virtual void restore_window_position() override;
    virtual ControlFeat::Feat support_features() const noexcept override;

    virtual std::pair<int, int> get_screen_res() const noexcept override;

private:
    void callback(AsstMsg msg, const json::value& details);

    Win32ScreencapInputType get_screencap_input_type() const noexcept;
    bool prepare_screencap(const Win32ScreencapPlan& plan);
    bool send_hover_message(int x, int y);
    bool send_mouse_leave_message();
    bool save_cursor_position();
    bool move_cursor_for_screencap(int x, int y);
    bool align_window_for_screencap(int x, int y);
    bool park_window_for_screencap();
    bool move_window_and_wait(LONG left, LONG top);
    bool wait_window_position(LONG left, LONG top) const;
    void restore_screencap_position();
    void restore_screencap_position_locked();

    // 记录连接时的窗口位置，输入或截图完成后恢复
    void save_window_position();
    bool save_window_position_locked();
    void restore_window_position_locked();
    void restore_window_after_input();
    bool is_window_pos_input() const noexcept;

    // 封装 MaaWin32ControlUnit 的调用
    bool unit_connect();
    bool unit_screencap(cv::Mat& image);
    bool unit_click(int x, int y);
    bool unit_swipe(int x1, int y1, int x2, int y2, int duration);
    bool unit_touch_down(int contact, int x, int y, int pressure);
    bool unit_touch_move(int contact, int x, int y, int pressure);
    bool unit_touch_up(int contact);
    bool unit_input_text(const std::string& text);
    bool unit_click_key(int key);

private:
    static constexpr int DefaultSwipeDelay = 10; // ms

    AsstCallback m_callback = nullptr;
    std::unique_ptr<Win32ControlUnitLoader> m_loader;
    void* m_unit_handle = nullptr;
    void* m_hwnd = nullptr;

    bool m_inited = false;
    std::string m_uuid;
    std::pair<int, int> m_screen_size = { 0, 0 };

    Win32ScreencapMethod m_screencap_method = Win32Screencap::None;
    Win32InputMethod m_mouse_method = Win32Input::None;
    Win32InputMethod m_keyboard_method = Win32Input::None;

    bool m_main_screen_recognition = false;
    std::mutex m_position_mutex;

    RECT m_original_window_rect = { 0, 0, 0, 0 };
    bool m_window_rect_saved = false;

    POINT m_saved_cursor_position = { 0, 0 };
    POINT m_screencap_cursor_target = { 0, 0 };
    bool m_cursor_position_saved = false;
};
} // namespace asst

#endif // _WIN32
