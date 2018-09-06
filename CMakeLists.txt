cmake_minimum_required(VERSION 2.8)
project(route_io)

set(PROJNAME routeio)

IF (WIN32 OR MSVC OR MSYS OR MINGW)
SET(RIO_COMPILE_WINDOWS TRUE)
ELSEif(UNIX AND NOT APPLE)
SET(RIO_COMPILE_LINUX TRUE)
endif()

if(DEFINED RIO_COMPILE_LINUX) 
MESSAGE(STATUS "Unix Build ... ")

IF (NOT DEFINED CMAKE_INSTALL_INCLUDEDIR)
SET(CMAKE_INSTALL_INCLUDEDIR /usr/local/include)
ENDIF(NOT DEFINED CMAKE_INSTALL_INCLUDEDIR)

include_directories(/usr/local/include /usr/include atomic_threadpool atomic_threadpool/lfqueue)

file(GLOB_RECURSE sources route_io_linux.c route_io_http.c route_io.h atomic_threadpool/at_thpool.c atomic_threadpool/at_thpool.h atomic_threadpool/lfqueue/lfqueue.c atomic_threadpool/lfqueue/lfqueue.h)
file(GLOB_RECURSE example_test route_io_linux.c route_io_http.c route_echoback_example.c route_io.h atomic_threadpool/at_thpool.c atomic_threadpool/at_thpool.h atomic_threadpool/lfqueue/lfqueue.c atomic_threadpool/lfqueue/lfqueue.h)

add_executable(route_echoback_handler ${example_test})
target_link_libraries(route_echoback_handler PUBLIC pthread)


SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11 -Wall -Wstrict-prototypes -Wmissing-prototypes")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wsign-compare -pedantic")

###############################################################################
## packaging ##################################################################
###############################################################################
add_library(${PROJNAME}.static STATIC ${sources})
target_link_libraries(${PROJNAME}.static pthread)
set_target_properties(${PROJNAME}.static PROPERTIES OUTPUT_NAME ${PROJNAME})
add_library(${PROJNAME} SHARED ${sources})
target_link_libraries(${PROJNAME} pthread)


# link_directories(/usr/local/lib /usr/lib)

## For .a library
install(TARGETS ${PROJNAME}.static EXPORT ${PROJNAME}
    ARCHIVE DESTINATION lib${LIB_SUFFIX}
    LIBRARY DESTINATION lib${LIB_SUFFIX}
    # RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

## For .so library
install(TARGETS ${PROJNAME} EXPORT ${PROJNAME}
    ARCHIVE DESTINATION lib${LIB_SUFFIX}
    LIBRARY DESTINATION lib${LIB_SUFFIX}
    # RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
install(FILES route_io.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(CODE "execute_process(COMMAND ldconfig)")


configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()


# if(DEFINED RIO_COMPILE_WINDOWS) 
# MESSAGE(STATUS "Windows Build ...")

# file(GLOB_RECURSE sources route_io_linux.c route_io.h)
# file(GLOB_RECURSE example_test route_io_linux.c route_io.h route_echoback_example.c)

# # SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static-libgcc")

# add_executable(route_echoback_handler ${example_test})
# target_link_libraries(route_echoback_handler PUBLIC static-libgcc ws2_32 wsock32 shlwapi)

# add_library(${PROJNAME} SHARED ${sources})
# target_link_libraries(${PROJNAME} static-libgcc ws2_32 wsock32 shlwapi)

# endif()


set(CPACK_PACKAGE_NAME "route_io")
set(CPACK_PACKAGE_VERSION "1.0.0")

# we don't want to split our program up into several things
set(CPACK_MONOLITHIC_INSTALL 1)

# This must be last
include(CPack)
