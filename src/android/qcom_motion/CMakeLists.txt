# Optional QCOM (Adreno) motion-estimator backend with Android AHB compact-luma input.
#
# This module is OFF by default and must stay optional: it depends on the
# Qualcomm-only VK_QCOM_image_processing extension, and every Android build that
# enables it also needs Vulkan, EGL and AHardwareBuffer headers.
#
# Ownership: nothing here is part of MeguvizMedia::core or ::android. The module
# is an independent shared object beside the RIFE backend so that hosts which
# never load it carry no QCOM code, no extra Vulkan instance and no extra .so.

cmake_minimum_required(VERSION 3.31)
project(MeguvizQcomMotion LANGUAGES C CXX)
include(GNUInstallDirs)

# Default OFF: no repository build acquires a Qualcomm-only dependency, an extra
# Vulkan device, or an extra .so unless a caller opts in explicitly.
option(MGM_WITH_QCOM_MOTION "Build the optional QCOM motion estimator and its JNI bridge" OFF)
option(MGM_QCOM_MOTION_PREBUILT_SPV
       "Use the checked-in SPIR-V headers instead of compiling the GLSL at build time" ON)

set(MGM_QCOM_MOTION_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT MGM_WITH_QCOM_MOTION)
  # Standalone configure-only path: keep the option visible without requiring a
  # toolchain, so repository CI can prove the OFF default costs nothing.
  message(STATUS "MeguvizQcomMotion: disabled (MGM_WITH_QCOM_MOTION=OFF)")
  return()
endif()

if(NOT ANDROID)
  message(FATAL_ERROR "MGM_WITH_QCOM_MOTION requires the Android NDK toolchain")
endif()

# The estimator and projection kernels are portable C++17; only the JNI bridge
# and the AHB import path are Android-specific.
set(MGM_QCOM_MOTION_CORE
  "${MGM_QCOM_MOTION_DIR}/qcom_motion.cpp"
  "${MGM_QCOM_MOTION_DIR}/qcom_candidate_projection.cpp")

add_library(meguviz_qcom_motion_core STATIC ${MGM_QCOM_MOTION_CORE})
# Deliberately default visibility: the frozen prototype compiled without
# -fvisibility=hidden and exported both the JNI class and the qcom_motion_* C
# ABI. Export control comes from the linker version script below, which can only
# restrict -- it cannot promote a symbol the compiler marked hidden.
set_target_properties(meguviz_qcom_motion_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON)
target_compile_features(meguviz_qcom_motion_core PUBLIC cxx_std_17)
target_compile_definitions(meguviz_qcom_motion_core PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
target_compile_options(meguviz_qcom_motion_core PRIVATE -fexceptions -Wall -Wextra
  -Wno-missing-field-initializers -fno-fast-math -ffp-contract=off)
target_include_directories(meguviz_qcom_motion_core PUBLIC
  "$<BUILD_INTERFACE:${MGM_QCOM_MOTION_DIR}>")
# NDK ships libvulkan.so as a platform stub; no find_package(Vulkan) needed.
target_link_libraries(meguviz_qcom_motion_core PUBLIC vulkan)

# SPIR-V: the checked-in headers are byte-identical to what glslang produced for
# the frozen prototype (unpack.spv a0c8c035..., motion.spv as pinned in
# coordinate/verification evidence). Recompiling requires glslangValidator on
# PATH and is only a cross-check, never the shipped artifact.
if(NOT MGM_QCOM_MOTION_PREBUILT_SPV)
  find_program(MGM_GLSLANG_VALIDATOR NAMES glslangValidator glslang)
  if(NOT MGM_GLSLANG_VALIDATOR)
    message(FATAL_ERROR "MGM_QCOM_MOTION_PREBUILT_SPV=OFF needs glslangValidator on PATH")
  endif()
  foreach(shader IN ITEMS unpack)
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    add_custom_command(OUTPUT "${spv}"
      COMMAND "${MGM_GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
              -o "${spv}" "${MGM_QCOM_MOTION_DIR}/${shader}.comp"
      DEPENDS "${MGM_QCOM_MOTION_DIR}/${shader}.comp"
      COMMENT "Compiling ${shader}.comp")
    add_custom_target("meguviz_qcom_motion_${shader}_spv" DEPENDS "${spv}")
    add_dependencies(meguviz_qcom_motion_core "meguviz_qcom_motion_${shader}_spv")
  endforeach()
endif()

# Single JNI object mirroring the verified prototype: estimator + Android
# projection + JNI entry points + AHB bridge in one .so.
add_library(meguviz_qcom_motion SHARED
  "${MGM_QCOM_MOTION_DIR}/qcom_motion_jni.cpp"
  "${MGM_QCOM_MOTION_DIR}/ahb_jni.cpp")
set_target_properties(meguviz_qcom_motion PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/jniLibs/${ANDROID_ABI}")
target_compile_features(meguviz_qcom_motion PRIVATE cxx_std_17)
target_compile_definitions(meguviz_qcom_motion PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_compile_options(meguviz_qcom_motion PRIVATE -fexceptions -Wall -Wextra
  -Wno-missing-field-initializers -fno-fast-math -ffp-contract=off)
target_include_directories(meguviz_qcom_motion PRIVATE "${MGM_QCOM_MOTION_DIR}")
# Link order is load-bearing: DT_NEEDED follows the command line, and the
# verified prototype emitted libEGL, libGLESv3, libvulkan, libandroid.
target_link_libraries(meguviz_qcom_motion PRIVATE
  meguviz_qcom_motion_core EGL GLESv3 vulkan android ${CMAKE_DL_LIBS})
# 16 KiB page alignment is mandatory for API 35+ devices.
target_link_options(meguviz_qcom_motion PRIVATE "LINKER:-z,max-page-size=16384")

# Export only the fixed JNI class plus the C ABI used by the harness.
set(MGM_QCOM_EXPORTS "{\n global:\n  qcom_motion_*;\n  qcom_prepare_candidates;\n  Java_io_github_jichuo1_meguviz_media_QcomMotionNative_*;\n local: *;\n};\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qcom_motion.exports.map" "${MGM_QCOM_EXPORTS}")
target_link_options(meguviz_qcom_motion PRIVATE
  "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/qcom_motion.exports.map")

install(TARGETS meguviz_qcom_motion_core ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS meguviz_qcom_motion LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES qcom_motion.h ahb_motion_api.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/meguviz/qcom_motion)
