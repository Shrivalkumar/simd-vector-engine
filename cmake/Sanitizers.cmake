function(vdb_enable_sanitizers target)
  if(VDB_ENABLE_SANITIZERS AND NOT MSVC)
    # macOS 26's current arm64 ASan runtime can spin during dyld allocator
    # initialization before main. Keep the Apple gate useful with UBSan; run
    # ASan in Linux CI, where the shadow-memory runtime is stable.
    if(APPLE)
      target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=undefined)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()
