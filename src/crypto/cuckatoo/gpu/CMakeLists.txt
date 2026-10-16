# Copyright (c) 2026 The Quicksilver developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Not core_interface: WERROR (/WX) hangs off it (F-268). These two definitions
# are the part of that interface <windows.h> still needs, or min/max become
# macros and std::max in the shared headers breaks (F-491).
function(qs_gpu_windows_definitions target)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
endfunction()

add_library(qsgpu_opencl_loader STATIC EXCLUDE_FROM_ALL opencl_loader.cpp)
target_include_directories(qsgpu_opencl_loader PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(qsgpu_opencl_loader PRIVATE warn_interface sanitize_interface ${CMAKE_DL_LIBS})
qs_gpu_windows_definitions(qsgpu_opencl_loader)

add_library(qsgpu_opencl_devices STATIC EXCLUDE_FROM_ALL opencl_devices.cpp)
target_include_directories(qsgpu_opencl_devices PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(qsgpu_opencl_devices PRIVATE qsgpu_opencl_loader warn_interface sanitize_interface)
qs_gpu_windows_definitions(qsgpu_opencl_devices)

if(QS_BUILD_GPU_HELPER)
  set(lean_cl_header ${CMAKE_CURRENT_BINARY_DIR}/lean_cl.h)
  add_custom_command(OUTPUT ${lean_cl_header}
    COMMAND ${CMAKE_COMMAND}
      -DIN=${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lean.cl
      -DOUT=${lean_cl_header} -DSYMBOL=qsgpu_lean_cl
      -P ${PROJECT_SOURCE_DIR}/cmake/script/EmbedTextFile.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lean.cl
            ${PROJECT_SOURCE_DIR}/cmake/script/EmbedTextFile.cmake
    VERBATIM)
  add_custom_target(qsgpu_lean_cl DEPENDS ${lean_cl_header})

  function(qs_add_gpu_helper name edgebits)
    add_executable(${name} qsgpusolve-opencl.cpp ../vendor/lean_ocl.cpp
      ../vendor/blake2b-ref.c)
    add_dependencies(${name} qsgpu_lean_cl)
    target_compile_definitions(${name} PRIVATE EDGEBITS=${edgebits} SQUASH_OUTPUT=1)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/../vendor ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(${name} PRIVATE
      qsgpu_opencl_devices qsgpu_opencl_loader quicksilver_clientversion quicksilver_util
      Threads::Threads warn_interface sanitize_interface)
    qs_gpu_windows_definitions(${name})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
  endfunction()

  qs_add_gpu_helper(qsgpusolve-opencl 28)
  # Linux: <prefix>/libexec/quicksilver/, which is <bindir>/../libexec/quicksilver/
  # as the node searches (src/gpu/helper_config.cpp). Windows: beside the
  # executables, the installer's layout. Never on PATH: it is a helper, not a command.
  if(WIN32)
    install(TARGETS qsgpusolve-opencl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT qsgpusolve-opencl)
  else()
    include(GNUInstallDirs)
    install(TARGETS qsgpusolve-opencl RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/quicksilver COMPONENT qsgpusolve-opencl)
  endif()
  if(BUILD_TESTS)
    qs_add_gpu_helper(qsgpusolve-opencl-e19 19)
  endif()
endif()
