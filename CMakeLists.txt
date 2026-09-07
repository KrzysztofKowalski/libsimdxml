# libsimdxml — SIMD-accelerated XML parser with XPath 1.0 for C++23.
#
# Pure C++23 (no C++20 modules), no external dependencies. Builds a static
# library target `simdxml` (artifact: libsimdxml.a / simdxml.lib). Consumers:
#
#   add_subdirectory(libsimdxml)
#   target_link_libraries(my_target PRIVATE simdxml)
#
# and include headers as
#
#   #include <simdxml/simdxml.hpp>
#
# SIMD backends (AVX2, SSE4.2, NEON, scalar) are compiled unconditionally on
# their architectures and selected at RUN TIME via [[gnu::target(...)]]
# function attributes — no -mavx2 build flags, no fat binaries.
#
# CXX_SCAN_FOR_MODULES OFF — this is pure C++23 (no `export module`), so the
# CMake C++20 modules scanner must not scan these TUs. Otherwise CMake emits
# "CXX_SCAN_FOR_MODULES: scanning not available for this target" on compilers
# without module support.

cmake_minimum_required(VERSION 3.20)

project(libsimdxml
    VERSION 0.1.0
    DESCRIPTION "SIMD-accelerated XML parser with full XPath 1.0"
    LANGUAGES CXX
)

add_library(simdxml STATIC)
add_library(libsimdxml::simdxml ALIAS simdxml)

# GLOB_RECURSE picks up all .cpp under simdxml/ (batch/, index/, parallel/,
# persist/, simd/, xpath/). CONFIGURE_DEPENDS so new source files trigger a
# re-configure. Test/bench/example files are excluded defensively.
file(GLOB_RECURSE SIMDXML_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/simdxml/*.cpp"
)
list(FILTER SIMDXML_SOURCES EXCLUDE REGEX "(test|bench|example|fuzz)[._-]")
list(FILTER SIMDXML_SOURCES EXCLUDE REGEX "/(tests|benchmarks|examples|fuzz)/")
target_sources(simdxml PRIVATE ${SIMDXML_SOURCES})

# Public include roots:
#   - repo root          -> consumers: #include <simdxml/simdxml.hpp>
#   - repo root/simdxml  -> internal cross-directory includes
#                          ("simd/scalar.hpp" from simd/dispatch.hpp)
target_include_directories(simdxml PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/simdxml>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/simdxml>
)

# Pure C++23, no module scanning.
set_target_properties(simdxml PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# C++23 required — std::expected, std::span, std::ranges, std::format.
target_compile_features(simdxml PUBLIC cxx_std_23)

# Warnings: the codebase uses signed/unsigned comparisons and size_t casts
# that are benign but noisy on -Wsign-conversion / -Wconversion. Relax only
# those for this target; consumers don't inherit relaxed flags.
if(MSVC)
    target_compile_options(simdxml PRIVATE
        /W3
        /wd4244  # conversion from 'X' to 'Y', possible loss of data
        /wd4267  # conversion from 'size_t' to 'X', possible loss of data
        /wd4018  # signed/unsigned mismatch in comparison
    )
else()
    target_compile_options(simdxml PRIVATE
        -Wall -Wextra -Wno-sign-compare -Wno-conversion -Wno-sign-conversion
    )
endif()

include(GNUInstallDirs)
install(TARGETS simdxml
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY simdxml/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/simdxml
    FILES_MATCHING
    PATTERN "*.h*"
    PATTERN "*.cpp" EXCLUDE
)