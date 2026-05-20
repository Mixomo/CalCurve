cmake_minimum_required(VERSION 3.22)

project(CalCurve VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED JUCE_DIR)
    message(FATAL_ERROR "Set JUCE_DIR to a local JUCE checkout, for example -DJUCE_DIR=C:/JUCE")
endif()

add_subdirectory("${JUCE_DIR}" JUCE)

juce_add_plugin(CalCurve
    COMPANY_NAME "Mixomo"
    COMPANY_WEBSITE "https://github.com/Mixomo"
    COMPANY_COPYRIGHT "Copyright 2026 Ezequiel Casas (Mixomo)"
    BUNDLE_ID "com.mixomo.calcurve"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_AUTO_MANIFEST FALSE
    PLUGIN_MANUFACTURER_CODE Mxmo
    PLUGIN_CODE ClCv
    FORMATS VST3
    PRODUCT_NAME "CalCurve")
juce_generate_juce_header(CalCurve)

target_sources(CalCurve
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/CurveFIR.cpp)

target_compile_definitions(CalCurve
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(CalCurve
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

juce_add_console_app(CalCurveVST3SmokeTest
    PRODUCT_NAME "CalCurveVST3SmokeTest")

target_sources(CalCurveVST3SmokeTest
    PRIVATE
        tools/vst3_smoke_test.cpp
        Source/CurveFIR.cpp)

target_compile_definitions(CalCurveVST3SmokeTest
    PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(CalCurveVST3SmokeTest
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
