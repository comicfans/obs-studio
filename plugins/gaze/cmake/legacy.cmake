project(gaze)

add_library(gaze MODULE)
add_library(OBS::gaze ALIAS gaze)

target_sources(gaze PRIVATE gaze.c color-source-gaze.c)

target_link_libraries(gaze PRIVATE OBS::libobs)

if(OS_WINDOWS)
  if(MSVC)
    target_link_libraries(gaze PRIVATE OBS::w32-pthreads)
  endif()

  set(MODULE_DESCRIPTION "OBS gaze module")
  configure_file(${CMAKE_SOURCE_DIR}/cmake/bundle/windows/obs-module.rc.in gaze.rc)

  target_sources(gaze PRIVATE gaze.rc)
endif()

set_target_properties(gaze PROPERTIES FOLDER "plugins" PREFIX "")

setup_plugin_target(gaze)
