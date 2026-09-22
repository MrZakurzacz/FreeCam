#include <winsock2.h>
#include <ws2tcpip.h>

#include <mfapi.h>
#include <objbase.h>

#include "H264Decoder.hpp"
#include "PreviewWindow.hpp"
#include "VideoReceiver.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr std::uint16_t kControlPort = 47820;
constexpr std::uint16_t kVideoPort = 47821;
constexpr std::size_t kReceiveBufferSize = 4096;

std::atomic_bool g_running = true;
std::atomic_uint64_t g_received_frames = 0;

void handle_signal(int) {
    g_running = false;
}

bool send_all(SOCKET socket, const std::string& data) {
    std::size_t sent_total = 0;

    while (sent_total < data.size()) {
        const auto remaining = static_cast<int>(data.size() - sent_total);
        const int sent = send(
            socket,
            data.data() + sent_total,
            remaining,
            0
        );

        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }

        sent_total += static_cast<std::size_t>(sent);
    }

    return true;
}

bool looks_like_hello(const std::string& line) {
    return line.find("\"type\"") != std::string::npos &&
           line.find("\"hello\"") != std::string::npos &&
           line.find("\"protocol\"") != std::string::npos;
}

void handle_client(
    SOCKET client_socket,
    const sockaddr_in& client_address
) {
    char address_buffer[INET_ADDRSTRLEN] = {};
    inet_ntop(
        AF_INET,
        &client_address.sin_addr,
        address_buffer,
        static_cast<socklen_t>(sizeof(address_buffer))
    );

    std::cout << "Client connected from "
              << address_buffer << ":"
              << ntohs(client_address.sin_port)
              << "\n";

    std::array<char, kReceiveBufferSize> buffer{};
    std::string pending;

    while (g_running) {
        const int received = recv(
            client_socket,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0
        );

        if (received == 0) {
            std::cout << "Client disconnected.\n";
            break;
        }

        if (received == SOCKET_ERROR) {
            std::cerr << "recv() failed with WSA error "
                      << WSAGetLastError() << "\n";
            break;
        }

        pending.append(
            buffer.data(),
            static_cast<std::size_t>(received)
        );

        while (true) {
            const auto newline = pending.find('\n');
            if (newline == std::string::npos) {
                break;
            }

            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) {
                continue;
            }

            std::cout << "CONTROL <= " << line << "\n";

            if (!looks_like_hello(line)) {
                const std::string error =
                    "{\"type\":\"error\","
                    "\"code\":\"invalid_message\"}\n";
                send_all(client_socket, error);
                continue;
            }

            const std::string response =
                "{\"type\":\"hello_ack\","
                "\"protocol\":0,"
                "\"videoPort\":" +
                std::to_string(kVideoPort) +
                "}\n";

            if (!send_all(client_socket, response)) {
                std::cerr << "Failed to send hello_ack.\n";
                return;
            }

            std::cout << "CONTROL => " << response;
        }
    }
}

} // namespace

int main() {
    std::signal(SIGINT, handle_signal);

    const HRESULT com_result = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    if (FAILED(com_result)) {
        std::cerr << "CoInitializeEx failed: 0x"
                  << std::hex
                  << static_cast<unsigned long>(com_result)
                  << std::dec << "\n";
        return 1;
    }

    const HRESULT mf_result = MFStartup(MF_VERSION);
    if (FAILED(mf_result)) {
        std::cerr << "MFStartup failed: 0x"
                  << std::hex
                  << static_cast<unsigned long>(mf_result)
                  << std::dec << "\n";
        CoUninitialize();
        return 1;
    }

    WSADATA wsa_data{};
    const int startup_result = WSAStartup(
        MAKEWORD(2, 2),
        &wsa_data
    );

    if (startup_result != 0) {
        std::cerr << "WSAStartup failed with code "
                  << startup_result << "\n";
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    PreviewWindow preview;
    if (!preview.start()) {
        std::cerr << "Could not create preview window.\n";
        WSACleanup();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    auto decoder = std::make_shared<H264Decoder>(
        [&preview](DecodedFrame&& frame) {
            preview.present(std::move(frame));
        }
    );

    VideoReceiver video_receiver(
        kVideoPort,
        [decoder](EncodedFrame&& frame) {
            const auto count = ++g_received_frames;

            if (count == 1 || count % 30 == 0) {
                std::cout
                    << "VIDEO <= frame=" << frame.frame_id
                    << " bytes=" << frame.data.size()
                    << " keyframe="
                    << (frame.keyframe ? "yes" : "no")
                    << " total=" << count
                    << "\n";
            }

            if (!decoder->decode(frame)) {
                std::cerr
                    << "Could not decode H.264 frame "
                    << frame.frame_id << "\n";
            }
        }
    );

    if (!video_receiver.start()) {
        preview.stop();
        WSACleanup();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    const SOCKET listen_socket = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );

    if (listen_socket == INVALID_SOCKET) {
        std::cerr << "socket() failed with WSA error "
                  << WSAGetLastError() << "\n";
        video_receiver.stop();
        preview.stop();
        WSACleanup();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kControlPort);

    if (bind(
            listen_socket,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)
        ) == SOCKET_ERROR) {
        std::cerr << "bind() failed with WSA error "
                  << WSAGetLastError() << "\n";
        closesocket(listen_socket);
        video_receiver.stop();
        preview.stop();
        WSACleanup();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() failed with WSA error "
                  << WSAGetLastError() << "\n";
        closesocket(listen_socket);
        video_receiver.stop();
        preview.stop();
        WSACleanup();
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    std::cout << "FreeCam Receiver\n"
              << "Control: TCP " << kControlPort << "\n"
              << "Video:   UDP " << kVideoPort << "\n"
              << "Preview window is ready.\n"
              << "Press Ctrl+C to stop.\n";

    while (g_running) {
        sockaddr_in client_address{};
        int client_address_size =
            sizeof(client_address);

        const SOCKET client_socket = accept(
            listen_socket,
            reinterpret_cast<sockaddr*>(&client_address),
            &client_address_size
        );

        if (client_socket == INVALID_SOCKET) {
            if (g_running) {
                std::cerr << "accept() failed with WSA error "
                          << WSAGetLastError() << "\n";
            }
            break;
        }

        handle_client(client_socket, client_address);
        closesocket(client_socket);
    }

    closesocket(listen_socket);
    video_receiver.stop();
    decoder.reset();
    preview.stop();
    WSACleanup();
    MFShutdown();
    CoUninitialize();
    return 0;
}
