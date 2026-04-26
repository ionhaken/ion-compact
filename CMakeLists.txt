cmake_minimum_required(VERSION 3.16)

project(ion-compact LANGUAGES C CXX)

option(ION_COMPACT_BUILD_TESTS "Build the standalone test runner" ON)

# ---------------------------------------------------------------------------
# zstd (vendored under depend/zstd)
# ---------------------------------------------------------------------------
add_subdirectory(depend)

# ---------------------------------------------------------------------------
# ion-compact
# ---------------------------------------------------------------------------
file(GLOB_RECURSE COMPACT_SRCS src/ion/compact/*.cpp)
file(GLOB_RECURSE COMPACT_HDRS src/ion/*.h)

add_library(ion-compact STATIC ${COMPACT_SRCS} ${COMPACT_HDRS})
target_include_directories(ion-compact PUBLIC src depend)
target_link_libraries(ion-compact PUBLIC zstd)
target_compile_features(ion-compact PUBLIC cxx_std_20)
target_compile_definitions(ion-compact PRIVATE
    UNICODE _UNICODE
    ZSTD_LEGACY_SUPPORT=0
    XXH_NAMESPACE=ZSTD_)

if(MSVC)
    target_compile_options(ion-compact PRIVATE /W3)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src/ion FILES ${COMPACT_SRCS} ${COMPACT_HDRS})

# ---------------------------------------------------------------------------
# Standalone test runner (optional). No external test framework dependency.
# ---------------------------------------------------------------------------
if(ION_COMPACT_BUILD_TESTS)
    add_executable(ion-compact-tests test/tests.cpp test/test_helpers.h)
    target_link_libraries(ion-compact-tests PRIVATE ion-compact)
    target_compile_features(ion-compact-tests PRIVATE cxx_std_20)
endif()
