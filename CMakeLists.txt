cmake_minimum_required(VERSION 3.16)

project(cpu-client LANGUAGES C)

add_executable(cpu-client main.c)

include(GNUInstallDirs)
install(TARGETS cpu-client
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
