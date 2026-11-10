# CMake helpers for plugins built on the kit.

# lkit_seam_tokens(<target> [SEAM_DESIGN_DIR <dir>])
# Generates Tokens.h from the read-only Seam design system (tokens.json) into the build tree at build
# time and adds it to <target>'s include path. No design file is ever copied into a repo.
function(lkit_seam_tokens target)
  cmake_parse_arguments(A "" "SEAM_DESIGN_DIR" "" ${ARGN})
  if(NOT A_SEAM_DESIGN_DIR)
    if(DEFINED SEAM_DESIGN_DIR)
      set(A_SEAM_DESIGN_DIR "${SEAM_DESIGN_DIR}")
    else()
      set(A_SEAM_DESIGN_DIR "$ENV{HOME}/Apps/Seam/design")
    endif()
  endif()
  if(NOT EXISTS "${A_SEAM_DESIGN_DIR}/tokens.json")
    message(FATAL_ERROR "Seam design system not found: ${A_SEAM_DESIGN_DIR}/tokens.json (set SEAM_DESIGN_DIR)")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(gen "${CMAKE_CURRENT_BINARY_DIR}/${target}_seam")
  add_custom_command(
    OUTPUT "${gen}/Tokens.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen}"
    COMMAND ${Python3_EXECUTABLE} "${LKIT_DIR}/juce/gen_tokens.py" "${A_SEAM_DESIGN_DIR}/tokens.json" "${gen}/Tokens.h"
    DEPENDS "${A_SEAM_DESIGN_DIR}/tokens.json" "${LKIT_DIR}/juce/gen_tokens.py"
    COMMENT "Generating Tokens.h for ${target} from the Seam design system")
  add_custom_target(${target}_seam_tokens DEPENDS "${gen}/Tokens.h")
  add_dependencies(${target} ${target}_seam_tokens)
  target_include_directories(${target} PRIVATE "${gen}")
endfunction()
