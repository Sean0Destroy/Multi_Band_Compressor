cmake_minimum_required(VERSION 3.22)

project(ToneLegionMultiband VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TL_BUILD_CLAP  "Build the CLAP format via clap-juce-extensions (compatibility gate vs JUCE 9)" ON)
option(TL_BUILD_TESTS "Build the Catch2 test suite" ON)
option(TL_BUILD_BENCH "Build the CPU benchmark executable tl_bench (not run by ctest)" ON)
option(TL_ENABLE_RTSAN "Build with RealtimeSanitizer (requires clang >= 20)" OFF)
option(TL_ENABLE_ASAN_UBSAN "Build everything (tests included) with -fsanitize=address,undefined" OFF)

# ---------------------------------------------------------------------------
# RealtimeSanitizer: only honest support. Errors if the toolchain cannot do it.
# ---------------------------------------------------------------------------
if(TL_ENABLE_RTSAN)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "TL_ENABLE_RTSAN requires Clang >= 20 (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}).")
    endif()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
        message(FATAL_ERROR "TL_ENABLE_RTSAN requires Clang >= 20 for -fsanitize=realtime (found ${CMAKE_CXX_COMPILER_VERSION}).")
    endif()
    add_compile_options(-fsanitize=realtime)
    add_link_options(-fsanitize=realtime)
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer + UndefinedBehaviorSanitizer (GCC / Clang). Applied to every target so that
# JUCE, Catch2 and the project share one instrumented runtime. The test binary's malloc
# interposition is compiled out automatically under ASan (see tests/support/AllocationCounter.h).
# ---------------------------------------------------------------------------
if(TL_ENABLE_ASAN_UBSAN)
    if(TL_ENABLE_RTSAN)
        message(FATAL_ERROR "TL_ENABLE_ASAN_UBSAN and TL_ENABLE_RTSAN cannot be combined.")
    endif()
    if(MSVC OR NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        message(FATAL_ERROR "TL_ENABLE_ASAN_UBSAN requires GCC or Clang with a GNU-style driver (found ${CMAKE_CXX_COMPILER_ID}).")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Dependencies (git submodules, pinned)
# ---------------------------------------------------------------------------
add_subdirectory(libs/JUCE)

if(TL_BUILD_CLAP)
    add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# Plugin identity. PLACEHOLDER codes: pending uniqueness / registration check.
# Must stay in sync with source/PluginIdentity.h (verified by the identity test).
# JUCE rules: manufacturer code = 4 chars, >= 1 upper-case letter, first upper
# and the rest lower-case (GarageBand); plugin code = 4 chars, exactly one
# upper-case letter, first upper and the rest lower-case.
# ---------------------------------------------------------------------------
set(TL_MANUFACTURER_CODE "Tlgn")   # PLACEHOLDER
set(TL_PLUGIN_CODE       "Tlmb")   # PLACEHOLDER

set(TL_BUNDLE_ID "com.tonelegion.multiband")
set(TL_CLAP_ID   "com.tonelegion.multiband")

set(TL_FORMATS VST3 AU)

juce_add_plugin(ToneLegionMultiband
    COMPANY_NAME "Tone Legion"
    PRODUCT_NAME "Tone Legion Multiband"
    PLUGIN_NAME "Tone Legion Multiband"
    BUNDLE_ID ${TL_BUNDLE_ID}
    PLUGIN_MANUFACTURER_CODE ${TL_MANUFACTURER_CODE}
    PLUGIN_CODE ${TL_PLUGIN_CODE}
    FORMATS ${TL_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx Dynamics
    COPY_PLUGIN_AFTER_BUILD FALSE)

set(TL_RT_SOURCES
    source/rt/FiniteGuard.h
    source/rt/AnalyzerFifo.h
    source/rt/LevelMeter.h)

set(TL_ANALYSIS_SOURCES
    source/analysis/SpectrumAnalyzer.h
    source/analysis/SpectrumAnalyzer.cpp)

set(TL_DSP_SOURCES
    source/dsp/DspConstants.h
    source/dsp/TptSvf.h
    source/dsp/EdgeSmoother.h
    source/dsp/GainComputer.h
    source/dsp/GrSmoother.h
    source/dsp/Ramp.h
    source/dsp/KWeighting.h
    source/dsp/OutputStage.h
    source/dsp/Lookahead.h
    source/dsp/BandDetector.h
    source/dsp/GrMeter.h
    source/dsp/IBandEngine.h
    source/dsp/A3Model.h
    source/dsp/DynamicsCore.h
    source/dsp/DynamicsCore.cpp
    source/dsp/BandEngineBase.h
    source/dsp/BandEngineBase.cpp
    source/dsp/A3RegionEngine.h
    source/dsp/A3RegionEngine.cpp)

set(TL_UI_SOURCES
    source/ui/Theme.h
    source/ui/ValueFormat.h
    source/ui/ValueFormat.cpp
    source/ui/Controls.h
    source/ui/Controls.cpp
    source/ui/BandColumn.h
    source/ui/BandColumn.cpp
    source/ui/BandInspector.h
    source/ui/BandInspector.cpp
    source/ui/DynamicsView.h
    source/ui/DynamicsView.cpp
    source/ui/MeterRail.h
    source/ui/MeterRail.cpp
    source/ui/SpectrumView.h
    source/ui/SpectrumView.cpp
    source/ui/PluginEditor.h
    source/ui/PluginEditor.cpp)

set(TL_CORE_SOURCES
    source/PluginIdentity.h
    source/ParameterSchema.h
    source/ParameterSchema.cpp
    source/BandSlots.h
    source/BandSlots.cpp
    source/StateSerialization.h
    source/StateSerialization.cpp
    source/PresetManager.h
    source/PresetManager.cpp
    source/PluginProcessor.h
    source/PluginProcessor.cpp
    ${TL_RT_SOURCES}
    ${TL_ANALYSIS_SOURCES}
    ${TL_DSP_SOURCES}
    ${TL_UI_SOURCES})

target_sources(ToneLegionMultiband PRIVATE ${TL_CORE_SOURCES})

# Project sources: -Wall -Wextra explicitly (on top of juce_recommended_warning_flags, which also
# reach the JUCE module sources compiled into this target). Source-level options come last.
if(NOT MSVC AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(TL_PROJECT_WARNING_FLAGS -Wall -Wextra)
    set_source_files_properties(${TL_CORE_SOURCES} PROPERTIES COMPILE_OPTIONS "${TL_PROJECT_WARNING_FLAGS}")
endif()
target_include_directories(ToneLegionMultiband PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)

target_compile_definitions(ToneLegionMultiband PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    # Headers for these optional X11 extensions are not required for a plugin.
    JUCE_USE_XRANDR=0
    JUCE_USE_XINERAMA=0
    JUCE_USE_XCURSOR=0
    JUCE_USE_XINPUT=0)

# Only juce_audio_processors (APVTS, generic editor) + plugin client; no
# juce_audio_devices / juce_audio_utils, hence no ALSA requirement.
target_link_libraries(ToneLegionMultiband
    PUBLIC
        juce::juce_audio_processors
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# CLAP (compatibility gate; see README)
# ---------------------------------------------------------------------------
if(TL_BUILD_CLAP)
    clap_juce_extensions_plugin(TARGET ToneLegionMultiband
        CLAP_ID "${TL_CLAP_ID}"
        CLAP_FEATURES audio-effect compressor stereo)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(libs/Catch2)
    add_subdirectory(tests)
endif()

if(TL_BUILD_BENCH)
    add_subdirectory(bench)
endif()
