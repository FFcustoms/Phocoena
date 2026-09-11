# Static driver artifacts are produced by experiments/nxvk/dolphin-build.sh.
# Never search the host or silently substitute the SDK's OpenGL driver.
if(NOT HORIZON_NXVK_SOURCE OR NOT EXISTS "${HORIZON_NXVK_SOURCE}/switch/smoke/nvk_compat.c")
  message(FATAL_ERROR "Set HORIZON_NXVK_SOURCE to the pinned NXVK checkout")
endif()
if(NOT HORIZON_NXVK_ARCHIVES OR NOT EXISTS "${HORIZON_NXVK_ARCHIVES}")
  message(FATAL_ERROR "Set HORIZON_NXVK_ARCHIVES to the generated archive inventory")
endif()
file(STRINGS "${HORIZON_NXVK_ARCHIVES}" _nxvk_archives)
foreach(archive IN LISTS _nxvk_archives)
  if(NOT IS_ABSOLUTE "${archive}" OR NOT EXISTS "${archive}")
    message(FATAL_ERROR "Missing or non-absolute NXVK archive: ${archive}")
  endif()
endforeach()
list(POP_FRONT _nxvk_archives _nxvk_driver)
add_library(horizon_nxvk_compat OBJECT "${HORIZON_NXVK_SOURCE}/switch/smoke/nvk_compat.c")
add_library(horizon_nxvk INTERFACE)
# Place the newlib gap fills as an object, not an archive encountered before
# its driver callers. NintendoSwitch.cmake has no LINK_LIBRARY feature table.
target_link_libraries(horizon_nxvk INTERFACE "$<TARGET_OBJECTS:horizon_nxvk_compat>"
  "-Wl,--whole-archive" "${_nxvk_driver}" "-Wl,--no-whole-archive"
  "-Wl,--start-group" ${_nxvk_archives} z expat nx c m stdc++ pthread "-Wl,--end-group")
