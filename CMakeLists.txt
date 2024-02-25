cmake_minimum_required(VERSION 3.12)
project(minitelnet)

if(NOT WITH_MINIIO)
    message(FATAL_ERROR "Miniio required.")
endif()

set(srcs
    minitelnet.c
    libtelnet/libtelnet.c)
if(MINITELNET_EMBEDDED)
    add_library(minitelnet OBJECT ${srcs})
    target_compile_definitions(minitelnet PRIVATE
        MINITELNET_EMBEDDED=1)
else()
    list(APPEND ${WITH_MINIIO}/src/miniio_${IO_BACKEND}.c)
    add_executable(minitelnet ${srcs})
    if(${IO_BACKEND} STREQUAL uv)
        if(uvlib)
            target_link_libraries(minitelnet ${uvlib})
        endif()
    endif()
endif()

target_include_directories(minitelnet PRIVATE libtelnet ${WITH_MINIIO}/include)

