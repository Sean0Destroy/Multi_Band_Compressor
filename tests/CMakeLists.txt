list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/libs/Catch2/extras)
include(Catch)

add_executable(tl_tests
    support/AllocationCounter.cpp
    support/AllocationCounter.h
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
    BandSlotsTests.cpp)

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
target_compile_options(tl_tests PRIVATE -Wno-float-equal)
