#ifdef _WIN32

#include "Win32Controller.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <thread>

#include "Config/GeneralConfig.h"
#include "SwipeHelper.hpp"
#include "Utils/Logger.hpp"
#include "Utils/WorkingDir.hpp"

namespace asst
{
namespace
{
constexpr auto WindowMoveSettleTimeout = std::chrono::milliseconds(250);
constexpr auto WindowMovePollInterval = std::chrono::milliseconds(5);
constexpr auto WindowPosTrackingSettleDelay = std::chrono::milliseconds(40);
constexpr LONG OffscreenWindowPadding = 8;

class ScopedThreadDpiAwareness
{
public:
    ScopedThreadDpiAwareness()
    {
        m_previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    ~ScopedThreadDpiAwareness()
    {
        if (m_previous) {
            SetThreadDpiAwarenessContext(m_previous);
        }
    }

    ScopedThreadDpiAwareness(const ScopedThreadDpiAwareness&) = delete;
    ScopedThreadDpiAwareness& operator=(const ScopedThreadDpiAwareness&) = delete;

private:
    DPI_AWARENESS_CONTEXT m_previous = nullptr;
};
} // namespace

Win32Controller::Win32Controller(const AsstCallback& callback, Assistant* inst) :
    InstHelper(inst),
    m_callback(callback),
    m_loader(std::make_unique<Win32ControlUnitLoader>())
{
    LogTraceFunction;
}

Win32Controller::~Win32Controller()
{
    LogTraceFunction;

    restore_screencap_position();

    if (m_unit_handle && m_loader) {
        m_loader->destroy(m_unit_handle);
        m_unit_handle = nullptr;
    }
}

bool Win32Controller::attach(
    void* hwnd,
    Win32ScreencapMethod screencap_method,
    Win32InputMethod mouse_method,
    Win32InputMethod keyboard_method)
{
    LogTraceFunction;

    m_inited = false;

    restore_screencap_position();

    // 销毁旧的控制单元
    if (m_unit_handle && m_loader) {
        m_loader->destroy(m_unit_handle);
        m_unit_handle = nullptr;
    }

    {
        std::scoped_lock lock(m_position_mutex);
        m_window_rect_saved = false;
        m_cursor_position_saved = false;
    }

    m_hwnd = hwnd;
    m_screencap_method = screencap_method;
    m_mouse_method = mouse_method;
    m_keyboard_method = keyboard_method;
    if (is_window_pos_input()) {
        save_window_position();
    }

    // 加载 DLL
    if (!m_loader->loaded()) {
        auto dll_path = "MaaWin32ControlUnit";
        if (!m_loader->load(dll_path)) {
            Log.error("Failed to load MaaWin32ControlUnit.dll");
            return false;
        }
    }

    // 创建控制单元
    m_unit_handle = m_loader->create(hwnd, screencap_method, mouse_method, keyboard_method);
    if (!m_unit_handle) {
        Log.error("Failed to create Win32ControlUnit");
        return false;
    }

    // 连接
    if (!unit_connect()) {
        Log.error("Failed to connect Win32ControlUnit");
        m_loader->destroy(m_unit_handle);
        m_unit_handle = nullptr;
        return false;
    }

    // 获取 UUID
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit->request_uuid(m_uuid)) {
        std::stringstream ss;
        ss << hwnd;
        m_uuid = ss.str();
    }

    // 尝试截图获取屏幕分辨率
    cv::Mat image;
    if (unit_screencap(image)) {
        m_screen_size = { image.cols, image.rows };
        Log.info("Screen size:", m_screen_size.first, "x", m_screen_size.second);
    }

