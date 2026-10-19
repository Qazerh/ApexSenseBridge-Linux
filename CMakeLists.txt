cmake_minimum_required(VERSION 3.20)
project(ApexSenseBridge VERSION 1.0.2 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This repository builds the Linux bridge only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(ASB_RELEASE_LABEL="${PROJECT_VERSION}")
find_package(Threads REQUIRED)

add_library(asb_core STATIC
    src/core/ApexProfileRestoreGuard.cpp
    src/dualsense/DualSenseFeedback.cpp
    src/dualsense/DualSenseInput.cpp
    src/dualsense/TouchpadGestureProfile.cpp
    src/dualsense/AdaptiveTriggerTranslation.cpp
    src/dualsense/AdaptiveTriggerBridge.cpp
    src/dualsense/Apex6TriggerEffect.cpp
    src/dualsense/Apex6HapticBridge.cpp
    src/dualsense/RumbleBridge.cpp
    src/dualsense/LightbarBridge.cpp
    src/haptics/HapticProcessor.cpp
    src/platform/XInputMapping.cpp
    src/flydigi/Apex4Input.cpp
    src/flydigi/Apex4Protocol.cpp
    src/flydigi/Apex5Identity.cpp
    src/flydigi/Apex5Input.cpp
    src/flydigi/Apex5Protocol.cpp
    src/flydigi/Apex6Protocol.cpp
    src/flydigi/Apex5Device.cpp
    src/platform/linux/LinuxHidTransport.cpp
    src/platform/linux/InputStream.cpp
    src/platform/linux/InputPump.cpp
    src/platform/linux/EvdevInput.cpp
    src/platform/linux/EvdevRumble.cpp
    src/platform/linux/BridgeLifecycle.cpp
    src/platform/linux/FeedbackBridge.cpp
    src/platform/linux/AsyncDiagnostics.cpp
    src/platform/linux/Options.cpp
    src/platform/linux/RuntimeSupport.cpp
    src/cli/JsonSupport.cpp
    src/platform/linux/HidReportDescriptor.cpp
    src/platform/linux/DualSenseDescriptor.cpp
    src/platform/linux/DualSenseReports.cpp
    src/platform/linux/UhidDevice.cpp
    src/platform/linux/EvdevGrab.cpp
)
target_include_directories(asb_core PUBLIC src)
target_compile_options(asb_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(asb_core PUBLIC Threads::Threads)

add_executable(ApexSenseBridge src/platform/linux/LinuxMain.cpp)
target_compile_options(ApexSenseBridge PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ApexSenseBridge PRIVATE asb_core)

install(TARGETS ApexSenseBridge RUNTIME DESTINATION bin)
install(PROGRAMS scripts/apexsensebridge-steam DESTINATION bin)
install(FILES packaging/linux/70-apexsensebridge.rules packaging/linux/apexsensebridge.conf
        DESTINATION share/ApexSenseBridge)
install(FILES LICENSE THIRD_PARTY_NOTICES.md packaging/linux/VIIPER-LICENSE.txt
        DESTINATION share/ApexSenseBridge/licenses)

include(CTest)
if(BUILD_TESTING)
    function(asb_test target name source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE asb_core)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME ${name} COMMAND ${target})
    endfunction()

    asb_test(ApexSenseBridgeLinuxTests linux_hid_uhid_bridge tests/test_linux_bridge.cpp)
    asb_test(ApexSenseBridgeLinuxFeatureTests linux_features tests/test_linux_features.cpp)
    asb_test(ApexSenseBridgeLinuxReconnectTests linux_reconnect tests/test_linux_reconnect.cpp)
    asb_test(ApexSenseBridgeLinuxLatencyTests linux_input_latency tests/test_linux_latency.cpp)
    asb_test(ApexSenseBridgeLinuxModelTests linux_controller_models tests/test_linux_models.cpp)
    asb_test(ApexSenseBridgeTests protocol tests/test_protocol.cpp)
    asb_test(ApexSenseBridgeApex6ProtocolTests apex6_protocol tests/test_apex6_protocol.cpp)
    asb_test(ApexSenseBridgeIdentityTests identity_guard tests/test_identity.cpp)
    asb_test(ApexSenseBridgeTriggerTranslationTests adaptive_trigger_translation tests/test_adaptive_trigger_translation.cpp)
    asb_test(ApexSenseBridgeTriggerBridgeTests adaptive_trigger_bridge tests/test_adaptive_trigger_bridge.cpp)
    asb_test(ApexSenseBridgeFeedbackWriterTests apex_feedback_writer tests/test_apex_feedback_writer.cpp)
    asb_test(ApexSenseBridgeRumbleTests rumble_bridge tests/test_rumble_bridge.cpp)
    asb_test(ApexSenseBridgeApex6HapticTests apex6_haptic_bridge tests/test_apex6_haptic_bridge.cpp)
    asb_test(ApexSenseBridgeHapticProcessorTests haptic_processor tests/test_haptic_processor.cpp)
    asb_test(ApexSenseBridgeXInputMappingTests xinput_mapping tests/test_xinput_mapping.cpp)
    asb_test(ApexSenseBridgeTouchpadGestureProfileTests touchpad_gesture_profiles tests/test_touchpad_gesture_profile.cpp)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME linux_steam_launcher COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_steam_launcher.py ${CMAKE_CURRENT_SOURCE_DIR}/scripts/apexsensebridge-steam)
    add_test(NAME linux_cli_contract COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_linux_cli.py $<TARGET_FILE:ApexSenseBridge>)
    set_tests_properties(linux_reconnect linux_steam_launcher linux_cli_contract PROPERTIES TIMEOUT 30)
    add_test(NAME linux_cli_help COMMAND ApexSenseBridge --help)
    add_test(NAME linux_cli_invalid_strength COMMAND ApexSenseBridge bridge --strength 101)
    set_tests_properties(linux_cli_invalid_strength PROPERTIES WILL_FAIL TRUE)
    add_test(NAME linux_cli_invalid_duration COMMAND ApexSenseBridge test-triggers --seconds -1)
    set_tests_properties(linux_cli_invalid_duration PROPERTIES WILL_FAIL TRUE)
endif()
