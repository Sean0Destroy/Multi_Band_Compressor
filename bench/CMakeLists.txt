# CPU benchmark executable. Not a test: it is never registered with ctest.
add_executable(tl_bench Benchmark.cpp)
target_link_libraries(tl_bench PRIVATE ToneLegionMultiband)
if(TL_PROJECT_WARNING_FLAGS)
    set_source_files_properties(Benchmark.cpp PROPERTIES COMPILE_OPTIONS "${TL_PROJECT_WARNING_FLAGS}")
endif()
