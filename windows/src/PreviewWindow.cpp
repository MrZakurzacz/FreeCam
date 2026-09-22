#include "PreviewWindow.hpp"

#include <algorithm>
#include <iostream>

namespace {

constexpr wchar_t kWindowClassName[] =
    L"FreeCamPreviewWindowClass";
constexpr wchar_t kWindowTitle[] =
    L"FreeCam Preview";

} // namespace

PreviewWindow::PreviewWindow() = default;

PreviewWindow::~PreviewWindow() {
    stop();
}

bool PreviewWindow::start() {
    if (running_) {
        return true;
    }

    {
        std::lock_guard lock(startup_mutex_);
        startup_complete_ = false;
        startup_success_ = false;
    }

    running_ = true;
    thread_ = std::thread(&PreviewWindow::run, this);

    std::unique_lock lock(startup_mutex_);
    startup_cv_.wait(
        lock,
        [this] { return startup_complete_; }
    );

    return startup_success_;
}

void PreviewWindow::stop() {
    if (!running_ && !thread_.joinable()) {
        return;
    }

    running_ = false;

    if (hwnd_) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    hwnd_ = nullptr;
}

void PreviewWindow::present(DecodedFrame&& frame) {
    {
        std::lock_guard lock(frame_mutex_);
        width_ = frame.width;
        height_ = frame.height;
        bgra_ = std::move(frame.bgra);
    }

    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PreviewWindow::run() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &PreviewWindow::windowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&window_class)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            std::cerr << "RegisterClassExW failed: "
                      << error << "\n";

            {
                std::lock_guard lock(startup_mutex_);
                startup_complete_ = true;
                startup_success_ = false;
            }
            startup_cv_.notify_one();
            running_ = false;
            return;
        }
    }

    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1000,
        620,
        nullptr,
        nullptr,
        instance,
        this
    );

    if (!window) {
        std::cerr << "CreateWindowExW failed: "
                  << GetLastError() << "\n";

        {
            std::lock_guard lock(startup_mutex_);
            startup_complete_ = true;
            startup_success_ = false;
        }
        startup_cv_.notify_one();
        running_ = false;
        return;
    }

    hwnd_ = window;

    {
        std::lock_guard lock(startup_mutex_);
        startup_complete_ = true;
        startup_success_ = true;
    }
    startup_cv_.notify_one();

    MSG message{};
    while (running_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    running_ = false;
    hwnd_ = nullptr;
}

LRESULT CALLBACK PreviewWindow::windowProc(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam
) {
    PreviewWindow* self = reinterpret_cast<PreviewWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA)
    );

    if (message == WM_NCCREATE) {
        auto* create =
            reinterpret_cast<CREATESTRUCTW*>(lparam);

        self = static_cast<PreviewWindow*>(
            create->lpCreateParams
        );

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self)
        );
    }

    switch (message) {
    case WM_PAINT:
        if (self) {
            self->paint(hwnd);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (self) {
            self->running_ = false;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(
        hwnd,
        message,
        wparam,
        lparam
    );
}

void PreviewWindow::paint(HWND hwnd) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);

    RECT client{};
    GetClientRect(hwnd, &client);

    FillRect(
        dc,
        &client,
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))
    );

    std::lock_guard lock(frame_mutex_);

    if (!bgra_.empty() && width_ > 0 && height_ > 0) {
        const int client_width =
            client.right - client.left;
        const int client_height =
            client.bottom - client.top;

        const double source_aspect =
            static_cast<double>(width_) /
            static_cast<double>(height_);

        int destination_width = client_width;
        int destination_height =
            static_cast<int>(
                destination_width / source_aspect
            );

        if (destination_height > client_height) {
            destination_height = client_height;
            destination_width =
                static_cast<int>(
                    destination_height * source_aspect
                );
        }

        const int x =
            (client_width - destination_width) / 2;
        const int y =
            (client_height - destination_height) / 2;

        BITMAPINFO info{};
        info.bmiHeader.biSize =
            sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth =
            static_cast<LONG>(width_);
        info.bmiHeader.biHeight =
            -static_cast<LONG>(height_);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(dc, HALFTONE);

        StretchDIBits(
            dc,
            x,
            y,
            destination_width,
            destination_height,
            0,
            0,
            static_cast<int>(width_),
            static_cast<int>(height_),
            bgra_.data(),
            &info,
            DIB_RGB_COLORS,
            SRCCOPY
        );
    }

    EndPaint(hwnd, &paint);
}
