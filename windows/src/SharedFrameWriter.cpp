#include "SharedFrameWriter.hpp"

#include "../common/SharedFrameProtocol.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

using namespace freecam;

SharedFrameWriter::SharedFrameWriter() = default;

SharedFrameWriter::~SharedFrameWriter() {
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }

    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool SharedFrameWriter::initialize() {
    if (view_) {
        return true;
    }

    mapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(shared::kMappingBytes),
        shared::kMappingName
    );

    if (!mapping_) {
        std::cerr << "CreateFileMappingW failed: "
                  << GetLastError() << "\n";
        return false;
    }

    view_ = MapViewOfFile(
        mapping_,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        shared::kMappingBytes
    );

    if (!view_) {
        std::cerr << "MapViewOfFile failed: "
                  << GetLastError() << "\n";
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    auto* header =
        static_cast<shared::Header*>(view_);

    header->magic = shared::kMagic;
    header->version = shared::kVersion;
    header->sequence = 0;
    header->active_slot = 0;
    header->width = 0;
    header->height = 0;
    header->stride = 0;
    header->pixel_format = 1;
    header->timestamp_us = 0;

    std::memset(
        shared::slot_pointer(view_, 0),
        0,
        shared::kSlotBytes * 2
    );

    std::cout
        << "Virtual camera frame buffer ready.\n";

    return true;
}

bool SharedFrameWriter::publish(
    const DecodedFrame& frame
) {
    if (!view_ && !initialize()) {
        return false;
    }

    if (frame.width == 0 ||
        frame.height == 0 ||
        frame.width > shared::kMaxWidth ||
        frame.height > shared::kMaxHeight) {
        return false;
    }

    const std::size_t stride =
        static_cast<std::size_t>(frame.width) *
        shared::kBytesPerPixel;

    const std::size_t bytes =
        stride * frame.height;

    if (frame.bgra.size() < bytes ||
        bytes > shared::kSlotBytes) {
        return false;
    }

    auto* header =
        static_cast<shared::Header*>(view_);

    const LONG current_slot =
        InterlockedCompareExchange(
            const_cast<LONG*>(&header->active_slot),
            0,
            0
        );

    const std::uint32_t next_slot =
        current_slot == 0 ? 1U : 0U;

    std::memcpy(
        shared::slot_pointer(view_, next_slot),
        frame.bgra.data(),
        bytes
    );

    header->width = frame.width;
    header->height = frame.height;
    header->stride =
        static_cast<std::uint32_t>(stride);
    header->pixel_format = 1;

    MemoryBarrier();

    InterlockedExchange(
        const_cast<LONG*>(&header->active_slot),
        static_cast<LONG>(next_slot)
    );

    InterlockedIncrement(
        const_cast<LONG*>(&header->sequence)
    );

    return true;
}
