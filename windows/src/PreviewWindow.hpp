#pragma once

#include "H264Decoder.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct HWND__;

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
    static long long __stdcall windowProc(
        HWND__* hwnd,
        unsigned int message,
        unsigned long long wparam,
        long long lparam
    );

    void run();
    void paint(HWND__* hwnd);

    std::thread thread_;
    std::atomic_bool running_ = false;
    HWND__* hwnd_ = nullptr;

    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    bool startup_complete_ = false;
    bool startup_success_ = false;

    std::mutex frame_mutex_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> bgra_;
};
