# Windows standalone backends only; live and the plug-ins keep their native audio.
if(CMAKE_VERSION VERSION_LESS 3.18)
  message(FATAL_ERROR "ASIO=ON requires CMake 3.18 or later")
endif()
enable_language(C)
set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "Build static PortAudio" FORCE)
set(PA_BUILD_TESTS OFF CACHE BOOL "Build PortAudio tests" FORCE)
set(PA_BUILD_EXAMPLES OFF CACHE BOOL "Build PortAudio examples" FORCE)
set(PA_USE_ASIO ON CACHE BOOL "Enable PortAudio ASIO" FORCE)
set(PA_USE_DS ON CACHE BOOL "Enable PortAudio DirectSound" FORCE)
add_subdirectory(third_party/portaudio)
get_target_property(_smu_portaudio_definitions portaudio COMPILE_DEFINITIONS)
if(NOT "PA_USE_ASIO=1" IN_LIST _smu_portaudio_definitions)
  message(FATAL_ERROR "ASIO=ON requires the Steinberg ASIO SDK")
endif()
target_sources(gui PRIVATE src/ui/audio_out_portaudio.cpp)
target_compile_definitions(gui PRIVATE SMU2000_ASIO=1)
target_link_libraries(gui PRIVATE portaudio)
add_custom_command(TARGET gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/portaudio/ASIO-GPL-3.0.txt"
    "$<TARGET_FILE_DIR:gui>/ASIO-GPL-3.0.txt"
  VERBATIM)
