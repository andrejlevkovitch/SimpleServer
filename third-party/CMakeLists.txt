# cmake for third-party modules

# init and update git submodules
if(NOT EXISTS simple_logs)
  find_package(Git REQUIRED)
  execute_process(COMMAND ${GIT_EXECUTABLE} submodule init)
  execute_process(COMMAND ${GIT_EXECUTABLE} submodule update)
endif()

add_subdirectory(simple_logs)
