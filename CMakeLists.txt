cmake_minimum_required(VERSION 3.16)

# 本地私密配置（Wi-Fi、访问码）：如果存在 sdkconfig.defaults.local 就自动叠加，它被 .gitignore 忽略。
# 改了这个文件后要删掉 sdkconfig 再编译，新值才会生效。
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
        list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(bambu_screen)
