cmake_minimum_required(VERSION 3.0.0)
project(VideoPlayer VERSION 0.1.0)

include(CTest)
enable_testing()

add_executable(${PROJECT_NAME} src/main.c)
target_link_libraries(${PROJECT_NAME} SDL2 avcodec avformat swresample swscale avutil)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
