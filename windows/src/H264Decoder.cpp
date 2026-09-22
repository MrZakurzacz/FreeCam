#include "H264Decoder.hpp"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

using Microsoft::WRL::ComPtr;

struct H264Decoder::Impl {
    ComPtr<IMFTransform> decoder;
    UINT32 width = 1280;
    UINT32 height = 720;
    LONG stride = 1280;
    MFT_OUTPUT_STREAM_INFO output_info{};
    bool output_configured = false;
};

namespace {

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(
        std::clamp(value, 0, 255)
    );
}

} // namespace

H264Decoder::H264Decoder(FrameHandler frame_handler)
    : impl_(new Impl()),
      frame_handler_(std::move(frame_handler)) {
}

H264Decoder::~H264Decoder() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool H264Decoder::initialize() {
    if (impl_->decoder) {
        return true;
    }

    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&impl_->decoder)
    );

    if (FAILED(hr)) {
        std::cerr << "Could not create Microsoft H.264 decoder: 0x"
                  << std::hex << static_cast<unsigned long>(hr)
                  << std::dec << "\n";
        return false;
    }

    ComPtr<IMFAttributes> decoder_attributes;
    if (SUCCEEDED(impl_->decoder->GetAttributes(&decoder_attributes))) {
        decoder_attributes->SetUINT32(
            CODECAPI_AVLowLatencyMode,
            TRUE
        );
    }

    ComPtr<IMFMediaType> input_type;
    hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) {
        return false;
    }

    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264_ES);
    MFSetAttributeSize(
        input_type.Get(),
        MF_MT_FRAME_SIZE,
        impl_->width,
        impl_->height
    );
    MFSetAttributeRatio(
        input_type.Get(),
        MF_MT_FRAME_RATE,
        30,
        1
    );
    MFSetAttributeRatio(
        input_type.Get(),
        MF_MT_PIXEL_ASPECT_RATIO,
        1,
        1
    );
    input_type->SetUINT32(
        MF_MT_INTERLACE_MODE,
        MFVideoInterlace_Progressive
    );

    hr = impl_->decoder->SetInputType(
        0,
        input_type.Get(),
        0
    );

    if (FAILED(hr)) {
        std::cerr << "Could not set H.264 decoder input type: 0x"
                  << std::hex << static_cast<unsigned long>(hr)
                  << std::dec << "\n";
        shutdown();
        return false;
    }

    if (!configureOutput()) {
        shutdown();
        return false;
    }

    impl_->decoder->ProcessMessage(
        MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
        0
    );
    impl_->decoder->ProcessMessage(
        MFT_MESSAGE_NOTIFY_START_OF_STREAM,
        0
    );

    std::cout << "H.264 decoder ready: "
              << impl_->width << "x" << impl_->height
              << " NV12\n";

    return true;
}

bool H264Decoder::configureOutput() {
    for (DWORD index = 0;; ++index) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = impl_->decoder->GetOutputAvailableType(
            0,
            index,
            &type
        );

        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }

        if (FAILED(hr)) {
            std::cerr << "Could not enumerate decoder output types: 0x"
                      << std::hex << static_cast<unsigned long>(hr)
                      << std::dec << "\n";
            return false;
        }

        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            continue;
        }

        if (subtype != MFVideoFormat_NV12) {
            continue;
        }

        const HRESULT set_hr = impl_->decoder->SetOutputType(
            0,
            type.Get(),
            0
        );

        if (FAILED(set_hr)) {
            continue;
        }

        UINT32 width = 0;
        UINT32 height = 0;
        if (SUCCEEDED(MFGetAttributeSize(
                type.Get(),
                MF_MT_FRAME_SIZE,
                &width,
                &height
            ))) {
            impl_->width = width;
            impl_->height = height;
        }

        LONG stride = 0;
        UINT32 stride_value = 0;

        if (SUCCEEDED(type->GetUINT32(
                MF_MT_DEFAULT_STRIDE,
                &stride_value
            ))) {
            stride = static_cast<LONG>(stride_value);
        } else {
            MFGetStrideForBitmapInfoHeader(
                MFVideoFormat_NV12.Data1,
                impl_->width,
                &stride
            );
        }

        if (stride == 0) {
            stride = static_cast<LONG>(impl_->width);
        }

        impl_->stride = stride;

        if (FAILED(impl_->decoder->GetOutputStreamInfo(
                0,
                &impl_->output_info
            ))) {
            std::cerr << "Could not read decoder output stream info.\n";
            return false;
        }

        impl_->output_configured = true;
        return true;
    }

    std::cerr << "Microsoft H.264 decoder did not expose NV12 output.\n";
    return false;
}

