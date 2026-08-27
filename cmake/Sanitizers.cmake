# SHARDKV_SANITIZER selects the build variant. CI builds all three
# (docs/architecture.md, "Sanitizers are part of the build matrix").
#
#   none    Release; benchmarking and ordinary regressions
#   address AddressSanitizer + UndefinedBehaviorSanitizer
#   thread  ThreadSanitizer
#
# address and thread are mutually exclusive -- they instrument the same
# accesses and cannot be linked together.

set(SHARDKV_SANITIZER "none" CACHE STRING "none | address | thread")
set_property(CACHE SHARDKV_SANITIZER PROPERTY STRINGS none address thread)

function(shardkv_apply_sanitizers target)
  if(SHARDKV_SANITIZER STREQUAL "none")
    return()
  endif()

  if(SHARDKV_SANITIZER STREQUAL "address")
    set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  elseif(SHARDKV_SANITIZER STREQUAL "thread")
    set(flags -fsanitize=thread -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "SHARDKV_SANITIZER must be none, address or thread; got '${SHARDKV_SANITIZER}'")
  endif()

  target_compile_options(${target} PRIVATE ${flags} -g)
  target_link_options(${target} PRIVATE ${flags})
endfunction()
