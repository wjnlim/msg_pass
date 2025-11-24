cmake_minimum_required(VERSION 3.15)

if ("${CMAKE_BINARY_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
  message(FATAL_ERROR "In-source builds are not allowed.
    Please create a subfolder and use `cmake ..` inside it.
    NOTE: cmake creates CMakeCache.txt and CMakeFiles/*.
          Remove them, or cmake will refuse to work.")
endif()

project(Msg_pass C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

include(FetchContent)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    Fetchcontent_Declare(
        Ep_engine
        GIT_REPOSITORY https://github.com/wjnlim/ep_engine.git
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(Ep_engine)

else()
    Fetchcontent_Declare(
        Ep_engine
        GIT_REPOSITORY https://github.com/wjnlim/ep_engine.git
    )
    FetchContent_GetProperties(Ep_engine)
    string(TOLOWER Ep_engine lc_Ep_engine)
    if(NOT ${lc_Ep_engine}_POPULATED)
        FetchContent_Populate(Ep_engine)
        add_subdirectory(${${lc_Ep_engine}_SOURCE_DIR} ${${lc_Ep_engine}_BINARY_DIR} EXCLUDE_FROM_ALL)
        # add_subdirectory(${${lc_Ep_engine}_SOURCE_DIR} ${${lc_Ep_engine}_BINARY_DIR})
    endif()
endif()

add_library(mp_objs OBJECT 
    src/mp_client.c
    src/mp_conn.c
    src/mp_server.c
)
target_include_directories(mp_objs 
    PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(mp_objs 
    PRIVATE
        $<$<CONFIG:Debug>: -D DEBUG_MP -O0 -g>
)
find_library(PTHREAD_LIB pthread REQUIRED)
if (PTHREAD_LIB)
    target_link_libraries(mp_objs PUBLIC ${PTHREAD_LIB})
endif()
if(DEBUG_MP)
    target_compile_definitions(mp_objs PRIVATE DEBUG_MP)
endif()
# link to utils_objs, utils_ds_objs, ep_engine_objs to get usage requirements
target_link_libraries(mp_objs PUBLIC 
    utils_objs
    utils_ds_objs
    ep_engine_objs
)

add_library(msg_pass
    # $<TARGET_OBJECTS:mp_objs>
)
target_link_libraries(msg_pass PUBLIC 
    mp_objs
    ep_engine_objs
    utils_objs
    utils_ds_objs
)
install(DIRECTORY include/msg_pass DESTINATION include)
install(TARGETS msg_pass
        ARCHIVE DESTINATION lib)

# target_link_libraries(msg_pass PUBLIC ep_engine_lib)

# find_library(PTHREAD_LIB pthread REQUIRED)
# if (PTHREAD_LIB)
#     set_property(TARGET mp_objs PROPERTY INTERFACE_LINK_LIBRARIES
#                     ${PTHREAD_LIB})
# endif()