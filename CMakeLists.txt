cmake_minimum_required(VERSION 3.22)

project(FlexCurve VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE")
endif()

if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE was not found. Expected it at third_party/JUCE, or pass -DJUCE_DIR=/path/to/JUCE.")
endif()

add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/third_party/JUCE")

juce_add_plugin(FlexCurve
    COMPANY_NAME "Mixomo"
    COMPANY_WEBSITE "https://github.com/Mixomo"
    COMPANY_COPYRIGHT "Copyright 2026 Ezequiel Casas (Mixomo)"
    BUNDLE_ID "com.mixomo.flexcurve"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_AUTO_MANIFEST FALSE
    PLUGIN_MANUFACTURER_CODE Mxmo
    PLUGIN_CODE FxCv
    FORMATS VST3
    PRODUCT_NAME "FlexCurve")
juce_generate_juce_header(FlexCurve)

target_sources(FlexCurve
    PRIVATE
        Source/FlexCurveProcessor.cpp
        Source/FlexCurveEditor.cpp
        Source/CurveFIR.cpp)

target_compile_definitions(FlexCurve
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FlexCurve
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

juce_add_console_app(FlexCurveVST3SmokeTest
    PRODUCT_NAME "FlexCurveVST3SmokeTest")

target_sources(FlexCurveVST3SmokeTest
    PRIVATE
        tools/vst3_smoke_test.cpp
        Source/CurveFIR.cpp)

target_compile_definitions(FlexCurveVST3SmokeTest
    PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(FlexCurveVST3SmokeTest
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

juce_add_console_app(FlexCurveEngineTest
    PRODUCT_NAME "FlexCurveEngineTest")

target_sources(FlexCurveEngineTest
    PRIVATE
        tools/flexcurve_engine_test.cpp)

target_compile_definitions(FlexCurveEngineTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_include_directories(FlexCurveEngineTest
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/FlexCurve_artefacts/JuceLibraryCode")

target_link_libraries(FlexCurveEngineTest
    PRIVATE
        FlexCurve
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
