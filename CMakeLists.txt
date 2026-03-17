cmake_minimum_required(VERSION 3.10)
project(vfio-user-edu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g")

# Find libvfio-user
# Try pkg-config first, then fall back to manual paths
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(VFIO_USER QUIET libvfio-user)
endif()

if(NOT VFIO_USER_FOUND)
    # Manual search
    find_path(VFIO_USER_INCLUDE_DIR
        NAMES libvfio-user.h
        PATHS /usr/local/include/vfio-user
              /usr/include/vfio-user
              ${VFIO_USER_PREFIX}/include/vfio-user
    )
    find_library(VFIO_USER_LIBRARY
        NAMES vfio-user
        PATHS /usr/local/lib
              /usr/lib
              ${VFIO_USER_PREFIX}/lib
    )

    if(VFIO_USER_INCLUDE_DIR AND VFIO_USER_LIBRARY)
        set(VFIO_USER_INCLUDE_DIRS ${VFIO_USER_INCLUDE_DIR}
            "${VFIO_USER_INCLUDE_DIR}/..")
        set(VFIO_USER_LIBRARIES ${VFIO_USER_LIBRARY})
        message(STATUS "Found libvfio-user: ${VFIO_USER_LIBRARY}")
    else()
        message(FATAL_ERROR
            "libvfio-user not found. Install it or set VFIO_USER_PREFIX:\n"
            "  cmake -DVFIO_USER_PREFIX=/path/to/libvfio-user/install ..")
    endif()
endif()

find_package(Threads REQUIRED)

add_executable(edu_device edu_device.c)
target_include_directories(edu_device PRIVATE ${VFIO_USER_INCLUDE_DIRS})
target_link_libraries(edu_device ${VFIO_USER_LIBRARIES} Threads::Threads)

install(TARGETS edu_device RUNTIME DESTINATION bin)
