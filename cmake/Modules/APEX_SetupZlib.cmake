#  Copyright (c) 2021 University of Oregon
#
#  Distributed under the Boost Software License, Version 1.0. (See accompanying
#  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

# Setup an imported target for zlib
find_package(ZLIB)
if(ZLIB_FOUND)
  hpx_info("apex" "Building APEX with ZLIB support.")

  if(NOT TARGET ZLIB::ZLIB)
    # Add an imported target
    add_library(ZLIB::ZLIB INTERFACE IMPORTED)
    set_property(TARGET ZLIB::ZLIB PROPERTY
      INTERFACE_INCLUDE_DIRECTORIES ${ZLIB_INCLUDE_DIR})
    
    # If supported, avoid issues with link-time keywords, such as:
    # <Property INTERFACE_LINK_LIBRARIES may not contain link-type keyword
    # "optimized".>
    if(CMAKE_VERSION VERSION_LESS 3.11)
      set_property(TARGET ZLIB::ZLIB PROPERTY
        INTERFACE_LINK_LIBRARIES ${ZLIB_LIBRARIES})
    else()
      target_link_libraries(ZLIB::ZLIB INTERFACE ${ZLIB_LIBRARIES})
    endif()
  endif()

  set(CMAKE_INSTALL_RPATH ${CMAKE_INSTALL_RPATH} ${ZLIB_LIBRARY_DIR})
  target_compile_definitions(apex_flags INTERFACE APEX_HAVE_ZLIB)
  message(INFO " Using zlib: ${ZLIB_INCLUDE_DIR}")
  message(INFO " Using zlib: ${ZLIB_LIBRARY_DIR} ${ZLIB_LIBRARIES}")

  list(APPEND _apex_imported_targets ZLIB::ZLIB)

endif()