    m_inited = true;
    return true;
}

bool Win32Controller::connect(
    const std::string& adb_path [[maybe_unused]],
    const std::string& address [[maybe_unused]],
    const std::string& config [[maybe_unused]])
{
    Log.error("Win32Controller does not support connect(), use attach() instead");
    return false;
}

bool Win32Controller::inited() const noexcept
{
    return m_inited && m_unit_handle;
}

const std::string& Win32Controller::get_uuid() const
{
    return m_uuid;
}

bool Win32Controller::screencap(cv::Mat& image_payload, bool allow_reconnect [[maybe_unused]])
{
    LogTraceFunction;

    std::scoped_lock lock(m_position_mutex);
    ScopedThreadDpiAwareness dpi_awareness;

    // 先结束上一次输入跟踪并恢复位置，避免它与本次截图的直接窗口移动竞争。
    restore_screencap_position_locked();

    auto hwnd = static_cast<HWND>(m_hwnd);
    constexpr auto PseudoMinimizeMethods = Win32Screencap::FramePool | Win32Screencap::PrintWindow;
    if (hwnd && IsWindow(hwnd) && IsIconic(hwnd) && (m_screencap_method & PseudoMinimizeMethods) != 0) {
        // 这些截图方式会在第一次截图时把真实最小化切换为伪最小化，过渡帧不能交给识别层。
        cv::Mat transition_frame;
        if (!unit_screencap(transition_frame)) {
            return false;
        }
    }

    const auto plan = make_win32_screencap_plan(
        m_screen_size.first,
        m_screen_size.second,
        m_main_screen_recognition,
        get_screencap_input_type());

    const bool result = execute_win32_screencap(
        plan,
        [this](const Win32ScreencapPlan& preparation) { return prepare_screencap(preparation); },
        [this, &image_payload]() { return unit_screencap(image_payload); },
        [this]() { restore_screencap_position_locked(); });

    if (result && m_screen_size.first == 0) {
        m_screen_size = { image_payload.cols, image_payload.rows };
    }

    return result;
}

bool Win32Controller::start_game(const std::string& client_type [[maybe_unused]])
{
    Log.warn("start_game is not supported on Win32Controller");
    return false;
}

bool Win32Controller::stop_game(const std::string& client_type [[maybe_unused]])
{
    LogTraceFunction;

    if (!m_hwnd) {
        Log.info("No window handle available, game may already be closed");
        return true;
    }

    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (!IsWindow(hwnd)) {
        Log.info("Invalid or stale window handle, game may already be closed, hwnd:", m_hwnd);
        return true;
    }

    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (tid == 0) {
        DWORD error = GetLastError();
        Log.error("Failed to get thread/process id from hwnd, hwnd:", m_hwnd, "last_error:", error);
        return false;
    }

    if (pid == 0) {
        Log.error("Failed to get process id from hwnd, hwnd:", m_hwnd);
        return false;
    }

    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!hProcess) {
        DWORD error = GetLastError();
        Log.error("Failed to open process, pid:", pid, "last_error:", error);
        return false;
    }

    if (PostMessage(hwnd, WM_CLOSE, 0, 0)) {
        DWORD wait_result = WaitForSingleObject(hProcess, 5000);
        if (wait_result == WAIT_OBJECT_0) {
            CloseHandle(hProcess);
            Log.info("Game process closed gracefully, pid:", pid);
            return true;
        }
    }

    BOOL ok = TerminateProcess(hProcess, 0);
    if (!ok) {
        DWORD error = GetLastError();
        CloseHandle(hProcess);
        Log.error("Failed to terminate process, pid:", pid, "last_error:", error);
        return false;
    }

    DWORD wait_result = WaitForSingleObject(hProcess, 5000);
    CloseHandle(hProcess);

    if (wait_result == WAIT_TIMEOUT) {
        Log.error("Terminate process timed out, pid:", pid);
        return false;
    }

    if (wait_result == WAIT_FAILED) {
        DWORD error = GetLastError();
        Log.error("Wait for process termination failed, pid:", pid, "last_error:", error);
        return false;
    }

    Log.info("Game process terminated, pid:", pid);
    return true;
}

