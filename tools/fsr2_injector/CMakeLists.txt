cmake_minimum_required(VERSION 3.12.1)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(fsr2injector LANGUAGES CXX)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
endif()

add_executable(fsr2injector
    main.cpp)

target_compile_features(fsr2injector PRIVATE cxx_std_17)
target_compile_definitions(fsr2injector PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(fsr2injector PRIVATE advapi32)

set_target_properties(fsr2injector PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_HOME_DIRECTORY}/bin"
    DEBUG_POSTFIX "d")
