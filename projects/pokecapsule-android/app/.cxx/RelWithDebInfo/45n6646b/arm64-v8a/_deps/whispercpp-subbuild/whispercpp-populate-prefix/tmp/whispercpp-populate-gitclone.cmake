
if(NOT "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-subbuild/whispercpp-populate-prefix/src/whispercpp-populate-stamp/whispercpp-populate-gitinfo.txt" IS_NEWER_THAN "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-subbuild/whispercpp-populate-prefix/src/whispercpp-populate-stamp/whispercpp-populate-gitclone-lastrun.txt")
  message(STATUS "Avoiding repeated git clone, stamp file is up to date: '/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-subbuild/whispercpp-populate-prefix/src/whispercpp-populate-stamp/whispercpp-populate-gitclone-lastrun.txt'")
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-src"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: '/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-src'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "/usr/bin/git"  clone --no-checkout --depth 1 --no-single-branch --config "advice.detachedHead=false" "https://github.com/ggml-org/whisper.cpp.git" "whispercpp-src"
    WORKING_DIRECTORY "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps"
    RESULT_VARIABLE error_code
    )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(STATUS "Had to git clone more than once:
          ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/ggml-org/whisper.cpp.git'")
endif()

execute_process(
  COMMAND "/usr/bin/git"  checkout f049fff95a089aa9969deb009cdd4892b3e74916 --
  WORKING_DIRECTORY "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-src"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: 'f049fff95a089aa9969deb009cdd4892b3e74916'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "/usr/bin/git"  submodule update --recursive --init 
    WORKING_DIRECTORY "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-src"
    RESULT_VARIABLE error_code
    )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: '/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-src'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy
    "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-subbuild/whispercpp-populate-prefix/src/whispercpp-populate-stamp/whispercpp-populate-gitinfo.txt"
    "/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-subbuild/whispercpp-populate-prefix/src/whispercpp-populate-stamp/whispercpp-populate-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: '/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/projects/pokecapsule-android/app/.cxx/RelWithDebInfo/45n6646b/arm64-v8a/_deps/whispercpp-subbuild/whispercpp-populate-prefix/src/whispercpp-populate-stamp/whispercpp-populate-gitclone-lastrun.txt'")
endif()

