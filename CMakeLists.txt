cmake_minimum_required(VERSION 3.16)
project(tauri_lua LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(LUAJIT_BIN luajit REQUIRED)

find_path(LUAJIT_INCLUDE_DIR lua.hpp
    HINTS
    /usr/include/luajit-2.1
    /usr/local/include/luajit-2.1
    /opt/homebrew/include/luajit-2.1
    ${LUAJIT_ROOT}/include
)

find_library(LUAJIT_LIB
    NAMES luajit-5.1 luajit lua51
    HINTS
    /usr/lib
    /usr/local/lib
    /opt/homebrew/lib
    ${LUAJIT_ROOT}/lib
)

set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

add_custom_command(
    OUTPUT ${GEN_DIR}/tauri_bc.h
    COMMAND ${LUAJIT_BIN} -b -n tauri ${CMAKE_CURRENT_SOURCE_DIR}/src/tauri/init.lua ${GEN_DIR}/tauri_bc.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/tauri/init.lua
)

if(NOT DEFINED APP_DIR)
    set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(EXISTS ${APP_DIR}/src-tauri/src/main.lua)
    set(MAIN_LUA_FILE ${APP_DIR}/src-tauri/src/main.lua)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/src/main.lua)
    set(MAIN_LUA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/src/main.lua)
else()
    set(MAIN_LUA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/main.lua)
endif()

add_custom_command(
    OUTPUT ${GEN_DIR}/main_bc.h
    COMMAND ${LUAJIT_BIN} -b -n main ${MAIN_LUA_FILE} ${GEN_DIR}/main_bc.h
    DEPENDS ${MAIN_LUA_FILE}
)

file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake "
file(READ \${INPUT_FILE} HEX_CONTENT HEX)
string(LENGTH \"\${HEX_CONTENT}\" HEX_LEN)
math(EXPR BYTE_COUNT \"\${HEX_LEN} / 2\")
set(ARRAY_DEF \"\")
set(I 0)
while(I LESS HEX_LEN)
    string(SUBSTRING \"\${HEX_CONTENT}\" \${I} 2 PAIR)
    string(APPEND ARRAY_DEF \"0x\${PAIR},\")
    math(EXPR I \"\${I} + 2\")
endwhile()
file(WRITE \${OUTPUT_HEADER} \"#pragma once\\nstatic const unsigned char \${VAR_NAME}[] = {\${ARRAY_DEF}0x00};\\nstatic const unsigned int \${VAR_NAME}_len = \${BYTE_COUNT};\\n\")
")

function(embed_file INPUT_FILE OUTPUT_HEADER VAR_NAME)
    add_custom_command(
        OUTPUT ${OUTPUT_HEADER}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${INPUT_FILE}
            -DOUTPUT_HEADER=${OUTPUT_HEADER}
            -DVAR_NAME=${VAR_NAME}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
        DEPENDS ${INPUT_FILE}
    )
endfunction()

if(EXISTS ${APP_DIR}/src-tauri/tauri.conf.json)
    set(TAURI_CONF_FILE ${APP_DIR}/src-tauri/tauri.conf.json)
elseif(EXISTS ${APP_DIR}/tauri.conf.json)
    set(TAURI_CONF_FILE ${APP_DIR}/tauri.conf.json)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/tauri.conf.json)
    set(TAURI_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/tauri.conf.json)
else()
    set(TAURI_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/tauri.conf.json)
endif()

embed_file(${TAURI_CONF_FILE} ${GEN_DIR}/config_json.h tauri_conf_json)

add_executable(tauri_app
    src/entry.cpp
    src/native_bridge.cpp
    ${GEN_DIR}/tauri_bc.h
    ${GEN_DIR}/main_bc.h
    ${GEN_DIR}/config_json.h
)

target_include_directories(tauri_app PRIVATE
    ${GEN_DIR}
    src
    include
    ${LUAJIT_INCLUDE_DIR}
)

if(APPLE)
    target_compile_definitions(tauri_app PRIVATE WEBVIEW_COCOA)
    enable_language(OBJCXX)
    set_source_files_properties(src/native_bridge.cpp PROPERTIES LANGUAGE OBJCXX)
    find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(tauri_app PRIVATE
        ${LUAJIT_LIB}
        ${WEBKIT_FRAMEWORK}
        ${COCOA_FRAMEWORK}
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        target_link_options(tauri_app PRIVATE -Wl,-pagezero_size,10000 -Wl,-image_base,100000000)
    endif()

elseif(WIN32)
    target_compile_definitions(tauri_app PRIVATE WEBVIEW_EDGE)
    find_path(WEBVIEW2_INCLUDE_DIR WebView2.h PATHS "$ENV{WEBVIEW2_PATH}/include" "${CMAKE_CURRENT_SOURCE_DIR}/deps/webview2/include")
    find_library(WEBVIEW2_STATIC_LIB WebView2LoaderStatic PATHS "$ENV{WEBVIEW2_PATH}/lib/x64" "${CMAKE_CURRENT_SOURCE_DIR}/deps/webview2/lib/x64")
    target_include_directories(tauri_app PRIVATE ${WEBVIEW2_INCLUDE_DIR})
    target_link_libraries(tauri_app PRIVATE
        ${LUAJIT_LIB}
        ${WEBVIEW2_STATIC_LIB}
        ws2_32.lib
        version.lib
        shlwapi.lib
        ole32.lib
        oleaut32.lib
        advapi32.lib
        user32.lib
        comdlg32.lib
        shell32.lib
    )
    set_target_properties(tauri_app PROPERTIES WIN32_EXECUTABLE TRUE)

else()
    target_compile_definitions(tauri_app PRIVATE WEBVIEW_GTK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK REQUIRED gtk+-3.0)
    pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.0)
    target_include_directories(tauri_app PRIVATE ${GTK_INCLUDE_DIRS} ${WEBKIT_INCLUDE_DIRS})
    target_link_libraries(tauri_app PRIVATE
        ${GTK_LIBRARIES}
        ${WEBKIT_LIBRARIES}
        ${LUAJIT_LIB}
        dl
        m
        pthread
    )
    set_target_properties(tauri_app PROPERTIES ENABLE_EXPORTS TRUE)
endif()