bool H264Decoder::decode(const EncodedFrame& frame) {
    if (!impl_->decoder && !initialize()) {
        return false;
    }

    if (frame.data.empty()) {
        return true;
    }

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;

    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        return false;
    }

    if (frame.data.size() >
        static_cast<std::size_t>(
            std::numeric_limits<DWORD>::max()
        )) {
        return false;
    }

    hr = MFCreateMemoryBuffer(
        static_cast<DWORD>(frame.data.size()),
        &buffer
    );
    if (FAILED(hr)) {
        return false;
    }

    BYTE* destination = nullptr;
    DWORD maximum_length = 0;

    hr = buffer->Lock(
        &destination,
        &maximum_length,
        nullptr
    );
    if (FAILED(hr)) {
        return false;
    }

    std::memcpy(
        destination,
        frame.data.data(),
        frame.data.size()
    );

    buffer->Unlock();
    buffer->SetCurrentLength(
        static_cast<DWORD>(frame.data.size())
    );

    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(
        static_cast<LONGLONG>(frame.timestamp_us) * 10
    );
    sample->SetSampleDuration(10'000'000 / 30);

    hr = impl_->decoder->ProcessInput(
        0,
        sample.Get(),
        0
    );

    if (hr == MF_E_NOTACCEPTING) {
        if (!drainOutput()) {
            return false;
        }

        hr = impl_->decoder->ProcessInput(
            0,
            sample.Get(),
            0
        );
    }

    if (FAILED(hr)) {
        std::cerr << "H.264 ProcessInput failed: 0x"
                  << std::hex << static_cast<unsigned long>(hr)
                  << std::dec << "\n";
        return false;
    }

    return drainOutput();
}

bool H264Decoder::drainOutput() {
    while (true) {
        ComPtr<IMFSample> caller_sample;

        if ((impl_->output_info.dwFlags &
             MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            HRESULT hr = MFCreateSample(&caller_sample);
            if (FAILED(hr)) {
                return false;
            }

            const DWORD minimum_size =
                std::max<DWORD>(
                    impl_->output_info.cbSize,
                    impl_->width * impl_->height * 3 / 2
                );

            ComPtr<IMFMediaBuffer> output_buffer;
            hr = MFCreateMemoryBuffer(
                minimum_size,
                &output_buffer
            );
            if (FAILED(hr)) {
                return false;
            }

            caller_sample->AddBuffer(output_buffer.Get());
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = caller_sample.Get();

        DWORD status = 0;
        HRESULT hr = impl_->decoder->ProcessOutput(
            0,
            1,
            &output,
            &status
        );

        if (output.pEvents) {
            output.pEvents->Release();
            output.pEvents = nullptr;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!configureOutput()) {
                return false;
            }
            continue;
        }

        if (FAILED(hr)) {
            std::cerr << "H.264 ProcessOutput failed: 0x"
                      << std::hex << static_cast<unsigned long>(hr)
                      << std::dec << "\n";
            return false;
        }

        if (caller_sample && output.pSample == caller_sample.Get()) {
            if (!deliverOutputSample(caller_sample.Get())) {
                return false;
            }
        } else if (output.pSample) {
            ComPtr<IMFSample> provided_sample;
            provided_sample.Attach(output.pSample);

            if (!deliverOutputSample(provided_sample.Get())) {
                return false;
            }
        }
    }
}

bool H264Decoder::deliverOutputSample(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) {
        return false;
    }

    BYTE* data = nullptr;
    DWORD maximum_length = 0;
    DWORD current_length = 0;

    hr = buffer->Lock(
        &data,
        &maximum_length,
        &current_length
    );

    if (FAILED(hr)) {
        return false;
    }

    const LONG stride = std::abs(impl_->stride);
    const std::size_t y_bytes =
        static_cast<std::size_t>(stride) * impl_->height;
    const std::size_t required =
        y_bytes +
        static_cast<std::size_t>(stride) * impl_->height / 2;

    if (current_length < required) {
        buffer->Unlock();
        std::cerr << "Decoded NV12 buffer is smaller than expected.\n";
        return false;
    }

    DecodedFrame frame;
    frame.width = impl_->width;
    frame.height = impl_->height;
    frame.bgra.resize(
        static_cast<std::size_t>(frame.width) *
        frame.height *
        4
    );

    const auto* y_plane = data;
    const auto* uv_plane = data + y_bytes;

    for (UINT32 y = 0; y < frame.height; ++y) {
        for (UINT32 x = 0; x < frame.width; ++x) {
            const int y_value =
                static_cast<int>(
                    y_plane[
                        static_cast<std::size_t>(y) * stride + x
                    ]
                );

            const std::size_t uv_index =
                static_cast<std::size_t>(y / 2) * stride +
                (x & ~1U);

            const int u =
                static_cast<int>(uv_plane[uv_index]) - 128;
            const int v =
                static_cast<int>(uv_plane[uv_index + 1]) - 128;

            const int c = std::max(0, y_value - 16);

            const int r =
                (298 * c + 409 * v + 128) >> 8;
            const int g =
                (298 * c - 100 * u - 208 * v + 128) >> 8;
            const int b =
                (298 * c + 516 * u + 128) >> 8;

            const std::size_t output_index =
                (
                    static_cast<std::size_t>(y) *
                    frame.width +
                    x
                ) * 4;

            frame.bgra[output_index + 0] = clamp_byte(b);
            frame.bgra[output_index + 1] = clamp_byte(g);
            frame.bgra[output_index + 2] = clamp_byte(r);
            frame.bgra[output_index + 3] = 255;
        }
    }

    buffer->Unlock();

    if (frame_handler_) {
        frame_handler_(std::move(frame));
    }

    return true;
}

void H264Decoder::shutdown() {
    if (!impl_ || !impl_->decoder) {
        return;
    }

    impl_->decoder->ProcessMessage(
        MFT_MESSAGE_NOTIFY_END_OF_STREAM,
        0
    );
    impl_->decoder->ProcessMessage(
        MFT_MESSAGE_NOTIFY_END_STREAMING,
        0
    );

    impl_->decoder.Reset();
    impl_->output_configured = false;
}
