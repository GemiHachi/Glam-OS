# ESP-IDF Project CMakeLists.txt for G'lam OS
cmake_minimum_required(VERSION 3.16)

# Extra component directories
set(EXTRA_COMPONENT_DIRS "components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(glam_os)
