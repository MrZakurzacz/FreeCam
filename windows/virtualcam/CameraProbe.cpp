#include <windows.h>
#include <dshow.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

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

std::wstring subtype_name(const GUID& subtype) {
    if (subtype == MEDIASUBTYPE_NV12) {
        return L"NV12";
    }
    if (subtype == MEDIASUBTYPE_I420) {
        return L"I420";
    }
    if (subtype == MEDIASUBTYPE_YUY2) {
        return L"YUY2";
    }
    if (subtype == MEDIASUBTYPE_RGB32) {
        return L"RGB32";
    }

    wchar_t guid[64]{};
    StringFromGUID2(
        subtype,
        guid,
        static_cast<int>(std::size(guid))
    );
    return guid;
}

bool verify_capabilities(
    IBaseFilter* filter
) {
    IPin* output = nullptr;

    HRESULT hr = filter->FindPin(
        L"Output",
        &output
    );

    if (FAILED(hr) || !output) {
        std::wcerr
            << L"Could not find FreeCam output pin.\n";
        return false;
    }

    IAMStreamConfig* config = nullptr;

    hr = output->QueryInterface(
        IID_IAMStreamConfig,
        reinterpret_cast<void**>(&config)
    );

    if (FAILED(hr) || !config) {
        std::wcerr
            << L"FreeCam output pin has no IAMStreamConfig.\n";
        output->Release();
        return false;
    }

    int count = 0;
    int caps_size = 0;

    hr = config->GetNumberOfCapabilities(
        &count,
        &caps_size
    );

    if (FAILED(hr) ||
        count <= 0 ||
        caps_size != sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
        std::wcerr
            << L"Invalid camera capability list.\n";
        config->Release();
        output->Release();
        return false;
    }

    bool has_nv12 = false;
    bool has_i420 = false;
    bool has_yuy2 = false;
    bool has_rgb32 = false;

    for (int index = 0; index < count; ++index) {
        AM_MEDIA_TYPE* media_type = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps{};

        hr = config->GetStreamCaps(
            index,
            &media_type,
            reinterpret_cast<BYTE*>(&caps)
        );

        if (FAILED(hr) || !media_type) {
            continue;
        }

        std::wcout
            << L"FreeCam capability "
            << index
            << L": "
            << subtype_name(media_type->subtype)
            << L"\n";

        has_nv12 =
            has_nv12 ||
            media_type->subtype == MEDIASUBTYPE_NV12;

        has_i420 =
            has_i420 ||
            media_type->subtype == MEDIASUBTYPE_I420;

        has_yuy2 =
            has_yuy2 ||
            media_type->subtype == MEDIASUBTYPE_YUY2;

        has_rgb32 =
            has_rgb32 ||
            media_type->subtype == MEDIASUBTYPE_RGB32;

        free_media_type(*media_type);
        CoTaskMemFree(media_type);
    }

    config->Release();
    output->Release();

    if (!has_nv12 ||
        !has_i420 ||
        !has_yuy2 ||
        !has_rgb32) {
        std::wcerr
            << L"FreeCam is missing one or more expected formats.\n";
        return false;
    }

    return true;
}

