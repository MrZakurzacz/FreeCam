#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <strsafe.h>

#include "../common/SharedFrameProtocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr LONG kWidth = 1280;
constexpr LONG kHeight = 720;
constexpr LONG kBytesPerPixel = 4;
constexpr LONG kRgb32FrameBytes = kWidth * kHeight * kBytesPerPixel;
constexpr REFERENCE_TIME kFrameDuration = 10'000'000 / 30;

const CLSID CLSID_FreeCamVirtualCamera = {
    0xe9b9e13d, 0x2a0e, 0x4c83,
    {0xaf, 0xb3, 0x7d, 0x1f, 0xa0, 0xb9, 0x1f, 0x1a}
};

HMODULE g_module = nullptr;
std::atomic_long g_object_count{0};
std::atomic_long g_server_locks{0};

void free_media_type(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree(mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

HRESULT copy_media_type(
    AM_MEDIA_TYPE* destination,
    const AM_MEDIA_TYPE* source
) {
    if (!destination || !source) {
        return E_POINTER;
    }

    *destination = *source;
    destination->pUnk = nullptr;
    destination->pbFormat = nullptr;

    if (source->cbFormat != 0) {
        destination->pbFormat = static_cast<BYTE*>(
            CoTaskMemAlloc(source->cbFormat)
        );
        if (!destination->pbFormat) {
            destination->cbFormat = 0;
            return E_OUTOFMEMORY;
        }

        std::memcpy(
            destination->pbFormat,
            source->pbFormat,
            source->cbFormat
        );
    }

    if (source->pUnk) {
        destination->pUnk = source->pUnk;
        destination->pUnk->AddRef();
    }

    return S_OK;
}

enum class OutputFormat {
    NV12,
    I420,
    YUY2,
    RGB32
};

struct FormatDescriptor {
    OutputFormat format;
    const GUID* subtype;
    WORD bits_per_pixel;
    LONG sample_bytes;
    bool rgb;
};

constexpr LONG kRgb32FrameBytes =
    kWidth * kHeight * 4;
constexpr LONG kYuy2FrameBytes =
    kWidth * kHeight * 2;
constexpr LONG kYuv420FrameBytes =
    kWidth * kHeight * 3 / 2;

const FormatDescriptor kFormats[] = {
    {
        OutputFormat::NV12,
        &MEDIASUBTYPE_NV12,
        12,
        kYuv420FrameBytes,
        false
    },
    {
        OutputFormat::I420,
        &MEDIASUBTYPE_I420,
        12,
        kYuv420FrameBytes,
        false
    },
    {
        OutputFormat::YUY2,
        &MEDIASUBTYPE_YUY2,
        16,
        kYuy2FrameBytes,
        false
    },
    {
        OutputFormat::RGB32,
        &MEDIASUBTYPE_RGB32,
        32,
        kRgb32FrameBytes,
        true
    }
};

const FormatDescriptor* find_format(
    const AM_MEDIA_TYPE* mt
) {
    if (!mt ||
        mt->majortype != MEDIATYPE_Video ||
        mt->formattype != FORMAT_VideoInfo ||
        mt->cbFormat < sizeof(VIDEOINFOHEADER) ||
        !mt->pbFormat) {
        return nullptr;
    }

    const auto* vih =
        reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);

    if (vih->bmiHeader.biWidth != kWidth ||
        std::abs(vih->bmiHeader.biHeight) != kHeight) {
        return nullptr;
    }

    for (const auto& descriptor : kFormats) {
        if (mt->subtype == *descriptor.subtype &&
            vih->bmiHeader.biBitCount ==
                descriptor.bits_per_pixel) {
            return &descriptor;
        }
    }

    return nullptr;
}

const FormatDescriptor* find_format(
    OutputFormat format
) {
    for (const auto& descriptor : kFormats) {
        if (descriptor.format == format) {
            return &descriptor;
        }
    }
    return nullptr;
}

HRESULT make_media_type(
    const FormatDescriptor& descriptor,
    AM_MEDIA_TYPE* mt
) {
    if (!mt) {
        return E_POINTER;
    }

    std::memset(mt, 0, sizeof(*mt));

    auto* vih = static_cast<VIDEOINFOHEADER*>(
        CoTaskMemAlloc(sizeof(VIDEOINFOHEADER))
    );
    if (!vih) {
        return E_OUTOFMEMORY;
    }

    std::memset(vih, 0, sizeof(*vih));
    vih->AvgTimePerFrame = kFrameDuration;

    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = kWidth;
    vih->bmiHeader.biHeight = kHeight;
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount =
        descriptor.bits_per_pixel;
    vih->bmiHeader.biCompression =
        descriptor.rgb
            ? BI_RGB
            : descriptor.subtype->Data1;
    vih->bmiHeader.biSizeImage =
        descriptor.sample_bytes;

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *descriptor.subtype;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = descriptor.sample_bytes;
    mt->formattype = FORMAT_VideoInfo;
    mt->pUnk = nullptr;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = reinterpret_cast<BYTE*>(vih);

    return S_OK;
}

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(
        std::clamp(value, 0, 255)
    );
}

void bgra_to_yuv(
    const std::uint8_t* pixel,
    int& y,
    int& u,
    int& v
) {
    const int b = pixel[0];
    const int g = pixel[1];
    const int r = pixel[2];

    y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

    y = std::clamp(y, 0, 255);
    u = std::clamp(u, 0, 255);
    v = std::clamp(v, 0, 255);
}

