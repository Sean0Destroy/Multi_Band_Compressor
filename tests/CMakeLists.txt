list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/libs/Catch2/extras)
include(Catch)

set(TL_TEST_SOURCES
    support/AllocationCounter.cpp
    support/AllocationCounter.h
    support/DspTestSupport.cpp
    support/DspTestSupport.h
    support/OfflineRenderer.cpp
    support/OfflineRenderer.h
    support/TestMain.cpp
    IdentityTests.cpp
    PassThroughTests.cpp
    SidechainTests.cpp
    ParameterIdTests.cpp
    StateTests.cpp
    SilenceFiniteTests.cpp
    AllocationTests.cpp
    BandSlotsTests.cpp
    DspUnitTests.cpp
    EngineResponseTests.cpp
    CompressorTests.cpp
    EngineIntegrationTests.cpp
    ValueFormatTests.cpp
    EditorTests.cpp
    EngineDumpTests.cpp
    GainStagingTests.cpp
    RoutingTests.cpp
    SpectrumTests.cpp
    MeterTests.cpp
    OutputStageTests.cpp
    LookaheadTests.cpp
    TimingTests.cpp
    PresetTests.cpp
    AuditionTests.cpp
    CharacterTests.cpp
    APrimeEngineTests.cpp
    TransitionTests.cpp
    MutationGapTests.cpp
    RepairMeasurements.cpp
    EditUndoTests.cpp
    RepairTests.cpp
    ClipperOversamplingTests.cpp
    ClipMeterTests.cpp
    OverlapModeTests.cpp
    BandCharacterTests.cpp)

add_executable(tl_tests ${TL_TEST_SOURCES})

target_include_directories(tl_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(tl_tests PRIVATE
    TL_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
    TL_CMAKE_BUNDLE_ID="${TL_BUNDLE_ID}"
    TL_CMAKE_CLAP_ID="${TL_CLAP_ID}"
    TL_CMAKE_MANUFACTURER_CODE="${TL_MANUFACTURER_CODE}"
    TL_CMAKE_PLUGIN_CODE="${TL_PLUGIN_CODE}")
target_link_libraries(tl_tests PRIVATE ToneLegionMultiband Catch2::Catch2)

# Tests need a JUCE MessageManager but no display/audio device.
catch_discover_tests(tl_tests DISCOVERY_MODE PRE_TEST)
# Tests compare floats exactly on purpose (bit-exactness checks). GCC/Clang only: MSVC rejects
# GNU-style flags (D8021). Applied per source because source options come after the
# target/interface options, so this wins over juce_recommended_warning_flags' -Wfloat-equal
# (a target-level -Wno-float-equal came first and was ineffective).
if(NOT MSVC AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set_source_files_properties(${TL_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS "${TL_PROJECT_WARNING_FLAGS};-Wno-float-equal")
endif()