bool run_capture_graph(
    IBaseFilter* source
) {
    IGraphBuilder* graph = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_FilterGraph,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IGraphBuilder,
        reinterpret_cast<void**>(&graph)
    );

    if (FAILED(hr) || !graph) {
        std::wcerr
            << L"Could not create DirectShow graph.\n";
        return false;
    }

    IBaseFilter* renderer = nullptr;

    hr = CoCreateInstance(
        CLSID_NullRenderer,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IBaseFilter,
        reinterpret_cast<void**>(&renderer)
    );

    if (FAILED(hr) || !renderer) {
        graph->Release();
        std::wcerr
            << L"Could not create null renderer.\n";
        return false;
    }

    hr = graph->AddFilter(
        source,
        L"FreeCam Camera"
    );

    if (FAILED(hr)) {
        renderer->Release();
        graph->Release();
        std::wcerr
            << L"Could not add FreeCam to graph.\n";
        return false;
    }

    hr = graph->AddFilter(
        renderer,
        L"Null Renderer"
    );

    if (FAILED(hr)) {
        renderer->Release();
        graph->Release();
        std::wcerr
            << L"Could not add null renderer to graph.\n";
        return false;
    }

    IPin* source_pin = nullptr;
    source->FindPin(
        L"Output",
        &source_pin
    );

    IEnumPins* renderer_pins = nullptr;
    renderer->EnumPins(&renderer_pins);

    IPin* renderer_pin = nullptr;

    if (renderer_pins) {
        renderer_pins->Next(
            1,
            &renderer_pin,
            nullptr
        );
        renderer_pins->Release();
    }

    if (!source_pin || !renderer_pin) {
        if (source_pin) {
            source_pin->Release();
        }
        if (renderer_pin) {
            renderer_pin->Release();
        }
        renderer->Release();
        graph->Release();
        std::wcerr
            << L"Could not obtain graph pins.\n";
        return false;
    }

    hr = graph->Connect(
        source_pin,
        renderer_pin
    );

    source_pin->Release();
    renderer_pin->Release();

    if (FAILED(hr)) {
        renderer->Release();
        graph->Release();
        std::wcerr
            << L"Could not connect FreeCam capture graph: 0x"
            << std::hex
            << static_cast<unsigned long>(hr)
            << L"\n";
        return false;
    }

    IMediaControl* control = nullptr;

    hr = graph->QueryInterface(
        IID_IMediaControl,
        reinterpret_cast<void**>(&control)
    );

    if (FAILED(hr) || !control) {
        renderer->Release();
        graph->Release();
        return false;
    }

    hr = control->Pause();

    if (FAILED(hr)) {
        std::wcerr
            << L"DirectShow graph Pause failed: 0x"
            << std::hex
            << static_cast<unsigned long>(hr)
            << L"\n";
        control->Release();
        renderer->Release();
        graph->Release();
        return false;
    }

    OAFilterState state{};

    hr = control->GetState(
        2000,
        &state
    );

    if (FAILED(hr)) {
        std::wcerr
            << L"Paused live graph did not settle: 0x"
            << std::hex
            << static_cast<unsigned long>(hr)
            << L"\n";
        control->Release();
        renderer->Release();
        graph->Release();
        return false;
    }

    hr = control->Run();

    if (FAILED(hr)) {
        std::wcerr
            << L"DirectShow graph Run failed: 0x"
            << std::hex
            << static_cast<unsigned long>(hr)
            << L"\n";
        control->Release();
        renderer->Release();
        graph->Release();
        return false;
    }

    std::this_thread::sleep_for(
        std::chrono::milliseconds(250)
    );

    control->Stop();

    std::wcout
        << L"FreeCam DirectShow graph ran successfully.\n";

    control->Release();
    renderer->Release();
    graph->Release();

    return true;
}

} // namespace

int wmain() {
    const HRESULT init = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    if (FAILED(init)) {
        std::wcerr
            << L"CoInitializeEx failed: 0x"
            << std::hex
            << static_cast<unsigned long>(init)
            << L"\n";
        return 1;
    }

    ICreateDevEnum* device_enum = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_SystemDeviceEnum,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ICreateDevEnum,
        reinterpret_cast<void**>(&device_enum)
    );

    if (FAILED(hr)) {
        CoUninitialize();
        return 1;
    }

    IEnumMoniker* devices = nullptr;

    hr = device_enum->CreateClassEnumerator(
        CLSID_VideoInputDeviceCategory,
        &devices,
        0
    );

    device_enum->Release();

    if (hr != S_OK || !devices) {
        std::wcerr
            << L"No video capture devices were enumerated.\n";
        CoUninitialize();
        return 2;
    }

    bool found = false;
    bool capabilities_ok = false;
    bool graph_ok = false;

    IMoniker* moniker = nullptr;

    while (devices->Next(
               1,
               &moniker,
               nullptr
           ) == S_OK) {
        IPropertyBag* properties = nullptr;

        if (SUCCEEDED(
                moniker->BindToStorage(
                    nullptr,
                    nullptr,
                    IID_IPropertyBag,
                    reinterpret_cast<void**>(&properties)
                )
            )) {
            VARIANT name;
            VariantInit(&name);

            if (SUCCEEDED(
                    properties->Read(
                        L"FriendlyName",
                        &name,
                        nullptr
                    )
                ) &&
                name.vt == VT_BSTR &&
                name.bstrVal) {
                std::wcout
                    << L"Camera: "
                    << name.bstrVal
                    << L"\n";

                if (std::wstring(name.bstrVal) ==
                    L"FreeCam Camera") {
                    IBaseFilter* filter = nullptr;

                    const HRESULT bind =
                        moniker->BindToObject(
                            nullptr,
                            nullptr,
                            IID_IBaseFilter,
                            reinterpret_cast<void**>(&filter)
                        );

                    if (SUCCEEDED(bind) && filter) {
                        std::wcout
                            << L"FreeCam Camera instantiated successfully.\n";

                        found = true;
                        capabilities_ok =
                            verify_capabilities(filter);
                        graph_ok =
                            run_capture_graph(filter);

                        filter->Release();
                    }
                }
            }

            VariantClear(&name);
            properties->Release();
        }

        moniker->Release();
        moniker = nullptr;
    }

    devices->Release();
    CoUninitialize();

    if (!found) {
        std::wcerr
            << L"FreeCam Camera was not found.\n";
        return 3;
    }

    if (!capabilities_ok) {
        return 4;
    }

    if (!graph_ok) {
        return 5;
    }

    return 0;
}
