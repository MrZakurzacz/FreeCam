cmake_minimum_required(VERSION 3.20)

project(FreeCamReceiver LANGUAGES CXX)

add_executable(FreeCamReceiver
    src/main.cpp
    src/VideoReceiver.cpp
    src/VideoReceiver.hpp
    src/H264Decoder.cpp
    src/H264Decoder.hpp
    src/PreviewWindow.cpp
    src/PreviewWindow.hpp
)

target_compile_features(FreeCamReceiver PRIVATE cxx_std_20)

target_compile_definitions(FreeCamReceiver PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(FreeCamReceiver PRIVATE
    ws2_32
    ole32
    mfplat
    mfuuid
    wmcodecdspuuid
    user32
    gdi32
)
