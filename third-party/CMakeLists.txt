# cmake for third-party modules

# init and update git submodules
find_package(Git REQUIRED)
execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive)

add_subdirectory(simple_logs)
