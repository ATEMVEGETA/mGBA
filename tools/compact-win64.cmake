# Pass via CMAKE_PROJECT_INCLUDE after CMake initializes the Windows platform.
# BUILD_STATIC only selects the mGBA core; this selects real static dependency
# archives as well. A separate matching static Qt and recording-only FFmpeg
# prefix must be supplied in CMAKE_PREFIX_PATH / PKG_CONFIG_PATH.
if(NOT WIN32 OR NOT MINGW)
    message(FATAL_ERROR "The compact Windows recipe requires a MinGW toolchain")
endif()
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
set(PKG_CONFIG_ARGN --static CACHE STRING "Use static dependency metadata" FORCE)
# The installed MSYS2 libzip config explicitly imports its DLL, even when a
# static archive is present. Use its static pkg-config metadata instead.
set(CMAKE_DISABLE_FIND_PACKAGE_libzip TRUE)
add_compile_definitions(EPOXY_STATIC_LIB ZIP_STATIC)

function(_compact_win64_finish)
    cmake_policy(SET CMP0079 NEW)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(COMPACT_EXTRA REQUIRED sdl2 freetype2)
    foreach(frontend mgba-qt mgba-sdl)
        if(TARGET ${frontend})
            target_link_directories(${frontend} PRIVATE ${COMPACT_EXTRA_LIBRARY_DIRS})
            target_link_libraries(${frontend} ${COMPACT_EXTRA_LIBRARIES})
        endif()
    endforeach()
    # Some MSYS2 encoder metadata explicitly requests the shared GCC unwind
    # runtime. The compiler already supplies the matching static GCC runtime
    # with -static-libgcc; an explicit gcc_s would undo that choice.
    foreach(binary mgba mgba-qt mgba-sdl)
        if(TARGET ${binary})
            foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
                get_target_property(libraries ${binary} ${property})
                if(libraries)
                    list(REMOVE_ITEM libraries gcc_s "$<LINK_ONLY:gcc_s>")
                    set_property(TARGET ${binary} PROPERTY ${property} "${libraries}")
                endif()
            endforeach()
        endif()
    endforeach()
endfunction()
# These legacy find modules return only the primary archive, not the complete
# dependency list needed by SDL2 and script-font rendering in a static build.
cmake_language(DEFER CALL _compact_win64_finish)
