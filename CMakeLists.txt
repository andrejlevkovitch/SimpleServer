# cmake

cmake_minimum_required(VERSION 3.12)

project(ss)

include(cmake/build.cmake)

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(third-party)


set(PROJECT_SRC
  src/main.cpp
  src/ss/Server.cpp
  )

add_library(${PROJECT_NAME} src/ss/Server.cpp)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
target_link_libraries(${PROJECT_NAME} PRIVATE
  Boost::boost
  simple_logs
  )
target_include_directories(${PROJECT_NAME} PUBLIC
  include
  )

add_executable(echo_server test/main.cpp)
target_link_libraries(echo_server PRIVATE
  ${PROJECT_NAME}
  simple_logs
  Threads::Threads
  )