bool Win32Controller::click(const Point& p)
{
    LogTraceFunction;
    Log.trace("Win32Controller click:", p);

    // MaaWin32ControlUnit 返回 MaaControllerFeature_UseMouseDownAndUpInsteadOfClick
    // 需要使用 touch_down/touch_up 替代 click
    if (!unit_touch_down(0, p.x, p.y, 0)) {
        restore_window_after_input();
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const bool up = unit_touch_up(0);
    restore_window_after_input();
    return up;
}

bool Win32Controller::input(const std::string& text)
{
    LogTraceFunction;
    return unit_input_text(text);
}

bool Win32Controller::swipe(
    const Point& p1,
    const Point& p2,
    int duration,
    bool extra_swipe,
    double slope_in,
    double slope_out,
    bool with_pause [[maybe_unused]])
{
    LogTraceFunction;

    int x1 = p1.x, y1 = p1.y;
    int x2 = p2.x, y2 = p2.y;

    const auto width = m_screen_size.first;
    const auto height = m_screen_size.second;

    // 起点不能在屏幕外，但是终点可以
    if (width > 0 && height > 0) {
        if (x1 < 0 || x1 >= width || y1 < 0 || y1 >= height) {
            Log.warn("swipe point1 is out of range", x1, y1);
            x1 = std::clamp(x1, 0, width - 1);
            y1 = std::clamp(y1, 0, height - 1);
        }
    }

    Log.trace("Win32Controller swipe", p1, p2, duration, extra_swipe, slope_in, slope_out);

    // MaaWin32ControlUnit 返回 MaaControllerFeature_UseMouseDownAndUpInsteadOfClick
    // 需要使用 touch_down/touch_move/touch_up 实现滑动
    if (!unit_touch_down(0, x1, y1, 0)) {
        restore_window_after_input();
        return false;
    }

    const auto& opt = Config.get_options();
    int actual_duration = duration > 0 ? duration : opt.minitouch_swipe_default_duration;

    auto bounds_check = [width, height](int x, int y) {
        if (width <= 0 || height <= 0) {
            return true;
        }
        return x >= 0 && x <= width && y >= 0 && y <= height;
    };

    auto move_func = [this](int x, int y) {
        return unit_touch_move(0, x, y, 0);
    };

    auto do_swipe = [&](int _x1, int _y1, int _x2, int _y2, int _duration) {
        return interpolate_swipe(
            _x1,
            _y1,
            _x2,
            _y2,
            _duration,
            DefaultSwipeDelay,
            slope_in,
            slope_out,
            move_func,
            bounds_check);
    };

    if (!do_swipe(x1, y1, x2, y2, actual_duration)) {
        unit_touch_up(0);
        restore_window_after_input();
        return false;
    }

    if (extra_swipe && opt.minitouch_extra_swipe_duration > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.minitouch_swipe_extra_end_delay));
        do_swipe(x2, y2, x2, y2 - opt.minitouch_extra_swipe_dist, opt.minitouch_extra_swipe_duration);
    }

    const bool up = unit_touch_up(0);
    restore_window_after_input();
    return up;
}

bool Win32Controller::inject_input_event(const InputEvent& event)
{
    LogTraceFunction;

    switch (event.type) {
    case InputEvent::Type::TOUCH_DOWN:
        return unit_touch_down(event.pointerId, event.point.x, event.point.y, 0);
    case InputEvent::Type::TOUCH_UP: {
        const bool up = unit_touch_up(event.pointerId);
        restore_window_after_input();
        return up;
    }
    case InputEvent::Type::TOUCH_MOVE:
        return unit_touch_move(event.pointerId, event.point.x, event.point.y, 0);
    case InputEvent::Type::KEY_DOWN: {
        auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
        return unit ? unit->key_down(event.keycode) : false;
    }
    case InputEvent::Type::KEY_UP: {
        auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
        return unit ? unit->key_up(event.keycode) : false;
    }
    case InputEvent::Type::WAIT_MS:
        std::this_thread::sleep_for(std::chrono::milliseconds(event.milisec));
        return true;
    case InputEvent::Type::TOUCH_RESET:
    case InputEvent::Type::COMMIT:
        return true;
    case InputEvent::Type::UNKNOWN:
    default:
        Log.error("unknown input event type");
        return false;
    }
}

