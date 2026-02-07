# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# Root CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Read version from main/version.txt
file(READ ${CMAKE_SOURCE_DIR}/main/version.txt VERSION_STR)
string(STRIP ${VERSION_STR} VERSION_STR)

project(esp32_aws_mqtt VERSION 1.0.8)
