#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace freecam::shared {

inline constexpr wchar_t kMappingName[] = L"Local\\FreeCamFrames-v1";
inline constexpr std::uint32_t kMagic = 0x4D414346; // "FCAM"
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaxWidth = 1920;
inline constexpr std::uint32_t kMaxHeight = 1080;
inline constexpr std::uint32_t kBytesPerPixel = 4;
inline constexpr std::size_t kSlotBytes =
    static_cast<std::size_t>(kMaxWidth) *
    kMaxHeight *
    kBytesPerPixel;

struct alignas(64) Header {
    std::uint32_t magic;
    std::uint32_t version;
    volatile LONG sequence;
    volatile LONG active_slot;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t pixel_format; // 1 = BGRA8, top-down
    std::uint64_t timestamp_us;
    std::uint8_t reserved[24];
};

inline constexpr std::size_t kHeaderBytes = sizeof(Header);
inline constexpr std::size_t kMappingBytes =
    kHeaderBytes + (2 * kSlotBytes);

inline std::uint8_t* slot_pointer(
    void* base,
    std::uint32_t slot
) {
    return static_cast<std::uint8_t*>(base) +
           kHeaderBytes +
           (static_cast<std::size_t>(slot) * kSlotBytes);
}

inline const std::uint8_t* slot_pointer(
    const void* base,
    std::uint32_t slot
) {
    return static_cast<const std::uint8_t*>(base) +
           kHeaderBytes +
           (static_cast<std::size_t>(slot) * kSlotBytes);
}

} // namespace freecam::shared
