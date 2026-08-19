function(vdb_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Werror
      -Wconversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast
      -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
    )
  endif()
endfunction()