bool Win32Controller::press_esc()
{
    LogTraceFunction;
    return unit_click_key(VK_ESCAPE); // VK_ESCAPE = 0x1B, defined in WinUser.h
}

void Win32Controller::set_main_screen_recognition(bool on)
{
    m_main_screen_recognition = on;
}

Win32ScreencapInputType Win32Controller::get_screencap_input_type() const noexcept
{
    return classify_win32_screencap_input(m_mouse_method);
}

bool Win32Controller::prepare_screencap(const Win32ScreencapPlan& plan)
{
    return prepare_win32_screencap(
        plan,
        [this]() { return save_cursor_position(); },
        [this]() { return save_window_position_locked(); },
        [this](int x, int y) { return move_cursor_for_screencap(x, y); },
        [this](int x, int y) { return align_window_for_screencap(x, y); },
        [this]() { return park_window_for_screencap(); },
        [this](int x, int y) { return send_hover_message(x, y); },
        [this]() { return send_mouse_leave_message(); },
        [](int delay_ms) { std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms)); });
}

bool Win32Controller::send_hover_message(int x, int y)
{
    auto hwnd = static_cast<HWND>(m_hwnd);
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y), SMTO_ABORTIFHUNG, 25, &result)) {
        Log.warn("Failed to send Win32 hover message before screencap", GetLastError());
        return false;
    }
    return true;
}

bool Win32Controller::send_mouse_leave_message()
{
    auto hwnd = static_cast<HWND>(m_hwnd);
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(hwnd, WM_MOUSELEAVE, 0, 0, SMTO_ABORTIFHUNG, 25, &result)) {
        Log.warn("Failed to send Win32 mouse leave message before screencap", GetLastError());
        return false;
    }
    return true;
}

bool Win32Controller::save_cursor_position()
{
    if (!GetCursorPos(&m_saved_cursor_position)) {
        return false;
    }
    m_cursor_position_saved = true;
    return true;
}

