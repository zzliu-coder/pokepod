cmake_minimum_required(VERSION 3.22.1)
project(pokecapsule_whisper LANGUAGES C CXX)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_OPENCL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    whispercpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG f049fff95a089aa9969deb009cdd4892b3e74916
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(whispercpp)

add_library(pokecapsule_whisper SHARED whisper_jni.cpp)
target_include_directories(pokecapsule_whisper PRIVATE ${whispercpp_SOURCE_DIR}/include)
target_link_libraries(pokecapsule_whisper PRIVATE whisper android log)