void convert_bgra_to_yuy2(
    const std::uint8_t* source,
    std::uint8_t* destination
) {
    for (LONG y = 0; y < kHeight; ++y) {
        const auto* row =
            source +
            static_cast<std::size_t>(y) *
            kWidth *
            4;

        auto* out =
            destination +
            static_cast<std::size_t>(y) *
            kWidth *
            2;

        for (LONG x = 0; x < kWidth; x += 2) {
            int y0, u0, v0;
            int y1, u1, v1;

            bgra_to_yuv(row + x * 4, y0, u0, v0);
            bgra_to_yuv(row + (x + 1) * 4, y1, u1, v1);

            out[x * 2 + 0] = clamp_byte(y0);
            out[x * 2 + 1] =
                clamp_byte((u0 + u1) / 2);
            out[x * 2 + 2] = clamp_byte(y1);
            out[x * 2 + 3] =
                clamp_byte((v0 + v1) / 2);
        }
    }
}

void convert_bgra_to_nv12(
    const std::uint8_t* source,
    std::uint8_t* destination
) {
    auto* y_plane = destination;
    auto* uv_plane =
        destination +
        static_cast<std::size_t>(kWidth) *
        kHeight;

    for (LONG y = 0; y < kHeight; ++y) {
        const auto* row =
            source +
            static_cast<std::size_t>(y) *
            kWidth *
            4;

        for (LONG x = 0; x < kWidth; ++x) {
            int yy, u, v;
            bgra_to_yuv(row + x * 4, yy, u, v);
            y_plane[
                static_cast<std::size_t>(y) *
                kWidth + x
            ] = clamp_byte(yy);
        }
    }

    for (LONG y = 0; y < kHeight; y += 2) {
        for (LONG x = 0; x < kWidth; x += 2) {
            int u_sum = 0;
            int v_sum = 0;

            for (LONG dy = 0; dy < 2; ++dy) {
                const auto* row =
                    source +
                    static_cast<std::size_t>(y + dy) *
                    kWidth *
                    4;

                for (LONG dx = 0; dx < 2; ++dx) {
                    int yy, u, v;
                    bgra_to_yuv(
                        row + (x + dx) * 4,
                        yy,
                        u,
                        v
                    );
                    u_sum += u;
                    v_sum += v;
                }
            }

            const std::size_t uv_index =
                static_cast<std::size_t>(y / 2) *
                kWidth +
                x;

            uv_plane[uv_index] =
                clamp_byte(u_sum / 4);
            uv_plane[uv_index + 1] =
                clamp_byte(v_sum / 4);
        }
    }
}

void convert_bgra_to_i420(
    const std::uint8_t* source,
    std::uint8_t* destination
) {
    auto* y_plane = destination;
    auto* u_plane =
        destination +
        static_cast<std::size_t>(kWidth) *
        kHeight;
    auto* v_plane =
        u_plane +
        static_cast<std::size_t>(kWidth / 2) *
        (kHeight / 2);

    for (LONG y = 0; y < kHeight; ++y) {
        const auto* row =
            source +
            static_cast<std::size_t>(y) *
            kWidth *
            4;

        for (LONG x = 0; x < kWidth; ++x) {
            int yy, u, v;
            bgra_to_yuv(row + x * 4, yy, u, v);
            y_plane[
                static_cast<std::size_t>(y) *
                kWidth + x
            ] = clamp_byte(yy);
        }
    }

    for (LONG y = 0; y < kHeight; y += 2) {
        for (LONG x = 0; x < kWidth; x += 2) {
            int u_sum = 0;
            int v_sum = 0;

            for (LONG dy = 0; dy < 2; ++dy) {
                const auto* row =
                    source +
                    static_cast<std::size_t>(y + dy) *
                    kWidth *
                    4;

                for (LONG dx = 0; dx < 2; ++dx) {
                    int yy, u, v;
                    bgra_to_yuv(
                        row + (x + dx) * 4,
                        yy,
                        u,
                        v
                    );
                    u_sum += u;
                    v_sum += v;
                }
            }

            const std::size_t chroma_index =
                static_cast<std::size_t>(y / 2) *
                (kWidth / 2) +
                (x / 2);

            u_plane[chroma_index] =
                clamp_byte(u_sum / 4);
            v_plane[chroma_index] =
                clamp_byte(v_sum / 4);
        }
    }
}

bool write_output_frame(
    OutputFormat format,
    const std::uint8_t* source,
    std::uint8_t* destination
) {
    switch (format) {
    case OutputFormat::NV12:
        convert_bgra_to_nv12(source, destination);
        return true;

    case OutputFormat::I420:
        convert_bgra_to_i420(source, destination);
        return true;

    case OutputFormat::YUY2:
        convert_bgra_to_yuy2(source, destination);
        return true;

    case OutputFormat::RGB32:
        for (LONG y = 0; y < kHeight; ++y) {
            const auto* source_row =
                source +
                static_cast<std::size_t>(
                    kHeight - 1 - y
                ) *
                kWidth *
                4;

            std::memcpy(
                destination +
                    static_cast<std::size_t>(y) *
                    kWidth *
                    4,
                source_row,
                static_cast<std::size_t>(kWidth) * 4
            );
        }
        return true;
    }

    return false;
}

class SharedFrameReader {
public:
    ~SharedFrameReader() {
        close();
    }

