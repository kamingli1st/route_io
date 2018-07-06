cmake_minimum_required(VERSION 3.0)
project(stream_rt_handler)

set(PROJNAME stream_rt_handler)

IF (NOT DEFINED CMAKE_INSTALL_INCLUDEDIR)
SET(CMAKE_INSTALL_INCLUDEDIR /usr/local/include)
ENDIF(NOT DEFINED CMAKE_INSTALL_INCLUDEDIR)

include_directories(lock-free-stack-and-queue /usr/local/include)

file(GLOB_RECURSE sources lock-free-stack-and-queue/lfqueue/lfqueue.c  lock-free-stack-and-queue/lfqueue/lfqueue.h stream_rt_handler.c stream_rt_handler.h)
file(GLOB_RECURSE example_test lock-free-stack-and-queue/lfqueue/lfqueue.c  lock-free-stack-and-queue/lfqueue/lfqueue.h stream_rt_handler.c stream_rt_handler.h route_sample.c)

add_executable(route_handler ${example_test})
target_link_libraries(route_handler PUBLIC pthread atomic)


SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11 -Wall -Wstrict-prototypes -Wmissing-prototypes")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wmissing-declarations -Wshadow -Wpointer-arith -Wcast-qual")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wsign-compare -pedantic")


###############################################################################
## packaging ##################################################################
###############################################################################
add_library(${PROJNAME}.static STATIC ${sources})
target_link_libraries(${PROJNAME}.static pthread atomic)
set_target_properties(${PROJNAME}.static PROPERTIES OUTPUT_NAME ${PROJNAME})
add_library(${PROJNAME} SHARED ${sources})
target_link_libraries(${PROJNAME} pthread atomic)


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
install(FILES stream_rt_handler.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/srh)

install(CODE "execute_process(COMMAND ldconfig)")

set(CPACK_PACKAGE_NAME "stream_rt_handler")
set(CPACK_PACKAGE_VERSION "1.0.0")

# we don't want to split our program up into several things
set(CPACK_MONOLITHIC_INSTALL 1)

# This must be last
include(CPack)
