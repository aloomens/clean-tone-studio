cmake_minimum_required(VERSION 3.22)

project(CleanToneStudio VERSION 1.6.3 LANGUAGES C CXX)

# Product name shown to users (files, window, DAW). 1.6.0 was called "Open Clean Tone Studio"; the name went back
# in 1.6.1. The CMake target, the plugin codes (Ctna / Ctgs) and the saved-state tag never changed, so the
# plugin's VST3 IDs are the same in every version and all projects load it.
set(CTS_PRODUCT_NAME "Clean Tone Studio")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(external/JUCE)

# version and name shown in the plugin window and saved with settings (also used for the zip names)
add_compile_definitions(CTS_VERSION="${PROJECT_VERSION}" CTS_PRODUCT_NAME="${CTS_PRODUCT_NAME}")

# JUCE's oversampler zeroes tiny filter states once per buffer, which makes the sound depend (very slightly)
# on the buffer size. Denormals are already flushed in hardware (ScopedNoDenormals), so turn that off.
add_compile_definitions(JUCE_DSP_ENABLE_SNAP_TO_ZERO=0)

# Windows: VST3 + app. Linux also builds LV2 (for Ardour and other LV2 hosts).
# The LV2 URI identifies the plugin in saved LV2 sessions: never change it once a Linux build is out.
set(CTS_FORMATS VST3 Standalone)
set(CTS_COMPANY_EMAIL "openstudio.plugins@gmail.com")
if(UNIX AND NOT APPLE)
    list(APPEND CTS_FORMATS LV2)
    set(CTS_COMPANY_EMAIL "mailto:openstudio.plugins@gmail.com")   # LV2 writes it as a URI (foaf:mbox)
endif()

juce_add_plugin(CleanToneStudio
    COMPANY_NAME "CleanTone Audio"
    COMPANY_WEBSITE "https://aloomens.github.io/clean-tone-studio/"
    COMPANY_EMAIL "${CTS_COMPANY_EMAIL}"
    PRODUCT_NAME "${CTS_PRODUCT_NAME}"
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Ctna
    PLUGIN_CODE Ctgs
    FORMATS ${CTS_FORMATS}
    LV2URI "https://aloomens.github.io/clean-tone-studio/"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    BUNDLE_ID com.cleantoneaudio.cleantonestudio
    VST3_CATEGORIES Fx Distortion Delay Reverb)

# JUCE writes the Windows version resource once and doesn't notice a new VERSION; refresh it on every configure
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/CleanToneStudio_artefacts/JuceLibraryCode/CleanToneStudio_resources.rc")

target_sources(CleanToneStudio PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(CleanToneStudio PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Linux app: offer JACK (and PipeWire's JACK) next to ALSA for low latency. JUCE loads libjack at
    # run time, so a system without JACK simply doesn't list it.
    $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>)

# Linux app file without spaces (easier to start from a terminal); the window title, settings and
# plugin names keep the product name. Windows keeps "Clean Tone Studio.exe".
if(UNIX AND NOT APPLE)
    set_target_properties(CleanToneStudio_Standalone PROPERTIES OUTPUT_NAME "CleanToneStudio")
endif()

# ---- ASIO for the standalone app: Steinberg's ASIO SDK in external/asiosdk, used under GPLv3 ----
set(ASIO_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/asiosdk" CACHE PATH "Folder containing the ASIO SDK (with common/iasiodrv.h)")
if(WIN32 AND EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
    message(STATUS "ASIO SDK found at ${ASIO_SDK_DIR} - standalone app will support ASIO")
    target_compile_definitions(CleanToneStudio PUBLIC JUCE_ASIO=1)
    target_include_directories(CleanToneStudio PUBLIC "${ASIO_SDK_DIR}/common")
elseif(WIN32)
    message(WARNING "ASIO SDK not found in ${ASIO_SDK_DIR} - standalone app will only offer Windows Audio/DirectSound")
endif()

target_link_libraries(CleanToneStudio
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Offline test host: loads the built VST3 and runs audio through it ----
option(CTS_BUILD_TESTS "Build the test programs (VST3 / LV2 test host, preset and behaviour tests)" ON)
if(CTS_BUILD_TESTS)
    juce_add_console_app(PluginHostTest PRODUCT_NAME "PluginHostTest")
    target_sources(PluginHostTest PRIVATE Tests/HostTest.cpp)
    target_compile_definitions(PluginHostTest PRIVATE
        JUCE_PLUGINHOST_VST3=1
        $<$<PLATFORM_ID:Linux>:JUCE_PLUGINHOST_LV2=1>
        $<$<PLATFORM_ID:Linux>:JUCE_JACK=1>   # so --list-devices shows the same device types as the app
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(PluginHostTest
        PRIVATE
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_gui_basics
            juce::juce_dsp
            juce::juce_audio_devices
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    # direct test of the preset system (compiles the plugin sources into a console app)
    juce_add_console_app(PresetTest PRODUCT_NAME "PresetTest")
    target_sources(PresetTest PRIVATE Tests/PresetTest.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(PresetTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(PresetTest
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # sample-accurate behaviour tests: clicks, state reload, gate extremes, buffer-size independence
    juce_add_console_app(BehaviourTest PRODUCT_NAME "BehaviourTest")
    target_sources(BehaviourTest PRIVATE Tests/BehaviourTest.cpp Tests/LabMeasurements.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(BehaviourTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    # the module tests keep a few dozen complete plugin instances (about 40 KB each) on the stack: give it 8 MB
    # (Windows defaults to 1 MB; Linux already gives the main thread 8 MB)
    if(MSVC)
        target_link_options(BehaviourTest PRIVATE "/STACK:8388608")
    endif()
    target_link_libraries(BehaviourTest
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    if(WIN32 AND EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
        target_compile_definitions(PluginHostTest PRIVATE JUCE_ASIO=1)
        target_include_directories(PluginHostTest PRIVATE "${ASIO_SDK_DIR}/common")
    endif()
endif()