    bool read_latest(std::vector<std::uint8_t>& output) {
        if (!view_ && !open()) {
            return false;
        }

        const auto* header =
            static_cast<const freecam::shared::Header*>(view_);

        if (header->magic != freecam::shared::kMagic ||
            header->version != freecam::shared::kVersion ||
            header->width != static_cast<std::uint32_t>(kWidth) ||
            header->height != static_cast<std::uint32_t>(kHeight) ||
            header->stride !=
                static_cast<std::uint32_t>(kWidth * kBytesPerPixel) ||
            header->pixel_format != 1) {
            return false;
        }

        if (output.size() != static_cast<std::size_t>(kRgb32FrameBytes)) {
            output.resize(kRgb32FrameBytes);
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            const LONG before = InterlockedCompareExchange(
                const_cast<LONG*>(&header->sequence),
                0,
                0
            );

            const LONG slot = InterlockedCompareExchange(
                const_cast<LONG*>(&header->active_slot),
                0,
                0
            );

            if (slot != 0 && slot != 1) {
                return false;
            }

            MemoryBarrier();

            std::memcpy(
                output.data(),
                freecam::shared::slot_pointer(
                    view_,
                    static_cast<std::uint32_t>(slot)
                ),
                kRgb32FrameBytes
            );

            MemoryBarrier();

            const LONG after = InterlockedCompareExchange(
                const_cast<LONG*>(&header->sequence),
                0,
                0
            );

            if (before == after) {
                last_sequence_ = after;
                return true;
            }
        }

        return false;
    }

private:
    bool open() {
        mapping_ = OpenFileMappingW(
            FILE_MAP_READ | FILE_MAP_WRITE,
            FALSE,
            freecam::shared::kMappingName
        );

        if (!mapping_) {
            return false;
        }

        view_ = MapViewOfFile(
            mapping_,
            FILE_MAP_READ | FILE_MAP_WRITE,
            0,
            0,
            freecam::shared::kMappingBytes
        );

        if (!view_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        return true;
    }

    void close() {
        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    HANDLE mapping_ = nullptr;
    const void* view_ = nullptr;
    LONG last_sequence_ = -1;
};

class FreeCamFilter;
class FreeCamPin;

class MediaTypeEnumerator final : public IEnumMediaTypes {
public:
    MediaTypeEnumerator()
        : ref_count_(1) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object
    ) override {
        if (!object) {
            return E_POINTER;
        }

        if (riid == IID_IUnknown ||
            riid == IID_IEnumMediaTypes) {
            *object =
                static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(
            InterlockedIncrement(&ref_count_)
        );
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG value =
            InterlockedDecrement(&ref_count_);

        if (value == 0) {
            delete this;
            return 0;
        }

        return static_cast<ULONG>(value);
    }

    HRESULT STDMETHODCALLTYPE Next(
        ULONG count,
        AM_MEDIA_TYPE** media_types,
        ULONG* fetched
    ) override {
        if (!media_types) {
            return E_POINTER;
        }

        if (count > 1 && !fetched) {
            return E_POINTER;
        }

        ULONG produced = 0;

        while (produced < count &&
               index_ < std::size(kFormats)) {
            auto* mt = static_cast<AM_MEDIA_TYPE*>(
                CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE))
            );

            if (!mt) {
                break;
            }

            const HRESULT hr =
                make_media_type(
                    kFormats[index_],
                    mt
                );

            if (FAILED(hr)) {
                CoTaskMemFree(mt);
                break;
            }

            media_types[produced] = mt;
            ++produced;
            ++index_;
        }

        if (fetched) {
            *fetched = produced;
        }

        return produced == count
            ? S_OK
            : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(
        ULONG count
    ) override {
        const ULONG remaining =
            static_cast<ULONG>(
                std::size(kFormats) - index_
            );

        const ULONG skipped =
            std::min(count, remaining);

        index_ += skipped;

        return skipped == count
            ? S_OK
            : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        index_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(
        IEnumMediaTypes** clone
    ) override {
        if (!clone) {
            return E_POINTER;
        }

        auto* enumerator =
            new (std::nothrow) MediaTypeEnumerator();

        if (!enumerator) {
            return E_OUTOFMEMORY;
        }

        enumerator->index_ = index_;
        *clone = enumerator;
        return S_OK;
    }

private:
    volatile LONG ref_count_;
    ULONG index_ = 0;
};

class FreeCamPin final :
    public IPin,
    public IAMStreamConfig,
    public IKsPropertySet {
public:
    explicit FreeCamPin(FreeCamFilter* filter)
        : filter_(filter) {
        std::memset(&connection_type_, 0, sizeof(connection_type_));
    }

    ~FreeCamPin() {
        stop_streaming();
        disconnect_internal();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object
    ) override;

    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Connect(
        IPin* receive_pin,
        const AM_MEDIA_TYPE* media_type
    ) override;

    HRESULT STDMETHODCALLTYPE ReceiveConnection(
        IPin* connector,
        const AM_MEDIA_TYPE* media_type
    ) override {
        UNREFERENCED_PARAMETER(connector);
        UNREFERENCED_PARAMETER(media_type);
        return E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE Disconnect() override {
        std::lock_guard lock(connection_mutex_);
        if (!connected_pin_) {
            return S_FALSE;
        }
        disconnect_internal();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ConnectedTo(
        IPin** pin
    ) override {
        if (!pin) {
            return E_POINTER;
        }

        std::lock_guard lock(connection_mutex_);
        if (!connected_pin_) {
            *pin = nullptr;
            return VFW_E_NOT_CONNECTED;
        }

        connected_pin_->AddRef();
        *pin = connected_pin_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ConnectionMediaType(
        AM_MEDIA_TYPE* media_type
    ) override {
        if (!media_type) {
            return E_POINTER;
        }

        std::lock_guard lock(connection_mutex_);
        if (!connected_pin_) {
            return VFW_E_NOT_CONNECTED;
        }

        return copy_media_type(
            media_type,
            &connection_type_
        );
    }

    HRESULT STDMETHODCALLTYPE QueryPinInfo(
        PIN_INFO* info
    ) override;

    HRESULT STDMETHODCALLTYPE QueryDirection(
        PIN_DIRECTION* direction
    ) override {
        if (!direction) {
            return E_POINTER;
        }
        *direction = PINDIR_OUTPUT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryId(
        LPWSTR* id
    ) override {
        if (!id) {
            return E_POINTER;
        }

        constexpr wchar_t name[] = L"Output";
        const std::size_t bytes = sizeof(name);

        *id = static_cast<LPWSTR>(
            CoTaskMemAlloc(bytes)
        );
        if (!*id) {
            return E_OUTOFMEMORY;
        }

        std::memcpy(*id, name, bytes);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryAccept(
        const AM_MEDIA_TYPE* media_type
    ) override {
        return find_format(media_type)
            ? S_OK
            : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE EnumMediaTypes(
        IEnumMediaTypes** enumerator
    ) override {
        if (!enumerator) {
            return E_POINTER;
        }

        auto* value =
            new (std::nothrow) MediaTypeEnumerator();
        if (!value) {
            return E_OUTOFMEMORY;
        }

        *enumerator = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInternalConnections(
        IPin** pins,
        ULONG* count
    ) override {
        UNREFERENCED_PARAMETER(pins);
        UNREFERENCED_PARAMETER(count);
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EndOfStream() override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE BeginFlush() override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EndFlush() override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE NewSegment(
        REFERENCE_TIME start,
        REFERENCE_TIME stop,
        double rate
    ) override {
        UNREFERENCED_PARAMETER(start);
        UNREFERENCED_PARAMETER(stop);
        UNREFERENCED_PARAMETER(rate);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFormat(
        AM_MEDIA_TYPE* media_type
    ) override {
        if (!media_type) {
            return E_POINTER;
        }

        const auto* descriptor =
            find_format(media_type);

        if (!descriptor) {
            return VFW_E_INVALIDMEDIATYPE;
        }

        std::lock_guard lock(connection_mutex_);

        if (connected_pin_) {
            return VFW_E_NOT_STOPPED;
        }

        preferred_format_ = descriptor->format;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFormat(
        AM_MEDIA_TYPE** media_type
    ) override {
        if (!media_type) {
            return E_POINTER;
        }

        const auto* descriptor =
            find_format(preferred_format_);

        if (!descriptor) {
            return E_FAIL;
        }

        auto* mt = static_cast<AM_MEDIA_TYPE*>(
            CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE))
        );

        if (!mt) {
            return E_OUTOFMEMORY;
        }

        const HRESULT hr =
            make_media_type(*descriptor, mt);

        if (FAILED(hr)) {
            CoTaskMemFree(mt);
            return hr;
        }

        *media_type = mt;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(
        int* count,
        int* size
    ) override {
        if (!count || !size) {
            return E_POINTER;
        }

        *count = static_cast<int>(
            std::size(kFormats)
        );
        *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStreamCaps(
        int index,
        AM_MEDIA_TYPE** media_type,
        BYTE* caps
    ) override {
        if (!media_type || !caps) {
            return E_POINTER;
        }

        if (index < 0 ||
            index >= static_cast<int>(
                std::size(kFormats)
            )) {
            return S_FALSE;
        }

        auto* mt = static_cast<AM_MEDIA_TYPE*>(
            CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE))
        );

        if (!mt) {
            return E_OUTOFMEMORY;
        }

        const auto& descriptor =
            kFormats[index];

        HRESULT hr =
            make_media_type(descriptor, mt);

        if (FAILED(hr)) {
            CoTaskMemFree(mt);
            return hr;
        }

        *media_type = mt;

        auto* config =
            reinterpret_cast<
                VIDEO_STREAM_CONFIG_CAPS*
            >(caps);

        std::memset(
            config,
            0,
            sizeof(*config)
        );

        config->guid = FORMAT_VideoInfo;
        config->VideoStandard = AnalogVideo_None;
        config->InputSize = {kWidth, kHeight};
        config->MinCroppingSize = {kWidth, kHeight};
        config->MaxCroppingSize = {kWidth, kHeight};
        config->CropGranularityX = 1;
        config->CropGranularityY = 1;
        config->MinOutputSize = {kWidth, kHeight};
        config->MaxOutputSize = {kWidth, kHeight};
        config->OutputGranularityX = 1;
        config->OutputGranularityY = 1;
        config->MinFrameInterval = kFrameDuration;
        config->MaxFrameInterval = kFrameDuration;
        config->MinBitsPerSecond =
            descriptor.sample_bytes * 8 * 30;
        config->MaxBitsPerSecond =
            config->MinBitsPerSecond;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Set(
        REFGUID property_set,
        DWORD property_id,
        LPVOID instance_data,
        DWORD instance_length,
        LPVOID property_data,
        DWORD property_length
    ) override {
        UNREFERENCED_PARAMETER(property_set);
        UNREFERENCED_PARAMETER(property_id);
        UNREFERENCED_PARAMETER(instance_data);
        UNREFERENCED_PARAMETER(instance_length);
        UNREFERENCED_PARAMETER(property_data);
        UNREFERENCED_PARAMETER(property_length);
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Get(
        REFGUID property_set,
        DWORD property_id,
        LPVOID instance_data,
        DWORD instance_length,
        LPVOID property_data,
        DWORD property_length,
        DWORD* returned
    ) override {
        UNREFERENCED_PARAMETER(instance_data);
        UNREFERENCED_PARAMETER(instance_length);

        if (property_set == AMPROPSETID_Pin &&
            property_id == AMPROPERTY_PIN_CATEGORY) {
            if (returned) {
                *returned = sizeof(GUID);
            }

            if (!property_data) {
                return S_OK;
            }

            if (property_length < sizeof(GUID)) {
                return E_UNEXPECTED;
            }

            *static_cast<GUID*>(property_data) =
                PIN_CATEGORY_CAPTURE;
            return S_OK;
        }

        return E_PROP_SET_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE QuerySupported(
        REFGUID property_set,
        DWORD property_id,
        DWORD* support
    ) override {
        if (!support) {
            return E_POINTER;
        }

        if (property_set == AMPROPSETID_Pin &&
            property_id == AMPROPERTY_PIN_CATEGORY) {
            *support = KSPROPERTY_SUPPORT_GET;
            return S_OK;
        }

        *support = 0;
        return E_PROP_SET_UNSUPPORTED;
    }

    HRESULT start_streaming();
    void stop_streaming();

private:
    void disconnect_internal() {
        if (allocator_) {
            allocator_->Decommit();
            allocator_->Release();
            allocator_ = nullptr;
        }
        if (mem_input_) {
            mem_input_->Release();
            mem_input_ = nullptr;
        }
        if (connected_pin_) {
            connected_pin_->Release();
            connected_pin_ = nullptr;
        }
        free_media_type(connection_type_);
        std::memset(
            &connection_type_,
            0,
            sizeof(connection_type_)
        );
    }

    HRESULT negotiate_allocator(
        IMemInputPin* input,
        LONG sample_bytes,
        IMemAllocator** allocator
    ) {
        if (!input || !allocator) {
            return E_POINTER;
        }

        *allocator = nullptr;

        ALLOCATOR_PROPERTIES requested{};
        requested.cBuffers = 3;
        requested.cbBuffer = sample_bytes;
        requested.cbAlign = 1;
        requested.cbPrefix = 0;

        ALLOCATOR_PROPERTIES downstream{};
        if (SUCCEEDED(
                input->GetAllocatorRequirements(&downstream)
            )) {
            requested.cBuffers = std::max(
                requested.cBuffers,
                downstream.cBuffers
            );
            requested.cbBuffer = std::max(
                requested.cbBuffer,
                downstream.cbBuffer
            );
            requested.cbAlign = std::max(
                requested.cbAlign,
                downstream.cbAlign
            );
            requested.cbPrefix = std::max(
                requested.cbPrefix,
                downstream.cbPrefix
            );
        }

        IMemAllocator* selected = nullptr;
        HRESULT hr = input->GetAllocator(&selected);

        if (FAILED(hr) || !selected) {
            hr = CoCreateInstance(
                CLSID_MemoryAllocator,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_IMemAllocator,
                reinterpret_cast<void**>(&selected)
            );
            if (FAILED(hr)) {
                return hr;
            }
        }

        ALLOCATOR_PROPERTIES actual{};
        hr = selected->SetProperties(
            &requested,
            &actual
        );

        if (FAILED(hr) ||
            actual.cbBuffer < sample_bytes) {
            selected->Release();
            return FAILED(hr)
                ? hr
                : E_FAIL;
        }

        hr = input->NotifyAllocator(selected, FALSE);
        if (FAILED(hr)) {
            selected->Release();
            return hr;
        }

        *allocator = selected;
        return S_OK;
    }

    void stream_loop() {
        const auto* descriptor =
            find_format(&connection_type_);

        if (!descriptor) {
            return;
        }

        std::vector<std::uint8_t> frame(
            kRgb32FrameBytes,
            0
        );

        std::uint64_t frame_number = 0;
        auto next =
            std::chrono::steady_clock::now();

        while (streaming_.load()) {
            reader_.read_latest(frame);

            IMemAllocator* allocator = nullptr;
            IMemInputPin* input = nullptr;

            {
                std::lock_guard lock(connection_mutex_);
                allocator = allocator_;
                input = mem_input_;

                if (allocator) {
                    allocator->AddRef();
                }

                if (input) {
                    input->AddRef();
                }
            }

            if (allocator && input) {
                IMediaSample* sample = nullptr;

                HRESULT hr = allocator->GetBuffer(
                    &sample,
                    nullptr,
                    nullptr,
                    0
                );

                if (SUCCEEDED(hr) && sample) {
                    BYTE* destination = nullptr;

                    if (SUCCEEDED(
                            sample->GetPointer(&destination)
                        ) &&
                        sample->GetSize() >=
                            descriptor->sample_bytes &&
                        write_output_frame(
                            descriptor->format,
                            frame.data(),
                            destination
                        )) {
                        sample->SetActualDataLength(
                            descriptor->sample_bytes
                        );

                        REFERENCE_TIME start =
                            static_cast<REFERENCE_TIME>(
                                frame_number
                            ) *
                            kFrameDuration;

                        REFERENCE_TIME end =
                            start + kFrameDuration;

                        sample->SetTime(&start, &end);
                        sample->SetSyncPoint(TRUE);
                        sample->SetPreroll(FALSE);
                        sample->SetDiscontinuity(
                            frame_number == 0
                        );

                        input->Receive(sample);
                    }

                    sample->Release();
                }
            }

            if (input) {
                input->Release();
            }

            if (allocator) {
                allocator->Release();
            }

            ++frame_number;

            next += std::chrono::nanoseconds(
                kFrameDuration * 100
            );

            std::unique_lock wait_lock(stream_mutex_);
            stream_cv_.wait_until(
                wait_lock,
                next,
                [this] {
                    return !streaming_.load();
                }
            );
        }
    }

    FreeCamFilter* filter_;
    std::mutex connection_mutex_;
    IPin* connected_pin_ = nullptr;
    IMemInputPin* mem_input_ = nullptr;
    IMemAllocator* allocator_ = nullptr;
    AM_MEDIA_TYPE connection_type_{};
    OutputFormat preferred_format_ =
        OutputFormat::NV12;

    std::atomic_bool streaming_{false};
    std::thread stream_thread_;
    std::mutex stream_mutex_;
    std::condition_variable stream_cv_;
    SharedFrameReader reader_;
};

class PinEnumerator final : public IEnumPins {
public:
    explicit PinEnumerator(FreeCamFilter* filter);

    ~PinEnumerator();

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object
    ) override;

    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Next(
        ULONG count,
        IPin** pins,
        ULONG* fetched
    ) override;

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override;
    HRESULT STDMETHODCALLTYPE Reset() override;

    HRESULT STDMETHODCALLTYPE Clone(
        IEnumPins** clone
    ) override;

private:
    volatile LONG ref_count_ = 1;
    FreeCamFilter* filter_;
    ULONG index_ = 0;
};

class FreeCamFilter final : public IBaseFilter {
public:
    FreeCamFilter()
        : ref_count_(1),
          pin_(this) {
        g_object_count.fetch_add(1);
        name_[0] = L'\0';
    }

    ~FreeCamFilter() {
        pin_.stop_streaming();

        if (clock_) {
            clock_->Release();
            clock_ = nullptr;
        }

        g_object_count.fetch_sub(1);
    }

    FreeCamPin* pin() {
        return &pin_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object
    ) override {
        if (!object) {
            return E_POINTER;
        }

        if (riid == IID_IUnknown ||
            riid == IID_IPersist ||
            riid == IID_IMediaFilter ||
            riid == IID_IBaseFilter) {
            *object = static_cast<IBaseFilter*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(
            InterlockedIncrement(&ref_count_)
        );
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG value =
            InterlockedDecrement(&ref_count_);

        if (value == 0) {
            delete this;
            return 0;
        }

        return static_cast<ULONG>(value);
    }

    HRESULT STDMETHODCALLTYPE GetClassID(
        CLSID* class_id
    ) override {
        if (!class_id) {
            return E_POINTER;
        }
        *class_id = CLSID_FreeCamVirtualCamera;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stop() override {
        std::lock_guard lock(state_mutex_);
        pin_.stop_streaming();
        state_ = State_Stopped;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Pause() override {
        std::lock_guard lock(state_mutex_);
        state_ = State_Paused;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Run(
        REFERENCE_TIME start
    ) override {
        UNREFERENCED_PARAMETER(start);

        std::lock_guard lock(state_mutex_);
        const HRESULT hr = pin_.start_streaming();
        if (FAILED(hr) && hr != VFW_E_NOT_CONNECTED) {
            return hr;
        }

        state_ = State_Running;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetState(
        DWORD timeout,
        FILTER_STATE* state
    ) override {
        UNREFERENCED_PARAMETER(timeout);

        if (!state) {
            return E_POINTER;
        }

        std::lock_guard lock(state_mutex_);
        *state = state_;

        if (state_ == State_Paused) {
            return VFW_S_CANT_CUE;
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSyncSource(
        IReferenceClock* clock
    ) override {
        std::lock_guard lock(state_mutex_);

        if (clock) {
            clock->AddRef();
        }
        if (clock_) {
            clock_->Release();
        }

        clock_ = clock;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSyncSource(
        IReferenceClock** clock
    ) override {
        if (!clock) {
            return E_POINTER;
        }

        std::lock_guard lock(state_mutex_);
        *clock = clock_;
        if (*clock) {
            (*clock)->AddRef();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumPins(
        IEnumPins** enumerator
    ) override {
        if (!enumerator) {
            return E_POINTER;
        }

        auto* value =
            new (std::nothrow) PinEnumerator(this);
        if (!value) {
            return E_OUTOFMEMORY;
        }

        *enumerator = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FindPin(
        LPCWSTR id,
        IPin** pin
    ) override {
        if (!id || !pin) {
            return E_POINTER;
        }

        if (wcscmp(id, L"Output") != 0) {
            *pin = nullptr;
            return VFW_E_NOT_FOUND;
        }

        *pin = static_cast<IPin*>(&pin_);
        pin_.AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryFilterInfo(
        FILTER_INFO* info
    ) override {
        if (!info) {
            return E_POINTER;
        }

        StringCchCopyW(
            info->achName,
            MAX_FILTER_NAME,
            name_[0] != L'\0'
                ? name_
                : L"FreeCam Camera"
        );

        info->pGraph = graph_;
        if (info->pGraph) {
            info->pGraph->AddRef();
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE JoinFilterGraph(
        IFilterGraph* graph,
        LPCWSTR name
    ) override {
        graph_ = graph;

        if (name) {
            StringCchCopyW(
                name_,
                MAX_FILTER_NAME,
                name
            );
        } else {
            name_[0] = L'\0';
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryVendorInfo(
        LPWSTR* vendor_info
    ) override {
        if (!vendor_info) {
            return E_POINTER;
        }

        constexpr wchar_t vendor[] = L"FreeCam";
        *vendor_info = static_cast<LPWSTR>(
            CoTaskMemAlloc(sizeof(vendor))
        );
        if (!*vendor_info) {
            return E_OUTOFMEMORY;
        }

        std::memcpy(
            *vendor_info,
            vendor,
            sizeof(vendor)
        );

        return S_OK;
    }

private:
    volatile LONG ref_count_;
    FreeCamPin pin_;

    std::mutex state_mutex_;
    FILTER_STATE state_ = State_Stopped;
    IReferenceClock* clock_ = nullptr;
    IFilterGraph* graph_ = nullptr;
    WCHAR name_[MAX_FILTER_NAME]{};
};

HRESULT FreeCamPin::QueryInterface(
    REFIID riid,
    void** object
) {
    if (!object) {
        return E_POINTER;
    }

    if (riid == IID_IUnknown ||
        riid == IID_IPin) {
        *object = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *object = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *object = static_cast<IKsPropertySet*>(this);
    } else {
        *object = nullptr;
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG FreeCamPin::AddRef() {
    return filter_->AddRef();
}

ULONG FreeCamPin::Release() {
    return filter_->Release();
}

HRESULT FreeCamPin::QueryPinInfo(
    PIN_INFO* info
) {
    if (!info) {
        return E_POINTER;
    }

    info->pFilter = filter_;
    filter_->AddRef();
    info->dir = PINDIR_OUTPUT;
    StringCchCopyW(
        info->achName,
        MAX_PIN_NAME,
        L"Output"
    );

    return S_OK;
}

HRESULT FreeCamPin::Connect(
    IPin* receive_pin,
    const AM_MEDIA_TYPE* requested_type
) {
    if (!receive_pin) {
        return E_POINTER;
    }

    std::lock_guard lock(connection_mutex_);

    if (connected_pin_) {
        return VFW_E_ALREADY_CONNECTED;
    }

    AM_MEDIA_TYPE type{};
    HRESULT hr = S_OK;

    if (requested_type) {
        if (!find_format(requested_type)) {
            return VFW_E_TYPE_NOT_ACCEPTED;
        }

        hr = copy_media_type(
            &type,
            requested_type
        );
    } else {
        const auto* preferred =
            find_format(preferred_format_);

        if (!preferred) {
            return E_FAIL;
        }

        hr = make_media_type(
            *preferred,
            &type
        );
    }

    if (FAILED(hr)) {
        return hr;
    }

    const HRESULT accept =
        receive_pin->QueryAccept(&type);

    if (accept != S_OK) {
        free_media_type(type);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    hr = receive_pin->ReceiveConnection(
        this,
        &type
    );

    if (FAILED(hr)) {
        free_media_type(type);
        return hr;
    }

    IMemInputPin* input = nullptr;
    hr = receive_pin->QueryInterface(
        IID_IMemInputPin,
        reinterpret_cast<void**>(&input)
    );

    if (FAILED(hr)) {
        receive_pin->Disconnect();
        free_media_type(type);
        return hr;
    }

    const auto* connected_format =
        find_format(&type);

    if (!connected_format) {
        input->Release();
        receive_pin->Disconnect();
        free_media_type(type);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    IMemAllocator* allocator = nullptr;
    hr = negotiate_allocator(
        input,
        connected_format->sample_bytes,
        &allocator
    );

    if (FAILED(hr)) {
        input->Release();
        receive_pin->Disconnect();
        free_media_type(type);
        return hr;
    }

    receive_pin->AddRef();
    connected_pin_ = receive_pin;
    mem_input_ = input;
    allocator_ = allocator;
    connection_type_ = type;
    preferred_format_ =
        connected_format->format;

    return S_OK;
}

HRESULT FreeCamPin::start_streaming() {
    std::lock_guard lock(connection_mutex_);

    if (!connected_pin_ ||
        !mem_input_ ||
        !allocator_) {
        return VFW_E_NOT_CONNECTED;
    }

    if (streaming_.load()) {
        return S_OK;
    }

    const HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) {
        return hr;
    }

    streaming_.store(true);

    try {
        stream_thread_ =
            std::thread(&FreeCamPin::stream_loop, this);
    } catch (...) {
        streaming_.store(false);
        allocator_->Decommit();
        return E_FAIL;
    }

    return S_OK;
}

void FreeCamPin::stop_streaming() {
    if (!streaming_.exchange(false)) {
        return;
    }

    stream_cv_.notify_all();

    if (stream_thread_.joinable()) {
        stream_thread_.join();
    }

    std::lock_guard lock(connection_mutex_);
    if (allocator_) {
        allocator_->Decommit();
    }
}

PinEnumerator::PinEnumerator(
    FreeCamFilter* filter
)
    : filter_(filter) {
    filter_->AddRef();
}

PinEnumerator::~PinEnumerator() {
    filter_->Release();
}

HRESULT PinEnumerator::QueryInterface(
    REFIID riid,
    void** object
) {
    if (!object) {
        return E_POINTER;
    }

    if (riid == IID_IUnknown ||
        riid == IID_IEnumPins) {
        *object = static_cast<IEnumPins*>(this);
        AddRef();
        return S_OK;
    }

    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG PinEnumerator::AddRef() {
    return static_cast<ULONG>(
        InterlockedIncrement(&ref_count_)
    );
}

ULONG PinEnumerator::Release() {
    const LONG value =
        InterlockedDecrement(&ref_count_);

    if (value == 0) {
        delete this;
        return 0;
    }

    return static_cast<ULONG>(value);
}

HRESULT PinEnumerator::Next(
    ULONG count,
    IPin** pins,
    ULONG* fetched
) {
    if (!pins) {
        return E_POINTER;
    }
    if (count > 1 && !fetched) {
        return E_POINTER;
    }

    ULONG produced = 0;

    if (index_ == 0 && count > 0) {
        IPin* pin =
            static_cast<IPin*>(filter_->pin());

        pin->AddRef();
        pins[0] = pin;

        index_ = 1;
        produced = 1;
    }

    if (fetched) {
        *fetched = produced;
    }

    return produced == count ? S_OK : S_FALSE;
}

HRESULT PinEnumerator::Skip(ULONG count) {
    if (count == 0) {
        return S_OK;
    }

    if (index_ == 0) {
        index_ = 1;
        return count == 1 ? S_OK : S_FALSE;
    }

    return S_FALSE;
}

HRESULT PinEnumerator::Reset() {
    index_ = 0;
    return S_OK;
}

HRESULT PinEnumerator::Clone(
    IEnumPins** clone
) {
    if (!clone) {
        return E_POINTER;
    }

    auto* enumerator =
        new (std::nothrow) PinEnumerator(filter_);

    if (!enumerator) {
        return E_OUTOFMEMORY;
    }

    enumerator->index_ = index_;
    *clone = enumerator;

    return S_OK;
}

class FreeCamClassFactory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object
    ) override {
        if (!object) {
            return E_POINTER;
        }

        if (riid == IID_IUnknown ||
            riid == IID_IClassFactory) {
            *object =
                static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(
            InterlockedIncrement(&ref_count_)
        );
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG value =
            InterlockedDecrement(&ref_count_);

        if (value == 0) {
            delete this;
            return 0;
        }

        return static_cast<ULONG>(value);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(
        IUnknown* outer,
        REFIID riid,
        void** object
    ) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;

        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }

        auto* filter =
            new (std::nothrow) FreeCamFilter();

        if (!filter) {
            return E_OUTOFMEMORY;
        }

        const HRESULT hr =
            filter->QueryInterface(riid, object);

        filter->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(
        BOOL lock
    ) override {
        if (lock) {
            g_server_locks.fetch_add(1);
        } else {
            g_server_locks.fetch_sub(1);
        }
        return S_OK;
    }

private:
    volatile LONG ref_count_ = 1;
};

HRESULT register_com_class() {
    wchar_t module_path[MAX_PATH]{};
    if (!GetModuleFileNameW(
            g_module,
            module_path,
            MAX_PATH
        )) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t guid[64]{};
    if (!StringFromGUID2(
            CLSID_FreeCamVirtualCamera,
            guid,
            static_cast<int>(std::size(guid))
        )) {
        return E_FAIL;
    }

    std::wstring key_path =
        L"CLSID\\\\";
    key_path += guid;
    key_path += L"\\\\InprocServer32";

    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(
        HKEY_CLASSES_ROOT,
        key_path.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_WRITE,
        nullptr,
        &key,
        nullptr
    );

    if (result != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(result);
    }

    result = RegSetValueExW(
        key,
        nullptr,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(module_path),
        static_cast<DWORD>(
            (wcslen(module_path) + 1) *
            sizeof(wchar_t)
        )
    );

    if (result == ERROR_SUCCESS) {
        constexpr wchar_t threading[] = L"Both";
        result = RegSetValueExW(
            key,
            L"ThreadingModel",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(threading),
            sizeof(threading)
        );
    }

    RegCloseKey(key);

    return result == ERROR_SUCCESS
        ? S_OK
        : HRESULT_FROM_WIN32(result);
}

HRESULT unregister_com_class() {
    wchar_t guid[64]{};
    if (!StringFromGUID2(
            CLSID_FreeCamVirtualCamera,
            guid,
            static_cast<int>(std::size(guid))
        )) {
        return E_FAIL;
    }

    std::wstring key_path = L"CLSID\\\\";
    key_path += guid;

    const LONG result = RegDeleteTreeW(
        HKEY_CLASSES_ROOT,
        key_path.c_str()
    );

    if (result == ERROR_FILE_NOT_FOUND) {
        return S_OK;
    }

    return result == ERROR_SUCCESS
        ? S_OK
        : HRESULT_FROM_WIN32(result);
}

HRESULT register_filter() {
    IFilterMapper2* mapper = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_FilterMapper2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IFilterMapper2,
        reinterpret_cast<void**>(&mapper)
    );

    if (FAILED(hr)) {
        return hr;
    }

    REGPINTYPES media_types[
        std::size(kFormats)
    ]{};

    for (std::size_t index = 0;
         index < std::size(kFormats);
         ++index) {
        media_types[index].clsMajorType =
            &MEDIATYPE_Video;
        media_types[index].clsMinorType =
            kFormats[index].subtype;
    }

    REGFILTERPINS2 pin{};
    pin.dwFlags = REG_PINFLAG_B_OUTPUT;
    pin.cInstances = 1;
    pin.nMediaTypes =
        static_cast<UINT>(std::size(kFormats));
    pin.lpMediaType = media_types;
    pin.nMediums = 0;
    pin.lpMedium = nullptr;
    pin.clsPinCategory = &PIN_CATEGORY_CAPTURE;

    REGFILTER2 filter{};
    filter.dwVersion = 2;
    filter.dwMerit = MERIT_DO_NOT_USE;
    filter.cPins2 = 1;
    filter.rgPins2 = &pin;

    hr = mapper->RegisterFilter(
        CLSID_FreeCamVirtualCamera,
        L"FreeCam Camera",
        nullptr,
        &CLSID_VideoInputDeviceCategory,
        L"FreeCam Camera",
        &filter
    );

    mapper->Release();
    return hr;
}

HRESULT unregister_filter() {
    IFilterMapper2* mapper = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_FilterMapper2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IFilterMapper2,
        reinterpret_cast<void**>(&mapper)
    );

    if (FAILED(hr)) {
        return hr;
    }

    hr = mapper->UnregisterFilter(
        &CLSID_VideoInputDeviceCategory,
        L"FreeCam Camera",
        CLSID_FreeCamVirtualCamera
    );

    mapper->Release();

    if (hr == VFW_E_NOT_FOUND) {
        return S_OK;
    }

    return hr;
}

} // namespace

extern "C" BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID reserved
) {
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }

    return TRUE;
}

STDAPI DllGetClassObject(
    REFCLSID class_id,
    REFIID riid,
    void** object
) {
    if (!object) {
        return E_POINTER;
    }
    *object = nullptr;

    if (class_id != CLSID_FreeCamVirtualCamera) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory =
        new (std::nothrow) FreeCamClassFactory();

    if (!factory) {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr =
        factory->QueryInterface(riid, object);

    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return
        g_object_count.load() == 0 &&
        g_server_locks.load() == 0
            ? S_OK
            : S_FALSE;
}

STDAPI DllRegisterServer() {
    HRESULT init = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    const bool uninitialize =
        SUCCEEDED(init);

    if (init == RPC_E_CHANGED_MODE) {
        init = S_OK;
    }

    if (FAILED(init)) {
        return init;
    }

    HRESULT hr = register_com_class();

    if (SUCCEEDED(hr)) {
        hr = register_filter();
    }

    if (FAILED(hr)) {
        unregister_filter();
        unregister_com_class();
    }

    if (uninitialize) {
        CoUninitialize();
    }

    return hr;
}

STDAPI DllUnregisterServer() {
    HRESULT init = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    const bool uninitialize =
        SUCCEEDED(init);

    if (init == RPC_E_CHANGED_MODE) {
        init = S_OK;
    }

    HRESULT filter_hr = S_OK;

    if (SUCCEEDED(init)) {
        filter_hr = unregister_filter();
    }

    const HRESULT class_hr =
        unregister_com_class();

    if (uninitialize) {
        CoUninitialize();
    }

    return FAILED(filter_hr)
        ? filter_hr
        : class_hr;
}
