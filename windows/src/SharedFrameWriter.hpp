#pragma once

#include "H264Decoder.hpp"

#include <windows.h>

class SharedFrameWriter {
public:
    SharedFrameWriter();
    ~SharedFrameWriter();

    SharedFrameWriter(const SharedFrameWriter&) = delete;
    SharedFrameWriter& operator=(const SharedFrameWriter&) = delete;

    bool initialize();
    bool publish(const DecodedFrame& frame);

private:
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
};
