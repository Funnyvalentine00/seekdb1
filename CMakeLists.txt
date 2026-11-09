cmake_minimum_required(VERSION 3.10)
project(MiniVectorDB CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(minivectordb
  src/main.cpp
  src/sql.cpp
  src/storage.cpp
  src/engine.cpp
  src/vecops.cpp
)

target_compile_options(minivectordb PRIVATE -O3 -Wall -Wextra)
target_link_options(minivectordb PRIVATE -static-libstdc++ -static-libgcc)

install(TARGETS minivectordb RUNTIME DESTINATION bin)
