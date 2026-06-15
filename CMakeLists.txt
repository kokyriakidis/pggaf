cmake_minimum_required(VERSION 3.20)

# Honor INTERPROCEDURAL_OPTIMIZATION on all targets, including vendored
# subprojects that declare an older cmake_minimum_required. Avoids CMP0069
# warnings and lets LTO apply uniformly where the toolchain supports it.
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

project(pggaf VERSION 0.1.0 LANGUAGES CXX C)

# Default to an optimized build. The runtime is dominated by GBWT/gbwtgraph
# find/extend/locate and reference-path extraction, which are 3-10x slower when
# compiled at -O0. A plain `cmake ..` should not silently produce a debug build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING
      "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
      Debug Release RelWithDebInfo MinSizeRel)
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckIPOSupported)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
option(PGGAF_BUILD_TESTS "Build pggaf tests" ON)
option(PGGAF_ENABLE_LTO "Enable interprocedural optimization (LTO) for optimized builds" ON)

# Enable LTO for the whole project on optimized builds. This lets the compiler
# inline across the pggaf core and the static graph libraries it links against.
if(PGGAF_ENABLE_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  check_ipo_supported(RESULT _pggaf_ipo_supported OUTPUT _pggaf_ipo_message)
  if(_pggaf_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    message(STATUS "pggaf: interprocedural optimization (LTO) enabled")
  else()
    message(STATUS "pggaf: LTO not supported by toolchain: ${_pggaf_ipo_message}")
  endif()
endif()

set(WITH_GOOGLETEST OFF CACHE BOOL "Disable vendored googletest in sdsl-lite" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable external project tests" FORCE)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenMP)

set(PGGAF_OPENMP_INCLUDE_DIRS)
set(PGGAF_OPENMP_COMPILE_OPTIONS)
if(NOT OpenMP_CXX_FOUND AND APPLE)
  set(PGGAF_LIBOMP_PREFIX "/opt/homebrew/opt/libomp")
  if(EXISTS "${PGGAF_LIBOMP_PREFIX}/include/omp.h" AND EXISTS "${PGGAF_LIBOMP_PREFIX}/lib/libomp.dylib")
    add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
    set_target_properties(OpenMP::OpenMP_CXX PROPERTIES
      INTERFACE_COMPILE_OPTIONS "-Xpreprocessor;-fopenmp"
      INTERFACE_INCLUDE_DIRECTORIES "${PGGAF_LIBOMP_PREFIX}/include"
      INTERFACE_LINK_LIBRARIES "${PGGAF_LIBOMP_PREFIX}/lib/libomp.dylib"
    )
    set(OpenMP_CXX_FOUND TRUE)
    list(APPEND PGGAF_OPENMP_INCLUDE_DIRS "${PGGAF_LIBOMP_PREFIX}/include")
    list(APPEND PGGAF_OPENMP_COMPILE_OPTIONS "-Xpreprocessor" "-fopenmp")
  endif()
endif()

pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

find_package(ZLIB REQUIRED)
include(ExternalProject)

# htslib: built from source for bgzf/tabix in index-gaf; no external install required
set(_htslib_src    "${CMAKE_CURRENT_SOURCE_DIR}/external/htslib")
set(_htslib_prefix "${CMAKE_BINARY_DIR}/htslib-prefix")
ExternalProject_Add(htslib_ext
  SOURCE_DIR    "${_htslib_src}"
  PREFIX        "${_htslib_prefix}"
  BUILD_IN_SOURCE TRUE
  CONFIGURE_COMMAND
    /bin/sh -c
    "cd '${_htslib_src}' && autoreconf -i && ./configure --prefix='${_htslib_prefix}' --disable-bz2 --disable-lzma --disable-libcurl --disable-s3 --disable-gcs CFLAGS='-O2 -fPIC'"
  BUILD_COMMAND   $(MAKE)
  INSTALL_COMMAND $(MAKE) install
  BUILD_BYPRODUCTS "${_htslib_prefix}/lib/libhts.a"
)
file(MAKE_DIRECTORY "${_htslib_prefix}/include")
add_library(hts_bundled STATIC IMPORTED GLOBAL)
add_dependencies(hts_bundled htslib_ext)
set_target_properties(hts_bundled PROPERTIES
  IMPORTED_LOCATION             "${_htslib_prefix}/lib/libhts.a"
  INTERFACE_INCLUDE_DIRECTORIES "${_htslib_prefix}/include"
  INTERFACE_LINK_LIBRARIES      "ZLIB::ZLIB"
)

# patchelf is only needed on Linux at install time to produce a self-contained
# package.  If it is not already on PATH we build it from source so the user
# never has to install it manually.
if(UNIX AND NOT APPLE)
  find_program(PATCHELF_EXECUTABLE patchelf)
  if(NOT PATCHELF_EXECUTABLE)
    message(STATUS "patchelf not found in PATH — will build from source")
    include(ExternalProject)
    set(_patchelf_prefix "${CMAKE_BINARY_DIR}/patchelf-host")
    ExternalProject_Add(patchelf_host
      URL      https://github.com/NixOS/patchelf/releases/download/0.18.0/patchelf-0.18.0.tar.bz2
      CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
      BUILD_COMMAND     make
      INSTALL_COMMAND   make install
      PREFIX            "${_patchelf_prefix}"
    )
    set(PATCHELF_EXECUTABLE "${_patchelf_prefix}/bin/patchelf")
  endif()
endif()

add_subdirectory(external/sdsl-lite EXCLUDE_FROM_ALL)
add_subdirectory(external/libhandlegraph EXCLUDE_FROM_ALL)

set(PGGAF_PARALLEL_LIBS Threads::Threads)
if(OpenMP_CXX_FOUND)
  list(APPEND PGGAF_PARALLEL_LIBS OpenMP::OpenMP_CXX)
endif()

add_library(gbwt STATIC
  external/gbwt/src/algorithms.cpp
  external/gbwt/src/bwtmerge.cpp
  external/gbwt/src/cached_gbwt.cpp
  external/gbwt/src/dynamic_gbwt.cpp
  external/gbwt/src/fast_locate.cpp
  external/gbwt/src/files.cpp
  external/gbwt/src/gbwt.cpp
  external/gbwt/src/internal.cpp
  external/gbwt/src/metadata.cpp
  external/gbwt/src/support.cpp
  external/gbwt/src/test.cpp
  external/gbwt/src/utils.cpp
  external/gbwt/src/variants.cpp
)
target_include_directories(gbwt
  PUBLIC
    external/gbwt/include
    external/sdsl-lite/include
    ${PGGAF_OPENMP_INCLUDE_DIRS}
)
target_compile_options(gbwt PRIVATE ${PGGAF_OPENMP_COMPILE_OPTIONS})
target_link_libraries(gbwt
  PUBLIC
    sdsl
    PkgConfig::ZSTD
    ${PGGAF_PARALLEL_LIBS}
)

add_library(gbwtgraph STATIC
  external/gbwtgraph/src/algorithms.cpp
  external/gbwtgraph/src/cached_gbwtgraph.cpp
  external/gbwtgraph/src/gbwtgraph.cpp
  external/gbwtgraph/src/gbz.cpp
  external/gbwtgraph/src/gfa.cpp
  external/gbwtgraph/src/index.cpp
  external/gbwtgraph/src/internal.cpp
  external/gbwtgraph/src/minimizer.cpp
  external/gbwtgraph/src/naive_graph.cpp
  external/gbwtgraph/src/path_cover.cpp
  external/gbwtgraph/src/subgraph.cpp
  external/gbwtgraph/src/utils.cpp
)
target_include_directories(gbwtgraph
  PUBLIC
    external/gbwtgraph/include
    external/gbwt/include
    external/sdsl-lite/include
    external/libhandlegraph/src/include
    ${PGGAF_OPENMP_INCLUDE_DIRS}
)
target_compile_options(gbwtgraph PRIVATE ${PGGAF_OPENMP_COMPILE_OPTIONS})
target_link_libraries(gbwtgraph
  PUBLIC
    gbwt
    handlegraph_static
    OpenSSL::Crypto
    PkgConfig::ZSTD
    ${PGGAF_PARALLEL_LIBS}
)

set(PGGAF_CORE_SOURCES
  src/annotate.cpp
  src/cli.cpp
  src/decode.cpp
  src/fingerprint.cpp
  src/gaf.cpp
  src/gbwt_backend.cpp
  src/sidecar.cpp
  src/index_gaf.cpp
)

add_library(pggaf_core STATIC ${PGGAF_CORE_SOURCES})
target_include_directories(pggaf_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(pggaf_core
  PUBLIC
    gbwtgraph
    hts_bundled
    OpenSSL::Crypto
    Threads::Threads
)

add_library(pggaf_shared SHARED ${PGGAF_CORE_SOURCES})
set_target_properties(pggaf_shared PROPERTIES
  OUTPUT_NAME pggaf
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME pggaf
)
target_include_directories(pggaf_shared PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(pggaf_shared
  PRIVATE
    gbwtgraph
    hts_bundled
    OpenSSL::Crypto
    Threads::Threads
)
if(APPLE)
  add_custom_command(TARGET pggaf_shared POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
      "$<TARGET_FILE_NAME:pggaf_shared>"
      "$<TARGET_FILE_DIR:pggaf_shared>/libpggaf.so"
    VERBATIM
  )
endif()

add_executable(pggaf src/main.cpp)
target_link_libraries(pggaf PRIVATE pggaf_shared)
if(TARGET patchelf_host)
  add_dependencies(pggaf patchelf_host)
endif()
if(APPLE)
  set_target_properties(pggaf PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  set_target_properties(pggaf_shared PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(pggaf PROPERTIES INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  set_target_properties(pggaf_shared PROPERTIES INSTALL_RPATH "\$ORIGIN")
endif()

if(PROJECT_IS_TOP_LEVEL AND PGGAF_BUILD_TESTS)
  enable_testing()
  add_executable(pggaf_tests
    tests/test_main.cpp
  )
  target_link_libraries(pggaf_tests PRIVATE pggaf_core)
  add_test(NAME pggaf_tests COMMAND pggaf_tests)
endif()

install(TARGETS pggaf_shared pggaf
  EXPORT pggafTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(APPLE)
  install(CODE "
    cmake_policy(SET CMP0011 NEW)
    cmake_policy(SET CMP0057 NEW)

    set(_lib_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\")
    set(_pggaf_lib \"\${_lib_dir}/libpggaf.dylib\")

    if(NOT EXISTS \"\${_pggaf_lib}\")
      message(FATAL_ERROR \"Bundle fixup: libpggaf.dylib not found at \${_pggaf_lib}\")
    endif()

    file(GET_RUNTIME_DEPENDENCIES
      LIBRARIES \"$<TARGET_FILE:pggaf_shared>\"
      RESOLVED_DEPENDENCIES_VAR   _resolved
      UNRESOLVED_DEPENDENCIES_VAR _unresolved
      PRE_EXCLUDE_REGEXES  \"^/usr/lib\" \"^/System/Library\"
      POST_EXCLUDE_REGEXES \"^/usr/lib\" \"^/System/Library\"
    )

    if(_unresolved)
      message(WARNING \"Bundle fixup: unresolved deps: \${_unresolved}\")
    endif()

    foreach(_dep_path IN LISTS _resolved)
      get_filename_component(_dep_name \"\${_dep_path}\" NAME)
      set(_dest \"\${_lib_dir}/\${_dep_name}\")
      if(NOT EXISTS \"\${_dest}\")
        message(STATUS \"Bundling: \${_dep_name}\")
        file(REAL_PATH \"\${_dep_path}\" _real_path)
        execute_process(
          COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different \"\${_real_path}\" \"\${_dest}\"
        )
        file(CHMOD \"\${_dest}\" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
      endif()
      execute_process(
        COMMAND install_name_tool -id \"@rpath/\${_dep_name}\" \"\${_dest}\"
        ERROR_QUIET
      )
    endforeach()

    set(_all_libs \"\${_pggaf_lib}\")
    foreach(_dep_path IN LISTS _resolved)
      get_filename_component(_dep_name \"\${_dep_path}\" NAME)
      list(APPEND _all_libs \"\${_lib_dir}/\${_dep_name}\")
    endforeach()

    foreach(_lib IN LISTS _all_libs)
      foreach(_dep_path IN LISTS _resolved)
        get_filename_component(_dep_name \"\${_dep_path}\" NAME)
        execute_process(
          COMMAND install_name_tool
            -change \"\${_dep_path}\" \"@loader_path/\${_dep_name}\"
            \"\${_lib}\"
          ERROR_QUIET
        )
      endforeach()
    endforeach()

    execute_process(
      COMMAND install_name_tool -add_rpath \"@loader_path\" \"\${_pggaf_lib}\"
      ERROR_QUIET
    )

    message(STATUS \"Runtime library bundling complete\")
  ")
elseif(UNIX)
  install(CODE "
    cmake_policy(SET CMP0011 NEW)
    cmake_policy(SET CMP0057 NEW)

    set(_patchelf \"${PATCHELF_EXECUTABLE}\")
    if(NOT EXISTS \"\${_patchelf}\")
      message(WARNING \"patchelf not available — skipping runtime library bundling.\")
      return()
    endif()

    set(_lib_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\")

    file(GLOB _pggaf_libs \"\${_lib_dir}/libpggaf.so*\")
    set(_pggaf_lib \"\")
    foreach(_f IN LISTS _pggaf_libs)
      if(NOT IS_SYMLINK \"\${_f}\")
        set(_pggaf_lib \"\${_f}\")
        break()
      endif()
    endforeach()
    if(NOT _pggaf_lib)
      message(FATAL_ERROR \"Bundle fixup: no libpggaf.so found in \${_lib_dir}\")
    endif()

    file(GET_RUNTIME_DEPENDENCIES
      LIBRARIES \"$<TARGET_FILE:pggaf_shared>\"
      RESOLVED_DEPENDENCIES_VAR   _resolved
      UNRESOLVED_DEPENDENCIES_VAR _unresolved
      PRE_EXCLUDE_REGEXES
        \"^/lib\" \"^/lib64\"
        \"^/usr/lib\" \"^/usr/lib64\"
        \"linux-vdso\" \"ld-linux\"
      POST_EXCLUDE_REGEXES
        \"^/lib\" \"^/lib64\"
        \"^/usr/lib\" \"^/usr/lib64\"
        \"linux-vdso\" \"ld-linux\"
    )

    if(_unresolved)
      message(WARNING \"Bundle fixup: unresolved deps: \${_unresolved}\")
    endif()

    foreach(_dep_path IN LISTS _resolved)
      get_filename_component(_dep_name \"\${_dep_path}\" NAME)
      set(_dest \"\${_lib_dir}/\${_dep_name}\")
      if(NOT EXISTS \"\${_dest}\")
        message(STATUS \"Bundling: \${_dep_name}\")
        file(REAL_PATH \"\${_dep_path}\" _real_path)
        execute_process(
          COMMAND \"${CMAKE_COMMAND}\" -E copy_if_different \"\${_real_path}\" \"\${_dest}\"
        )
        file(CHMOD \"\${_dest}\"
          PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                      GROUP_READ GROUP_EXECUTE
                      WORLD_READ WORLD_EXECUTE
        )
      endif()
      execute_process(
        COMMAND \"\${_patchelf}\" --set-rpath \"\$ORIGIN\" \"\${_dest}\"
        ERROR_QUIET
      )
    endforeach()

    execute_process(
      COMMAND \"\${_patchelf}\" --set-rpath \"\$ORIGIN\" \"\${_pggaf_lib}\"
      ERROR_QUIET
    )

    message(STATUS \"Runtime library bundling complete\")
  ")
endif()

install(DIRECTORY include/pggaf
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/pggafConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pggafConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/pggafConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pggaf
)

install(EXPORT pggafTargets
  FILE pggafTargets.cmake
  NAMESPACE pggaf::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pggaf
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/pggafConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/pggafConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pggaf
)

if(APPLE)
  install(CODE "
    execute_process(
      COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
              \"libpggaf.dylib\"
              \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libpggaf.so\"
    )
  ")
endif()
