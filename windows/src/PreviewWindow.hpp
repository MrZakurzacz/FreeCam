#pragma once

#include "H264Decoder.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class PreviewWindow {
public:
    PreviewWindow();
    ~PreviewWindow();

    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    bool start();
    void stop();
    void present(DecodedFrame&& frame);

private:
    static LRESULT CALLBACK windowProc(
        HWND hwnd,
        UINT message,
        WPARAM wparam,
        LPARAM lparam
    );

    void run();
    void paint(HWND hwnd);

    std::thread thread_;
    std::atomic_bool running_ = false;
    HWND hwnd_ = nullptr;

    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    bool startup_complete_ = false;
    bool startup_success_ = false;

    std::mutex frame_mutex_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> bgra_;
};
