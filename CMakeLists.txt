# The following three lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tonneda)

add_custom_target(otaupdate
    COMMAND scp "${PROJECT_NAME}.bin" "otaserver:/var/www/html/fsun/esp32_updates/"
    COMMAND esp32update `openssl x509 -noout -subject -in ${CMAKE_SOURCE_DIR}/pki/client.crt -nameopt sep_multiline | grep CN= | cut -d= -f2`
)
