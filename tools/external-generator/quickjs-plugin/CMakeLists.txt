cmake_minimum_required(VERSION 3.20)
project(vse-quickjs-gen LANGUAGES C)

# Standalone optional plugin binary (NOT linked into v-simple-editor.exe).
# Pass -DQUICKJS_DIR=/path/to/quickjs-source (containing quickjs.h & quickjs.c).
set(QUICKJS_DIR "" CACHE PATH "Path to QuickJS source directory")
if(NOT EXISTS "${QUICKJS_DIR}/quickjs.h")
    message(FATAL_ERROR "Set -DQUICKJS_DIR=/path/to/quickjs (missing quickjs.h)")
endif()

add_executable(vse-quickjs-gen
    main.c
    "${QUICKJS_DIR}/quickjs.c"
    "${QUICKJS_DIR}/libregexp.c"
    "${QUICKJS_DIR}/libunicode.c"
    "${QUICKJS_DIR}/cutils.c"
)
target_include_directories(vse-quickjs-gen PRIVATE "${QUICKJS_DIR}")
target_compile_definitions(vse-quickjs-gen PRIVATE CONFIG_VERSION="2024-01-13")
