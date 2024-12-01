cmake_minimum_required(VERSION 3.22...3.25)

legacy_check()

add_library(gaze MODULE)
add_library(OBS::gaze ALIAS gaze)

target_sources(gaze PRIVATE color-source-gaze.c gaze.c)

target_link_libraries(gaze PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in gaze.rc)
  target_sources(gaze PRIVATE gaze.rc)
endif()

# cmake-format: off
set_target_properties_obs(gaze PROPERTIES FOLDER plugins PREFIX "")
# cmake-format: on
