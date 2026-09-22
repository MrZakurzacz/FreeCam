cmake_minimum_required(VERSION 3.20)

project(FreeCamReceiver LANGUAGES CXX)

add_executable(FreeCamReceiver
    src/main.cpp
    src/video_receiver.cpp
    src/video_receiver.h
)

target_compile_features(FreeCamReceiver PRIVATE cxx_std_20)

target_compile_definitions(FreeCamReceiver PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(FreeCamReceiver PRIVATE ws2_32)
