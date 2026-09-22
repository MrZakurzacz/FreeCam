#include "VideoReceiver.hpp"

#include <objbase.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace {

constexpr std::size_t kHeaderBytes = 26;
constexpr std::size_t kMaxDatagramBytes = 2048;
constexpr std::chrono::milliseconds kFrameTimeout{250};

struct PartialFrame {
    bool keyframe = false;
    std::uint64_t timestamp_us = 0;
    std::uint16_t fragment_count = 0;
    std::size_t received_fragments = 0;
    std::vector<std::vector<std::uint8_t>> fragments;
    std::chrono::steady_clock::time_point updated =
        std::chrono::steady_clock::now();
};

std::uint16_t read_u16_be(const std::uint8_t* data) {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

std::uint32_t read_u32_be(const std::uint8_t* data) {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

std::uint64_t read_u64_be(const std::uint8_t* data) {
    return
        (static_cast<std::uint64_t>(data[0]) << 56) |
        (static_cast<std::uint64_t>(data[1]) << 48) |
        (static_cast<std::uint64_t>(data[2]) << 40) |
        (static_cast<std::uint64_t>(data[3]) << 32) |
        (static_cast<std::uint64_t>(data[4]) << 24) |
        (static_cast<std::uint64_t>(data[5]) << 16) |
        (static_cast<std::uint64_t>(data[6]) << 8) |
        static_cast<std::uint64_t>(data[7]);
}

bool has_magic(const std::uint8_t* data) {
    return data[0] == 'F' &&
           data[1] == 'C' &&
           data[2] == 'A' &&
           data[3] == 'M';
}

} // namespace

VideoReceiver::VideoReceiver(
    std::uint16_t port,
    FrameHandler frame_handler
)
    : port_(port),
      frame_handler_(std::move(frame_handler)) {
}

VideoReceiver::~VideoReceiver() {
    stop();
}

bool VideoReceiver::start() {
    if (running_) {
        return true;
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        std::cerr << "UDP socket() failed with WSA error "
                  << WSAGetLastError() << "\n";
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);

    if (bind(
            socket_,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) == SOCKET_ERROR) {
        std::cerr << "UDP bind() failed with WSA error "
                  << WSAGetLastError() << "\n";
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    DWORD timeout_ms = 200;
    setsockopt(
        socket_,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms)
    );

    running_ = true;
    thread_ = std::thread(&VideoReceiver::run, this);

    std::cout << "Listening for video on UDP port "
              << port_ << "\n";
    return true;
}

void VideoReceiver::stop() {
    running_ = false;

    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void VideoReceiver::run() {
    const HRESULT com_result = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    if (FAILED(com_result)) {
        std::cerr << "CoInitializeEx failed on video thread: 0x"
                  << std::hex
                  << static_cast<unsigned long>(com_result)
                  << std::dec << "\n";
        return;
    }

    std::unordered_map<std::uint32_t, PartialFrame> frames;
    std::array<std::uint8_t, kMaxDatagramBytes> buffer{};

    while (running_) {
        sockaddr_in sender{};
        int sender_length = sizeof(sender);

        const int received = recvfrom(
            socket_,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &sender_length
        );

        const auto now = std::chrono::steady_clock::now();

        for (auto it = frames.begin(); it != frames.end();) {
            if (now - it->second.updated > kFrameTimeout) {
                it = frames.erase(it);
            } else {
                ++it;
            }
        }

        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (!running_) {
                break;
            }

            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                continue;
            }

            std::cerr << "recvfrom() failed with WSA error "
                      << error << "\n";
            continue;
        }

        if (received < static_cast<int>(kHeaderBytes)) {
            continue;
        }

        const auto* data = buffer.data();

        if (!has_magic(data) || data[4] != 0) {
            continue;
        }

        const bool keyframe = (data[5] & 0x01) != 0;
        const std::uint32_t frame_id = read_u32_be(data + 10);
        const std::uint16_t fragment_id = read_u16_be(data + 14);
        const std::uint16_t fragment_count = read_u16_be(data + 16);
        const std::uint64_t timestamp_us = read_u64_be(data + 18);

        if (fragment_count == 0 || fragment_id >= fragment_count) {
            continue;
        }

        const auto payload_begin = data + kHeaderBytes;
        const auto payload_size =
            static_cast<std::size_t>(received) - kHeaderBytes;

        auto [it, inserted] = frames.try_emplace(frame_id);
        auto& frame = it->second;

        if (inserted) {
            frame.keyframe = keyframe;
            frame.timestamp_us = timestamp_us;
            frame.fragment_count = fragment_count;
            frame.fragments.resize(fragment_count);
        } else if (frame.fragment_count != fragment_count) {
            frames.erase(it);
            continue;
        }

        frame.updated = now;

        auto& fragment = frame.fragments[fragment_id];
        if (fragment.empty()) {
            fragment.assign(payload_begin, payload_begin + payload_size);
            ++frame.received_fragments;
        }

        if (frame.received_fragments != frame.fragment_count) {
            continue;
        }

        EncodedFrame complete;
        complete.frame_id = frame_id;
        complete.timestamp_us = frame.timestamp_us;
        complete.keyframe = frame.keyframe;

        std::size_t total_bytes = 0;
        for (const auto& part : frame.fragments) {
            total_bytes += part.size();
        }

        complete.data.reserve(total_bytes);
        for (const auto& part : frame.fragments) {
            complete.data.insert(
                complete.data.end(),
                part.begin(),
                part.end()
            );
        }

        frames.erase(it);

        if (frame_handler_) {
            frame_handler_(std::move(complete));
        }
    }

    CoUninitialize();
}
