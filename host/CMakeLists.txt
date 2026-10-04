set(REMOTE_TOUCH_SOURCES
    src/app/host_options.cpp
    src/app/remote_touch_host.cpp
    src/capture/desktop_capture.cpp
    src/encoder/h264_encoder.cpp
    src/input/touch_contact_tracker.cpp
    src/input/windows_touch_injector.cpp
    src/network/http_websocket_server.cpp
    src/network/network_addresses.cpp
    src/protocol/touch_packet.cpp
    src/streaming/desktop_streamer.cpp
)

add_library(remote_touch_core STATIC ${REMOTE_TOUCH_SOURCES})
target_compile_features(remote_touch_core PUBLIC cxx_std_20)
target_compile_definitions(remote_touch_core
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
)
target_include_directories(remote_touch_core PUBLIC src)
target_link_libraries(remote_touch_core PUBLIC
    bcrypt
    d3d11
    dxgi
    iphlpapi
    mf
    mfplat
    mfuuid
    ole32
    strmiids
    user32
    wmcodecdspuuid
    ws2_32
)

if(MSVC)
    target_compile_options(remote_touch_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(remote_touch_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(RemoteTouchHost src/main.cpp)
target_link_libraries(RemoteTouchHost PRIVATE remote_touch_core)

add_custom_command(TARGET RemoteTouchHost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/web"
            "$<TARGET_FILE_DIR:RemoteTouchHost>/web"
    COMMENT "Copying web client next to RemoteTouchHost"
)

if(BUILD_TESTING)
    add_executable(touch_packet_tests tests/touch_packet_tests.cpp)
    target_link_libraries(touch_packet_tests PRIVATE remote_touch_core)
    add_test(NAME touch_packet_tests COMMAND touch_packet_tests)

    add_executable(touch_contact_tracker_tests tests/touch_contact_tracker_tests.cpp)
    target_link_libraries(touch_contact_tracker_tests PRIVATE remote_touch_core)
    add_test(NAME touch_contact_tracker_tests COMMAND touch_contact_tracker_tests)

    # Manual smoke target: it requires an interactive Windows desktop, so it is
    # intentionally not registered with CTest for headless CI environments.
    add_executable(video_pipeline_smoke tests/video_pipeline_smoke.cpp)
    target_link_libraries(video_pipeline_smoke PRIVATE remote_touch_core)
endif()
