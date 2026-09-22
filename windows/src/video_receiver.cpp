#include "video_receiver.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kHeaderBytes = 26;
constexpr std::size_t kMaxDatagramBytes = 2048;
constexpr std::chrono::milliseconds kFrameTimeout{300};

struct PacketHeader {
    std::uint8_t version = 0;
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint32_t frame_id = 0;
    std::uint16_t fragment_id = 0;
    std::uint16_t fragment_count = 0;
    std::uint64_t timestamp_us = 0;
};

struct FrameAssembly {
    std::uint16_t expected_fragments = 0;
    std::vector<std::vector<std::uint8_t>> fragments;
    std::vector<bool> received;
    std::size_t received_count = 0;
    std::size_t total_bytes = 0;
    bool keyframe = false;
    std::uint64_t timestamp_us = 0;
    std::chrono::steady_clock::time_point last_update{};
};

std::uint16_t read_u16_be(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1])
    );
}

std::uint32_t read_u32_be(const std::uint8_t* data) {
    return
        (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_u64_be(const std::uint8_t* data) {
    std::uint64_t value = 0;

    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | data[index];
    }

    return value;
}

bool parse_header(
    const std::uint8_t* data,
    std::size_t size,
    PacketHeader& header
) {
    if (size < kHeaderBytes) {
        return false;
    }

    if (data[0] != 0x46 ||
        data[1] != 0x43 ||
        data[2] != 0x41 ||
        data[3] != 0x4D) {
        return false;
    }

    header.version = data[4];
    header.flags = data[5];
    header.sequence = read_u32_be(data + 6);
    header.frame_id = read_u32_be(data + 10);
    header.fragment_id = read_u16_be(data + 14);
    header.fragment_count = read_u16_be(data + 16);
    header.timestamp_us = read_u64_be(data + 18);

    if (header.version != 0 ||
        header.fragment_count == 0 ||
        header.fragment_id >= header.fragment_count) {
        return false;
    }

    return true;
}

} // namespace

VideoReceiver::~VideoReceiver() {
    stop();
}

bool VideoReceiver::start() {
    if (running_.load()) {
        return true;
    }

    const SOCKET video_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (video_socket == INVALID_SOCKET) {
        std::cerr << "UDP socket() failed with WSA error "
                  << WSAGetLastError() << "\n";
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kVideoPort);

    if (bind(
            video_socket,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) == SOCKET_ERROR) {
        std::cerr << "UDP bind() failed with WSA error "
                  << WSAGetLastError() << "\n";
        closesocket(video_socket);
        return false;
    }

    socket_.store(video_socket);
    running_.store(true);
    thread_ = std::thread(&VideoReceiver::run, this);

    std::cout << "Listening for video on UDP port "
              << kVideoPort << "\n";

    return true;
}

void VideoReceiver::stop() {
    running_.store(false);

    const SOCKET video_socket = socket_.exchange(INVALID_SOCKET);
    if (video_socket != INVALID_SOCKET) {
        closesocket(video_socket);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void VideoReceiver::run() {
    std::unordered_map<std::uint32_t, FrameAssembly> frames;

    std::uint64_t completed_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t invalid_packets = 0;

    std::array<std::uint8_t, kMaxDatagramBytes> buffer{};

    while (running_.load()) {
        const SOCKET video_socket = socket_.load();
        if (video_socket == INVALID_SOCKET) {
            break;
        }

        sockaddr_in sender{};
        int sender_size = sizeof(sender);

        const int received = recvfrom(
            video_socket,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &sender_size
        );

        if (received == SOCKET_ERROR) {
            if (running_.load()) {
                const int error = WSAGetLastError();

                if (error != WSAEINTR &&
                    error != WSAENOTSOCK &&
                    error != WSAESHUTDOWN) {
                    std::cerr << "recvfrom() failed with WSA error "
                              << error << "\n";
                }
            }
            break;
        }

        const auto now = std::chrono::steady_clock::now();

        for (auto iterator = frames.begin(); iterator != frames.end();) {
            if (now - iterator->second.last_update > kFrameTimeout) {
                ++dropped_frames;
                iterator = frames.erase(iterator);
            } else {
                ++iterator;
            }
        }

        PacketHeader header{};
        const auto packet_size = static_cast<std::size_t>(received);

        if (!parse_header(buffer.data(), packet_size, header)) {
            ++invalid_packets;
            continue;
        }

        const std::size_t payload_size = packet_size - kHeaderBytes;
        if (payload_size == 0) {
            ++invalid_packets;
            continue;
        }

        auto [iterator, inserted] = frames.try_emplace(header.frame_id);
        FrameAssembly& frame = iterator->second;

        if (inserted) {
            frame.expected_fragments = header.fragment_count;
            frame.fragments.resize(header.fragment_count);
            frame.received.resize(header.fragment_count, false);
            frame.keyframe = (header.flags & 0x01U) != 0;
            frame.timestamp_us = header.timestamp_us;
        } else if (frame.expected_fragments != header.fragment_count) {
            ++dropped_frames;
            frames.erase(iterator);
            continue;
        }

        frame.last_update = now;

        const std::size_t fragment_index = header.fragment_id;
        if (!frame.received[fragment_index]) {
            const auto payload_begin = buffer.begin() +
                static_cast<std::ptrdiff_t>(kHeaderBytes);
            const auto payload_end = buffer.begin() +
                static_cast<std::ptrdiff_t>(packet_size);

            frame.fragments[fragment_index].assign(
                payload_begin,
                payload_end
            );

            frame.received[fragment_index] = true;
            ++frame.received_count;
            frame.total_bytes += payload_size;
        }

        if (frame.received_count != frame.expected_fragments) {
            continue;
        }

        std::vector<std::uint8_t> access_unit;
        access_unit.reserve(frame.total_bytes);

        for (const auto& fragment : frame.fragments) {
            access_unit.insert(
                access_unit.end(),
                fragment.begin(),
                fragment.end()
            );
        }

        ++completed_frames;

        if (completed_frames <= 5 || completed_frames % 30 == 0) {
            std::cout
                << "VIDEO frame=" << header.frame_id
                << " bytes=" << access_unit.size()
                << " fragments=" << frame.expected_fragments
                << (frame.keyframe ? " keyframe" : "")
                << " complete=" << completed_frames
                << " dropped=" << dropped_frames
                << " invalid=" << invalid_packets
                << "\n";
        }

        frames.erase(header.frame_id);
    }
}
