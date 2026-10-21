cmake_minimum_required(VERSION 3.16)
project(tauri_lua LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(LUAJIT_BIN luajit REQUIRED)

find_path(LUAJIT_INCLUDE_DIR
    NAMES lua.hpp lua.h
    REQUIRED
    HINTS
    /usr/include/luajit-2.1
    /usr/local/include/luajit-2.1
    /usr/include/luajit-2.0
    /opt/homebrew/include/luajit-2.1
    ${LUAJIT_ROOT}/include
    PATH_SUFFIXES
    luajit-2.1
    luajit-2.0
)

find_library(LUAJIT_LIB
    NAMES luajit-5.1 luajit lua51
    REQUIRED
    HINTS
    /usr/lib
    /usr/lib/x86_64-linux-gnu
    /usr/lib/aarch64-linux-gnu
    /usr/local/lib
    /opt/homebrew/lib
    ${LUAJIT_ROOT}/lib
)

# Compiler warnings and security hardening flags
if(MSVC)
    add_compile_options(/W4 /wd4100 /GS /guard:cf)
    add_link_options(/guard:cf /DYNAMICBASE /NXCOMPAT)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wformat -Wformat-security -fstack-protector-strong)
    if(APPLE)
        add_compile_options(-Wno-nullability-completeness)
    else()
        add_compile_definitions(_FORTIFY_SOURCE=2)
        add_link_options(-Wl,-z,relro -Wl,-z,now)
    endif()
endif()

set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

add_custom_command(
    OUTPUT ${GEN_DIR}/tauri_bc.h
    COMMAND ${LUAJIT_BIN} -b -n tauri ${CMAKE_CURRENT_SOURCE_DIR}/src/tauri/init.lua ${GEN_DIR}/tauri_bc.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/tauri/init.lua
)

# Generate EmbedFile helper inside build directory (never modify source tree)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/cmake)
file(WRITE ${CMAKE_BINARY_DIR}/cmake/EmbedFile.cmake "
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
            -P ${CMAKE_BINARY_DIR}/cmake/EmbedFile.cmake
        DEPENDS ${INPUT_FILE}
    )
endfunction()

# Default embedded fallback is minimal and deny-all (never embed the demo app by default)
option(BUILD_DEMO "Build demo application into embedded runtime" OFF)

if(DEFINED APP_DIR AND EXISTS ${APP_DIR}/src-tauri/src/main.lua)
    set(MAIN_LUA_FILE ${APP_DIR}/src-tauri/src/main.lua)
elseif(DEFINED APP_DIR AND EXISTS ${APP_DIR}/src/main.lua)
    set(MAIN_LUA_FILE ${APP_DIR}/src/main.lua)
elseif(BUILD_DEMO AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/src/main.lua)
    set(MAIN_LUA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/src/main.lua)
else()
    set(MAIN_LUA_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_fallback/main.lua)
endif()

add_custom_command(
    OUTPUT ${GEN_DIR}/main_bc.h
    COMMAND ${LUAJIT_BIN} -b -n main ${MAIN_LUA_FILE} ${GEN_DIR}/main_bc.h
    DEPENDS ${MAIN_LUA_FILE}
)

if(DEFINED APP_DIR AND EXISTS ${APP_DIR}/src-tauri/tauri.conf.json)
    set(TAURI_CONF_FILE ${APP_DIR}/src-tauri/tauri.conf.json)
elseif(DEFINED APP_DIR AND EXISTS ${APP_DIR}/tauri.conf.json)
    set(TAURI_CONF_FILE ${APP_DIR}/tauri.conf.json)
elseif(BUILD_DEMO AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/tauri.conf.json)
    set(TAURI_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/tauri.conf.json)
else()
    set(TAURI_CONF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_fallback/tauri.conf.json)
endif()

embed_file(${TAURI_CONF_FILE} ${GEN_DIR}/config_json.h tauri_conf_json)

set(TAURILUA_SOURCES
    src/taurilua.cpp
    src/native_bridge.cpp
    ${GEN_DIR}/tauri_bc.h
    ${GEN_DIR}/main_bc.h
    ${GEN_DIR}/config_json.h
)

# 1. Embeddable Shared Library (libtaurilua.so / libtaurilua.dylib / taurilua.dll)
add_library(taurilua SHARED
    ${TAURILUA_SOURCES}
)

target_include_directories(taurilua PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(taurilua PRIVATE
    ${GEN_DIR}
    src
    ${LUAJIT_INCLUDE_DIR}
)

target_compile_definitions(taurilua PRIVATE TAURILUA_BUILD_DLL)

# 2. Standalone Application Executable
add_executable(tauri_app
    src/entry.cpp
)

target_include_directories(tauri_app PRIVATE
    ${GEN_DIR}
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LUAJIT_INCLUDE_DIR}
)

