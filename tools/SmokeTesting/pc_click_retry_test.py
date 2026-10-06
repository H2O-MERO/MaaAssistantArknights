"""Run PC click retry integration checks against an off-screen test window.

Requires Windows, Pillow, and a Debug MaaCore build. No game window is used.
"""

import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import sys
import tempfile
import threading
import time

from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
from maa_core_eval import CoreEval  # noqa: E402

WIDTH, HEIGHT = 1280, 720
user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
WNDPROC = ctypes.WINFUNCTYPE(
    ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
)


class WindowClass(ctypes.Structure):
    _fields_ = [
        ("style", wintypes.UINT),
        ("proc", WNDPROC),
        ("class_extra", ctypes.c_int),
        ("window_extra", ctypes.c_int),
        ("instance", wintypes.HINSTANCE),
        ("icon", wintypes.HICON),
        ("cursor", wintypes.HANDLE),
        ("background", wintypes.HBRUSH),
        ("menu", wintypes.LPCWSTR),
        ("name", wintypes.LPCWSTR),
    ]


class BitmapHeader(ctypes.Structure):
    _fields_ = [
        ("size", wintypes.DWORD),
        ("width", wintypes.LONG),
        ("height", wintypes.LONG),
        ("planes", wintypes.WORD),
        ("bits", wintypes.WORD),
        ("compression", wintypes.DWORD),
        ("image_size", wintypes.DWORD),
        ("x_density", wintypes.LONG),
        ("y_density", wintypes.LONG),
        ("colors", wintypes.DWORD),
        ("important", wintypes.DWORD),
    ]


user32.DefWindowProcW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.CreateWindowExW.argtypes = [
    wintypes.DWORD,
    wintypes.LPCWSTR,
    wintypes.LPCWSTR,
    wintypes.DWORD,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    wintypes.HWND,
    wintypes.HMENU,
    wintypes.HINSTANCE,
    wintypes.LPVOID,
]
user32.CreateWindowExW.restype = wintypes.HWND
user32.RegisterClassW.argtypes = [ctypes.POINTER(WindowClass)]
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
user32.ValidateRect.argtypes = [wintypes.HWND, ctypes.c_void_p]
user32.InvalidateRect.argtypes = [wintypes.HWND, ctypes.c_void_p, wintypes.BOOL]
user32.PostMessageW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
gdi32.SetDIBitsToDevice.argtypes = [
    wintypes.HDC,
    ctypes.c_int,
    ctypes.c_int,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.c_int,
    ctypes.c_int,
    wintypes.UINT,
    wintypes.UINT,
    ctypes.c_void_p,
    ctypes.POINTER(BitmapHeader),
    wintypes.UINT,
]


def make_template(color):
    image = Image.new("RGB", (48, 48), color)
    draw = ImageDraw.Draw(image)
    draw.rectangle((4, 6, 20, 39), fill="white")
    draw.line((25, 4, 43, 41), fill="black", width=6)
    return image


class TestWindow:
    def __init__(self, template, goal):
        self.template, self.goal = template, goal
        self.ready = threading.Event()
        self.proc = WNDPROC(self.window_proc)
        self.name = "MaaPcClickRetrySmokeTest"
        self.header = BitmapHeader(40, WIDTH, -HEIGHT, 1, 32)
        self.reset("initial_success")
        self.thread = threading.Thread(target=self.message_loop, daemon=True)
        self.thread.start()
        if not self.ready.wait(5) or not self.hwnd:
            raise RuntimeError("Failed to create the test window")

    def reset(self, scenario):
        self.scenario = scenario
        self.clicks = []
        self.button_x = 200
        self.success = False
        self.unknown = False
        self.draw()

    def draw(self):
        image = Image.new("RGB", (WIDTH, HEIGHT), (37, 49, 61))
        if self.success:
            image.paste(self.goal, (1000, 500))
        if not self.unknown and (
            not self.success or self.scenario == "success_with_source"
        ):
            image.paste(self.template, (self.button_x, 200))
        self.pixels = ctypes.create_string_buffer(
            image.convert("RGBA").tobytes("raw", "BGRA")
        )
        self.image = image
        if getattr(self, "hwnd", None):
            user32.InvalidateRect(self.hwnd, None, False)

    def window_proc(self, hwnd, message, wparam, lparam):
        if message in (0xF, 0x317, 0x318):  # WM_PAINT / WM_PRINT / WM_PRINTCLIENT
            hdc = user32.GetDC(hwnd) if message == 0xF else wparam
            gdi32.SetDIBitsToDevice(
                hdc,
                0,
                0,
                WIDTH,
                HEIGHT,
                0,
                0,
                0,
                HEIGHT,
                self.pixels,
                ctypes.byref(self.header),
                0,
            )
            if message == 0xF:
                user32.ReleaseDC(hwnd, hdc)
                user32.ValidateRect(hwnd, None)
            return 0
        if message == 0x201:  # WM_LBUTTONDOWN
            x, y = lparam & 0xFFFF, (lparam >> 16) & 0xFFFF
            self.clicks.append((x, y))
            if self.button_x <= x < self.button_x + 48 and 200 <= y < 248:
                if self.scenario == "moving_button" and len(self.clicks) == 1:
                    self.button_x = 800
                elif self.scenario == "unknown_page":
                    self.unknown = True
                elif self.scenario not in ("always_fails", "disabled"):
                    self.success = True
                self.draw()
            return 0
        if message == 0x10:  # WM_CLOSE
            user32.DestroyWindow(hwnd)
            return 0
        if message == 0x2:  # WM_DESTROY
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, message, wparam, lparam)

    def message_loop(self):
        window_class = WindowClass(proc=self.proc, name=self.name)
        if not user32.RegisterClassW(ctypes.byref(window_class)):
            self.hwnd = None
            self.ready.set()
            return
        # Non-activating popup, outside the visible desktop.
        self.hwnd = user32.CreateWindowExW(
            0x08000080,
            self.name,
            self.name,
            0x90000000,
            -16000,
            -16000,
            WIDTH,
            HEIGHT,
            None,
            None,
            None,
            None,
        )
        self.ready.set()
        message = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(message))
            user32.DispatchMessageW(ctypes.byref(message))

    def close(self):
        user32.PostMessageW(self.hwnd, 0x10, 0, 0)
        self.thread.join(5)


