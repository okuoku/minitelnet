cmake_minimum_required(VERSION 3.12)
project(minitelnet)

if(NOT WITH_MINIIO)
    message(FATAL_ERROR "Miniio required.")
endif()

set(srcs
    minitelnet.c
    ${WITH_MINIIO}/src/miniio_${IO_BACKEND}.c
    libtelnet/libtelnet.c)

add_executable(minitelnet ${srcs})

target_include_directories(minitelnet PRIVATE libtelnet ${WITH_MINIIO}/include)

if(${IO_BACKEND} STREQUAL uv)
    if(uvlib)
        target_link_libraries(minitelnet ${uvlib})
    endif()
endif()
