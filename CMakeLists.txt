# LOMAS Plugin-kit: shared DSP engine, measurement rig, wrapper base and Seam components.
# Standalone:  cmake -B build [-DLKIT_BUILD_JUCE=ON -DJUCE_DIR=...] && cmake --build build && ctest --test-dir build
# As a submodule: add JUCE first (add_subdirectory(JUCE)), then add_subdirectory(kit) with LKIT_BUILD_JUCE=ON.
cmake_minimum_required(VERSION 3.22)
project(LomasPluginKit VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(LKIT_TOP ON)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()
else()
  set(LKIT_TOP OFF)
endif()

option(LKIT_BUILD_TESTS "Build the kit's own tests" ${LKIT_TOP})
option(LKIT_BUILD_JUCE "Build the JUCE parts (wrapper base, Seam components, vst3render)" OFF)
set(LKIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")

# ---- core (no JUCE) ------------------------------------------------------------------------------
add_library(lkit_core STATIC core/src/Halfband.cpp)
target_include_directories(lkit_core PUBLIC core/include)
target_compile_features(lkit_core PUBLIC cxx_std_20)
target_compile_options(lkit_core PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(lkit_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(lkit::core ALIAS lkit_core)

# ---- measurement host (AU, macOS) ----------------------------------------------------------------
if(APPLE)
  add_executable(lkit_measure measure/lkit_measure.mm)
  set_source_files_properties(measure/lkit_measure.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_compile_features(lkit_measure PRIVATE cxx_std_20)
  target_compile_options(lkit_measure PRIVATE -Wall -Wextra)
  target_link_libraries(lkit_measure PRIVATE "-framework Foundation" "-framework AudioToolbox")
  set_target_properties(lkit_measure PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/measure")
endif()

# ---- JUCE parts ----------------------------------------------------------------------------------
include(cmake/Lkit.cmake)
if(LKIT_BUILD_JUCE)
  if(NOT TARGET juce::juce_audio_processors)
    if(NOT DEFINED JUCE_DIR)
      message(FATAL_ERROR "LKIT_BUILD_JUCE needs JUCE: add it before the kit or pass -DJUCE_DIR")
    endif()
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
  endif()
  # Interface libraries: the sources compile into each plugin (as JUCE modules do).
  add_library(lkit_juce INTERFACE)
  target_sources(lkit_juce INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/juce/src/ProcessorBase.cpp)
  target_include_directories(lkit_juce INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/juce/include)
  target_link_libraries(lkit_juce INTERFACE lkit_core)
  add_library(lkit::juce ALIAS lkit_juce)
  add_library(lkit_seam INTERFACE)   # needs lkit_seam_tokens(<target>) on the consumer
  target_sources(lkit_seam INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/juce/src/Seam.cpp)
  target_include_directories(lkit_seam INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/juce/include)
  add_library(lkit::seam ALIAS lkit_seam)

  juce_add_console_app(LkitVst3Render PRODUCT_NAME "lkit_vst3render")
  target_sources(LkitVst3Render PRIVATE tools/vst3render/vst3render.cpp)
  target_compile_definitions(LkitVst3Render PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(LkitVst3Render PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags "-framework CoreAudioKit")
endif()

# ---- tests ---------------------------------------------------------------------------------------
if(LKIT_BUILD_TESTS)
  enable_testing()
  add_executable(lkit_tests tests/cpp/test_kit.cpp)
  target_link_libraries(lkit_tests PRIVATE lkit_core)
  target_compile_options(lkit_tests PRIVATE -Wall -Wextra)
  add_test(NAME kit_core_tests COMMAND lkit_tests)
  if(LKIT_BUILD_JUCE)
    juce_add_console_app(LkitJuceTests PRODUCT_NAME "lkit_juce_tests")
    target_sources(LkitJuceTests PRIVATE tests/cpp/test_juce.cpp)
    target_compile_definitions(LkitJuceTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(LkitJuceTests PRIVATE lkit_juce lkit_seam juce::juce_audio_processors juce::juce_gui_basics
                                                juce::juce_recommended_config_flags)
    lkit_seam_tokens(LkitJuceTests)
    add_test(NAME kit_juce_tests COMMAND LkitJuceTests)
  endif()
  set(LKIT_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python")
  if(EXISTS "${LKIT_PYTHON}")
    add_test(NAME kit_python_tests
             COMMAND "${LKIT_PYTHON}" -m pytest -q -W ignore::RuntimeWarning "${CMAKE_CURRENT_SOURCE_DIR}/tests/python"
             WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()