bool Win32Controller::move_cursor_for_screencap(int x, int y)
{
    auto hwnd = static_cast<HWND>(m_hwnd);
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    POINT target = { x, y };
    if (!ClientToScreen(hwnd, &target)) {
        return false;
    }

    m_screencap_cursor_target = target;
    if (!SetCursorPos(target.x, target.y)) {
        Log.warn("Failed to move physical cursor before Win32 screencap", GetLastError());
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + WindowMoveSettleTimeout;
    do {
        POINT current = {};
        if (GetCursorPos(&current) && current.x == target.x && current.y == target.y) {
            return true;
        }
        std::this_thread::sleep_for(WindowMovePollInterval);
    } while (std::chrono::steady_clock::now() < deadline);

    Log.warn("Physical cursor did not settle before Win32 screencap");
    return false;
}

bool Win32Controller::align_window_for_screencap(int x, int y)
{
    auto hwnd = static_cast<HWND>(m_hwnd);
    POINT cursor = {};
    POINT client_origin = {};
    RECT window_rect = {};
    if (!hwnd || !IsWindow(hwnd) || !GetCursorPos(&cursor) || !ClientToScreen(hwnd, &client_origin) ||
        !GetWindowRect(hwnd, &window_rect)) {
        return false;
    }

    const int64_t border_x = static_cast<int64_t>(client_origin.x) - window_rect.left;
    const int64_t border_y = static_cast<int64_t>(client_origin.y) - window_rect.top;
    const int64_t desired_left = static_cast<int64_t>(cursor.x) - x - border_x;
    const int64_t desired_top = static_cast<int64_t>(cursor.y) - y - border_y;
    const auto clamp_long = [](int64_t value) {
        return static_cast<LONG>(std::clamp(
            value,
            static_cast<int64_t>(std::numeric_limits<LONG>::min()),
            static_cast<int64_t>(std::numeric_limits<LONG>::max())));
    };

    return move_window_and_wait(clamp_long(desired_left), clamp_long(desired_top));
}

bool Win32Controller::park_window_for_screencap()
{
    auto hwnd = static_cast<HWND>(m_hwnd);
    RECT window_rect = {};
    if (!hwnd || !IsWindow(hwnd) || !GetWindowRect(hwnd, &window_rect)) {
        return false;
    }

    const LONG window_height = window_rect.bottom - window_rect.top;
    if (window_height <= 0) {
        return false;
    }

    const int64_t desired_top =
        calculate_offscreen_window_top(GetSystemMetrics(SM_YVIRTUALSCREEN), window_height, OffscreenWindowPadding);
    const LONG top = static_cast<LONG>(std::clamp(
        desired_top,
        static_cast<int64_t>(std::numeric_limits<LONG>::min()),
        static_cast<int64_t>(std::numeric_limits<LONG>::max())));

    return move_window_and_wait(window_rect.left, top);
}

bool Win32Controller::move_window_and_wait(LONG left, LONG top)
{
    auto hwnd = static_cast<HWND>(m_hwnd);
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    if (!SetWindowPos(
            hwnd,
            nullptr,
            left,
            top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS)) {
        Log.warn("Failed to move Win32 window before screencap", GetLastError());
        return false;
    }
    if (!wait_window_position(left, top)) {
        Log.warn("Win32 window did not settle before screencap", left, top);
        return false;
    }
    return true;
}

bool Win32Controller::wait_window_position(LONG left, LONG top) const
{
    auto hwnd = static_cast<HWND>(m_hwnd);
    const auto deadline = std::chrono::steady_clock::now() + WindowMoveSettleTimeout;
    do {
        RECT current = {};
        if (hwnd && GetWindowRect(hwnd, &current) && std::abs(static_cast<int64_t>(current.left) - left) <= 1 &&
            std::abs(static_cast<int64_t>(current.top) - top) <= 1) {
            return true;
        }
        std::this_thread::sleep_for(WindowMovePollInterval);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

void Win32Controller::restore_screencap_position()
{
    std::scoped_lock lock(m_position_mutex);
    ScopedThreadDpiAwareness dpi_awareness;
    restore_screencap_position_locked();
}

void Win32Controller::restore_screencap_position_locked()
{
    if (is_window_pos_input()) {
        restore_window_position_locked();
    }

    if (!m_cursor_position_saved) {
        return;
    }

    POINT current = {};
    if (GetCursorPos(&current) && should_restore_cursor_position(
                                      m_saved_cursor_position.x,
                                      m_saved_cursor_position.y,
                                      m_screencap_cursor_target.x,
                                      m_screencap_cursor_target.y,
                                      current.x,
                                      current.y)) {
        if (!SetCursorPos(m_saved_cursor_position.x, m_saved_cursor_position.y)) {
            Log.warn("Failed to restore physical cursor after Win32 screencap", GetLastError());
        }
    }
    else if (current.x != m_screencap_cursor_target.x || current.y != m_screencap_cursor_target.y) {
        // 用户在截图期间主动移动了鼠标，不覆盖用户的新位置。
        Log.debug("Skip restoring physical cursor because it was moved by the user");
    }
    m_cursor_position_saved = false;
}

void Win32Controller::save_window_position()
{
    std::scoped_lock lock(m_position_mutex);
    ScopedThreadDpiAwareness dpi_awareness;
    save_window_position_locked();
}

bool Win32Controller::save_window_position_locked()
{
    if (m_window_rect_saved) {
        return true;
    }
    if (!m_hwnd) {
        return false;
    }
    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (!IsWindow(hwnd) || !GetWindowRect(hwnd, &m_original_window_rect)) {
        return false;
    }
    m_window_rect_saved = true;
    return true;
}

void Win32Controller::restore_window_position()
{
    LogTraceFunction;
    std::scoped_lock lock(m_position_mutex);
    ScopedThreadDpiAwareness dpi_awareness;
    restore_window_position_locked();
}

void Win32Controller::restore_window_position_locked()
{
    // inactive 会结束 MaaFramework 的 WindowPos 跟踪并清理其内部保存的位置。
    if (is_window_pos_input()) {
        if (auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle); unit != nullptr) {
            unit->inactive();
        }
    }

    if (!m_window_rect_saved || !m_hwnd) {
        return;
    }

    HWND hwnd = static_cast<HWND>(m_hwnd);
    if (!IsWindow(hwnd)) {
        return;
    }

    RECT current = {};
    if (GetWindowRect(hwnd, &current) &&
        std::abs(static_cast<int64_t>(current.left) - m_original_window_rect.left) <= 1 &&
        std::abs(static_cast<int64_t>(current.top) - m_original_window_rect.top) <= 1) {
        // 析构和重新绑定路径会额外兜底恢复，窗口已归位时不再重复触发移动消息。
        return;
    }

    if (!SetWindowPos(
            hwnd,
            nullptr,
            m_original_window_rect.left,
            m_original_window_rect.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS)) {
        Log.warn("Failed to restore Win32 window position", GetLastError());
        return;
    }
    if (!wait_window_position(m_original_window_rect.left, m_original_window_rect.top)) {
        Log.warn(
            "Win32 window did not settle at its original position",
            m_original_window_rect.left,
            m_original_window_rect.top);
        return;
    }
}

void Win32Controller::restore_window_after_input()
{
    if (!is_window_pos_input()) {
        return;
    }

    // MaaFramework 的 WindowPos 追踪在 touch_up 后仍保留短暂宽限期，等待结束后再恢复。
    std::this_thread::sleep_for(WindowPosTrackingSettleDelay);
    restore_window_position();
}

bool Win32Controller::is_window_pos_input() const noexcept
{
    constexpr auto WindowPosMethods = Win32Input::SendMessageWithWindowPos | Win32Input::PostMessageWithWindowPos;
    return (m_mouse_method & WindowPosMethods) != 0;
}

ControlFeat::Feat Win32Controller::support_features() const noexcept
{
    return ControlFeat::PRECISE_SWIPE;
}

std::pair<int, int> Win32Controller::get_screen_res() const noexcept
{
    return m_screen_size;
}

void Win32Controller::callback(AsstMsg msg, const json::value& details)
{
    if (m_callback) {
        m_callback(msg, details, m_inst);
    }
}

bool Win32Controller::unit_connect()
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->connect();
}

bool Win32Controller::unit_screencap(cv::Mat& image)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->screencap(image);
}

bool Win32Controller::unit_click(int x, int y)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->click(x, y);
}

bool Win32Controller::unit_swipe(int x1, int y1, int x2, int y2, int duration)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->swipe(x1, y1, x2, y2, duration);
}

bool Win32Controller::unit_touch_down(int contact, int x, int y, int pressure)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->touch_down(contact, x, y, pressure);
}

bool Win32Controller::unit_touch_move(int contact, int x, int y, int pressure)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->touch_move(contact, x, y, pressure);
}

bool Win32Controller::unit_touch_up(int contact)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->touch_up(contact);
}

bool Win32Controller::unit_input_text(const std::string& text)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }
    return unit->input_text(text);
}

bool Win32Controller::unit_click_key(int key)
{
    auto* unit = static_cast<MaaFwControlUnitAPI*>(m_unit_handle);
    if (!unit) {
        return false;
    }

    // MaaWin32ControlUnit 返回 MaaControllerFeature_UseKeyboardDownAndUpInsteadOfClick
    // 需要使用 key_down/key_up 替代 click_key
    if (!unit->key_down(key)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return unit->key_up(key);
}
} // namespace asst

#endif // _WIN32
