#pragma once

#include "VideoReceiver.hpp"

#include <cstdint>
#include <functional>
#include <vector>

struct DecodedFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgra;
};

class H264Decoder {
public:
    using FrameHandler = std::function<void(DecodedFrame&&)>;

    explicit H264Decoder(FrameHandler frame_handler);
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool initialize();
    bool decode(const EncodedFrame& frame);

private:
    bool configureOutput();
    bool drainOutput();
    bool deliverOutputSample(struct IMFSample* sample);
    void shutdown();

    struct Impl;
    Impl* impl_ = nullptr;
    FrameHandler frame_handler_;
};
