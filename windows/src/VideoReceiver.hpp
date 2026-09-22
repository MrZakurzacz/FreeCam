#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

struct EncodedFrame {
    std::uint32_t frame_id = 0;
    std::uint64_t timestamp_us = 0;
    bool keyframe = false;
    std::vector<std::uint8_t> data;
};

class VideoReceiver {
public:
    using FrameHandler = std::function<void(EncodedFrame&&)>;

    VideoReceiver(std::uint16_t port, FrameHandler frame_handler);
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    bool start();
    void stop();

private:
    void run();

    std::uint16_t port_;
    FrameHandler frame_handler_;
    SOCKET socket_ = INVALID_SOCKET;
    std::atomic_bool running_ = false;
    std::thread thread_;
};
