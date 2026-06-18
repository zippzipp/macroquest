# Generated from MacroQuest.vcxproj
# This file is designed to work with add_subdirectory()

# ---------------------------------------------------------------------
# Props file includes
# ---------------------------------------------------------------------
include("../Common.cmake")

# ---------------------------------------------------------------------
# vcpkg dependencies
# ---------------------------------------------------------------------
# Guard crashpad find_package: src/main may have already imported this target.
# crashpadConfig.cmake unconditionally calls add_library(crashpad INTERFACE) which
# CMake policy CMP0002 treats as a fatal duplicate if the target already exists.
if(NOT TARGET crashpad::crashpad)
    find_package(crashpad REQUIRED)
endif()
find_package(spdlog REQUIRED)
find_package(wil REQUIRED)
find_package(date REQUIRED)
find_package(curl REQUIRED)
find_package(cpr REQUIRED)
find_package(pe-parse REQUIRED)
find_package(protobuf REQUIRED)

# ---------------------------------------------------------------------
# Header files
# ---------------------------------------------------------------------
set(MacroQuest_HEADERS
    "../../include/mq/base/BuildInfo.h"
    "../../include/mq/utils/Markov.h"
    "../../include/mq/utils/Naming.h"
    "LoaderAutoLogin.h"
    "Crashpad.h"
    "ImGui.h"
    "imgui_backend/imgui_engine.h"
    "imgui_backend/imgui_impl_dx11.h"
    "imgui_backend/imgui_impl_win32.h"
    "MacroQuest.h"
    "PostOffice.h"
    "ProcessMonitor.h"
    "resource.h"
    "WinToastLib.h"
)

source_groups("Header Files" ${MacroQuest_HEADERS})

# ---------------------------------------------------------------------
# Other files
# ---------------------------------------------------------------------
set(MacroQuest_OTHERFILES
    "MacroQuest.rc"
)

source_group("Resource Files" FILES ${MacroQuest_OTHERFILES})

# ---------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------
set(MacroQuest_SOURCES
    "../common/HotKeys.cpp"
    "LoaderAutoLogin.cpp"
    "LoaderAutoLoginImGui.cpp"
    "Crashpad.cpp"
    "ImGui.cpp"
    "imgui_backend/imgui_engine.cpp"
    "imgui_backend/imgui_impl_dx11.cpp"
    "imgui_backend/imgui_impl_win32.cpp"
    "MacroQuest.cpp"
    "PostOffice.cpp"
    "PostOfficeImGui.cpp"
    "ProcessList.cpp"
    "ProcessMonitor.cpp"
    "RemoteOps.cpp"
    "Utility.cpp"
    "WinToastLib.cpp"
)

source_groups("Source Files" ${MacroQuest_SOURCES})

# ---------------------------------------------------------------------
# Target definition
# ---------------------------------------------------------------------
add_executable(MacroQuest
    ${MacroQuest_HEADERS}
    ${MacroQuest_OTHERFILES}
    ${MacroQuest_SOURCES}
)

set_target_properties(MacroQuest PROPERTIES FOLDER "core/applications")

# ---------------------------------------------------------------------
# Apply props file configurations
# ---------------------------------------------------------------------
target_Common_props(MacroQuest)

# ---------------------------------------------------------------------
# Link vcpkg dependencies
# ---------------------------------------------------------------------
target_link_libraries(MacroQuest PRIVATE
    crashpad::crashpad
    spdlog::spdlog
    WIL::WIL
    date::date
    curl::curl
    cpr::cpr
    pe-parse::pe-parse
    protobuf::libprotobuf
)

# ---------------------------------------------------------------------
# Project dependencies
# ---------------------------------------------------------------------
add_dependencies(MacroQuest imgui login routing)

target_link_libraries(MacroQuest PRIVATE imgui login routing)

# ---------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------
target_include_directories(MacroQuest PRIVATE
    "${VCPKG_IncludeStatic}/crashpad"
    "${CMAKE_SOURCE_DIR}/include"
)

# ---------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------
target_compile_options(MacroQuest PRIVATE
    "$<$<CONFIG:Release>:/O2>"
    "/std:c++17"
    "$<$<CONFIG:Release>:/Oi>"
    "$<$<CONFIG:Release>:/Gy>"
)

# ---------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------
# Windows SDK libraries required by the loader (sourced from MacroQuest.vcxproj
# LoaderDependenciesRelease/Debug definitions). These are not provided by any
# find_package above so must be listed explicitly.
target_link_libraries(MacroQuest PRIVATE
    d3d11.lib
    Setupapi.lib
    imm32.lib
    Ws2_32.lib
    Wldap32.lib
    version.lib
)
# These variables (LoaderDependenciesDebug/Release) are only set if the project
# was configured from the legacy .vcxproj path; safe to include as they resolve
# to empty strings when not defined.
target_link_libraries(MacroQuest PRIVATE
    "$<$<CONFIG:Debug>:${LoaderDependenciesDebug}>"
    "$<$<CONFIG:Release>:${LoaderDependenciesRelease}>"
)

# ---------------------------------------------------------------------
# Linker options
# ---------------------------------------------------------------------
target_link_options(MacroQuest PRIVATE
    "$<$<CONFIG:Release>:/OPT:ICF>"
    "$<$<CONFIG:Release>:/OPT:REF>"
)

# ---------------------------------------------------------------------
# Character set defines
# ---------------------------------------------------------------------
target_compile_definitions(MacroQuest PRIVATE
    "_MBCS"
)

# ============================================================================
# MANUAL CONVERSION NOTES
# ============================================================================
#
# - Review and adjust as needed
