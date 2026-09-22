#include <windows.h>
#include <dshow.h>

#include <iostream>
#include <string>

int wmain() {
    const HRESULT init = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    if (FAILED(init)) {
        std::wcerr << L"CoInitializeEx failed: 0x"
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
                    const HRESULT bind = moniker->BindToObject(
                        nullptr,
                        nullptr,
                        IID_IBaseFilter,
                        reinterpret_cast<void**>(&filter)
                    );

                    if (SUCCEEDED(bind) && filter) {
                        std::wcout
                            << L"FreeCam Camera instantiated successfully.\n";
                        filter->Release();
                        found = true;
                    } else {
                        std::wcerr
                            << L"FreeCam Camera was registered but could not be instantiated.\n";
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

    return 0;
}