class TestCore(CoreEval):
    def __init__(self, root):
        self.events = []
        super().__init__(user_dir=root / "logs")
        self._lib.AsstAttachWindow.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
        ]
        self._lib.AsstAttachWindow.restype = ctypes.c_bool

    def _on_callback(self, msg, details):
        self.events.append((msg, json.loads(details.decode("utf-8"))))
        super()._on_callback(msg, details)


def main():
    user32.SetProcessDPIAware()
    test_root = REPO_ROOT / "build" / "_pc_retry_validation"
    test_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="run-", dir=test_root))
    resources = root / "resource"
    (resources / "tasks").mkdir(parents=True)
    (resources / "template").mkdir()
    source, goal = make_template("red"), make_template("green")
    source.save(resources / "template" / "RetryTestSource.png")
    goal.save(resources / "template" / "RetryTestGoal.png")
    general = json.loads(
        (REPO_ROOT / "resource" / "config.json").read_text(encoding="utf-8")
    )
    general["options"]["taskDelay"] = 20
    (resources / "config.json").write_text(json.dumps(general), encoding="utf-8")
    window = TestWindow(source, goal)
    core = TestCore(root)
    try:
        for scenario, expected_clicks, retry_states in [
            ("initial_success", 1, []),
            ("moving_button", 2, ["retrying", "succeeded"]),
            ("always_fails", 4, ["retrying", "retrying", "retrying", "failed"]),
            ("unknown_page", 1, []),
            ("success_with_source", 1, []),
            ("disabled", 1, []),
        ]:
            window.reset(scenario)
            tasks = {
                "RetryTestSource": {
                    "action": "ClickSelf",
                    "roi": [0, 0, WIDTH, HEIGHT],
                    "cache": True,
                    "postDelay": 10,
                    "templThreshold": 0.99,
                    "pcClickRetryTimes": 0 if scenario == "disabled" else 3,
                    "next": ["RetryTestGoal"],
                },
                "RetryTestGoal": {
                    "action": "Stop",
                    "roi": [950, 450, 200, 200],
                    "templThreshold": 0.99,
                },
            }
            (resources / "tasks" / "tasks.json").write_text(
                json.dumps(tasks), encoding="utf-8"
            )
            assert core._lib.AsstLoadResource(str(root).encode("utf-8"))
            fixture = root / "fixture.png"
            window.image.save(fixture)
            report = core.report([fixture], ["RetryTestSource"])
            assert report[0]["results"][0]["hit"], report
            # The test window reads message coordinates; plain PostMessage avoids
            # depending on the interactive desktop's physical cursor position.
            assert core._lib.AsstAttachWindow(core._handle, window.hwnd, 16, 4, 2)
            core.events.clear()
            params = json.dumps({"task_names": ["RetryTestSource"]}).encode()
            assert core._lib.AsstAppendTask(core._handle, b"Custom", params)
            assert core._lib.AsstStart(core._handle)
            deadline = time.monotonic() + 20
            while core._lib.AsstRunning(core._handle) and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not core._lib.AsstRunning(core._handle), scenario
            time.sleep(0.1)
            retries = [
                event["details"]
                for _, event in core.events
                if event.get("what") == "PcClickRetry"
            ]
            starts = [
                event
                for msg, event in core.events
                if msg == 20001
                and event.get("details", {}).get("task") == "RetryTestSource"
            ]
            assert len(window.clicks) == expected_clicks, (scenario, window.clicks)
            assert len(starts) == 1, (scenario, starts)
            assert [event["state"] for event in retries] == retry_states, (
                scenario,
                retries,
            )
            if scenario == "moving_button":
                assert window.clicks[1][0] >= 800, window.clicks
            print(
                f"PASS: {scenario}: clicks={expected_clicks}, states={retry_states}",
                flush=True,
            )
    finally:
        core.close()
        window.close()


if __name__ == "__main__":
    main()
