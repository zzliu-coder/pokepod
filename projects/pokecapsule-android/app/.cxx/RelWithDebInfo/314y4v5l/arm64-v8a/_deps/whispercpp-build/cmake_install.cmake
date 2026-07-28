# Install script for directory: /Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/work/vendor/whisper.cpp

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "0")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/opt/homebrew/share/android-commandlinetools/ndk/27.2.12479018/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objdump")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/src/libwhisper.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/work/vendor/whisper.cpp/include/whisper.h")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/src/libparakeet.a")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/work/vendor/whisper.cpp/include/parakeet.h")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/whisper" TYPE FILE FILES
    "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/whisper-config.cmake"
    "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/whisper-version.cmake"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/whisper.pc")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/parakeet" TYPE FILE FILES
    "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/parakeet-config.cmake"
    "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/parakeet-version.cmake"
    )
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/parakeet.pc")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/ggml/cmake_install.cmake")
  include("/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/314y4v5l/arm64-v8a/_deps/whispercpp-build/src/cmake_install.cmake")

endif()

