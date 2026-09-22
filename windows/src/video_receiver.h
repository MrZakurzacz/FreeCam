#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <thread>

class VideoReceiver {
public:
    static constexpr std::uint16_t kVideoPort = 47821;

    VideoReceiver() = default;
    ~VideoReceiver();

    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;

    bool start();
    void stop();

private:
    void run();

    std::atomic_bool running_{false};
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::thread thread_;
};