target_link_libraries(tauri_app PRIVATE taurilua)

# 3. Host C Embedding Example
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/host_c/main.c)
    add_executable(host_c_example
        examples/host_c/main.c
    )
    target_include_directories(host_c_example PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(host_c_example PRIVATE taurilua)
endif()

# 4. CI Smoke Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.c)
    add_executable(smoke_test
        tests/smoke_test.c
    )
    target_include_directories(smoke_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(smoke_test PRIVATE taurilua)
endif()

# 5. Native Security Unit Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_security.cpp)
    add_executable(test_native_security
        tests/test_native_security.cpp
    )
    target_include_directories(test_native_security PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endif()

# 6. Origin Validator Unit Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_origin_validator.cpp)
    add_executable(test_origin_validator
        tests/test_origin_validator.cpp
    )
    target_include_directories(test_origin_validator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endif()

# 7. Token & Envelope Standalone Unit Test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_token_standalone.cpp)
    add_executable(test_token_standalone
        tests/test_token_standalone.cpp
    )
    target_include_directories(test_token_standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endif()

# Automated CTest Registration
enable_testing()

if(TARGET smoke_test)
    add_test(NAME smoke_test COMMAND smoke_test)
endif()

if(TARGET test_native_security)
    add_test(NAME test_native_security COMMAND test_native_security)
endif()

if(TARGET test_origin_validator)
    add_test(NAME test_origin_validator COMMAND test_origin_validator)
endif()

if(TARGET test_token_standalone)
    add_test(NAME test_token_standalone COMMAND test_token_standalone)
endif()

add_test(NAME test_security_set1 COMMAND ${LUAJIT_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_security_set1.lua)
set_tests_properties(test_security_set1 PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME test_json_rfc8259 COMMAND ${LUAJIT_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_json_rfc8259.lua)
set_tests_properties(test_json_rfc8259 PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME test_audit COMMAND ${LUAJIT_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audit.lua)
set_tests_properties(test_audit PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

if(APPLE)
    target_compile_definitions(taurilua PRIVATE WEBVIEW_COCOA)
    enable_language(OBJCXX)
    set_source_files_properties(src/native_bridge.cpp PROPERTIES LANGUAGE OBJCXX)
    find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
    target_link_libraries(taurilua PRIVATE
        ${LUAJIT_LIB}
        ${WEBKIT_FRAMEWORK}
        ${COCOA_FRAMEWORK}
        ${USERNOTIFICATIONS_FRAMEWORK}
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        target_link_options(taurilua PRIVATE -Wl,-pagezero_size,10000 -Wl,-image_base,100000000)
    endif()
    set_target_properties(taurilua PROPERTIES
        OUTPUT_NAME "taurilua"
        MACOSX_RPATH TRUE
    )
    set_target_properties(tauri_app PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path;@executable_path/../Frameworks;@loader_path"
    )
    if(TARGET host_c_example)
        set_target_properties(host_c_example PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path;@executable_path/../Frameworks;@loader_path"
        )
    endif()
    if(TARGET smoke_test)
        set_target_properties(smoke_test PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path;@executable_path/../Frameworks;@loader_path"
        )
    endif()

elseif(WIN32)
    target_compile_definitions(taurilua PRIVATE WEBVIEW_EDGE)
    find_path(WEBVIEW2_INCLUDE_DIR WebView2.h PATHS "$ENV{WEBVIEW2_PATH}/include" "${CMAKE_CURRENT_SOURCE_DIR}/deps/webview2/include")
    find_library(WEBVIEW2_STATIC_LIB WebView2LoaderStatic PATHS "$ENV{WEBVIEW2_PATH}/lib/x64" "${CMAKE_CURRENT_SOURCE_DIR}/deps/webview2/lib/x64")
    target_include_directories(taurilua PRIVATE ${WEBVIEW2_INCLUDE_DIR})
    target_link_libraries(taurilua PRIVATE
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
    target_compile_definitions(taurilua PRIVATE WEBVIEW_GTK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK REQUIRED gtk+-3.0)
    pkg_check_modules(WEBKIT webkit2gtk-4.1)
    if(NOT WEBKIT_FOUND)
        pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.0)
    endif()
    target_include_directories(taurilua PRIVATE ${GTK_INCLUDE_DIRS} ${WEBKIT_INCLUDE_DIRS})
    target_link_libraries(taurilua PRIVATE
        ${GTK_LIBRARIES}
        ${WEBKIT_LIBRARIES}
        ${LUAJIT_LIB}
        dl
        m
        pthread
    )
    set_target_properties(taurilua PROPERTIES ENABLE_EXPORTS TRUE)
    set_target_properties(tauri_app PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
    )
    if(TARGET host_c_example)
        set_target_properties(host_c_example PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
        )
    endif()
endif()