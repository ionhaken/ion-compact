cmake_minimum_required(VERSION 3.9.4)

project(zstd)

file(GLOB_RECURSE SRCS1 lib/common/*.c)
file(GLOB_RECURSE HDRS1 lib/common/*.h)
file(GLOB_RECURSE SRCS2 lib/compress/*.c)
file(GLOB_RECURSE HDRS2 lib/compress/*.h)
file(GLOB_RECURSE SRCS3 lib/decompress/*.c)
file(GLOB_RECURSE HDRS3 lib/decompress/*.h)

file(GLOB_RECURSE HDRS4 lib/*.h)

add_library(zstd STATIC ${SRCS1} ${HDRS1} ${SRCS2} ${HDRS2} ${SRCS3} ${HDRS3} ${HDRS4})

target_include_directories(zstd PUBLIC lib)

add_definitions(-DUNICODE -D_UNICODE)
add_definitions(-DZSTD_LEGACY_SUPPORT=0)
add_definitions(-DXXH_NAMESPACE=ZSTD_)

target_compile_features(zstd PRIVATE cxx_std_20)

set_target_properties(zstd PROPERTIES FOLDER "Lib")


