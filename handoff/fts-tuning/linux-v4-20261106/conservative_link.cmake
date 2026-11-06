if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_link_options("-Wl,--threads=2")
endif()
