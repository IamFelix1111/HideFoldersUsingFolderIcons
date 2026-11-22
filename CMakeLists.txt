cmake_minimum_required(VERSION 3.20)
project(HideFoldersUsingFolderIcons LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(HideFoldersUsingFolderIcons WIN32 main.cpp)

target_compile_definitions(HideFoldersUsingFolderIcons PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(HideFoldersUsingFolderIcons PRIVATE /utf-8 /W3)
elseif(MINGW)
    # wWinMain entry + UNICODE
    target_compile_options(HideFoldersUsingFolderIcons PRIVATE -municode -Wall -Wextra)
    target_link_options(HideFoldersUsingFolderIcons PRIVATE -municode)
endif()

target_link_libraries(HideFoldersUsingFolderIcons PRIVATE user32 gdi32 comctl32 shell32 dwmapi ole32 windowscodecs)
