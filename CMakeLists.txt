cmake_minimum_required(VERSION 3.20)
project(BirthdayAdventure LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ---- GLFW, GLM, FreeType (downloaded and built automatically) ----
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git GIT_TAG VER-2-13-2)
FetchContent_MakeAvailable(glfw glm freetype)

# ---- single-header libraries: stb and miniaudio ----
# The code includes <stb/stb_image.h> and <miniaudio/miniaudio.h>, so copy the
# headers into a folder layout that matches.
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master)
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG master)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
  FetchContent_Populate(stb)
endif()
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
  FetchContent_Populate(miniaudio)
endif()
set(DEPS_INC ${CMAKE_BINARY_DIR}/deps_include)
file(COPY ${stb_SOURCE_DIR}/stb_image.h ${stb_SOURCE_DIR}/stb_image_write.h DESTINATION ${DEPS_INC}/stb)
file(COPY ${miniaudio_SOURCE_DIR}/miniaudio.h DESTINATION ${DEPS_INC}/miniaudio)

# ---- glad (OpenGL loader) ----
# Generate once at https://gen.glad.sh : Language C/C++, Specification OpenGL,
# API gl 3.3, Profile Core, Generator C/C++ (glad2). Unzip into third_party/glad
# so that third_party/glad/include/glad/gl.h and third_party/glad/src/gl.c exist.
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/glad/src/gl.c)
  message(FATAL_ERROR "glad is missing. See the comment above this line in CMakeLists.txt.")
endif()
add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

# ---- the game ----
add_executable(birthday_adventure
  main.cpp Audio.cpp Atlas.cpp Common.cpp Config.cpp Entities.cpp Renderer.cpp World.cpp)
target_include_directories(birthday_adventure PRIVATE ${CMAKE_SOURCE_DIR} ${DEPS_INC})
target_link_libraries(birthday_adventure PRIVATE glad glfw glm::glm freetype)
if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(birthday_adventure PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(WIN32)
  target_compile_definitions(birthday_adventure PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Copy the config and assets next to the executable so the game finds its root folder.
foreach(item assets shaders)
  if(EXISTS ${CMAKE_SOURCE_DIR}/${item})
    add_custom_command(TARGET birthday_adventure POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_SOURCE_DIR}/${item} $<TARGET_FILE_DIR:birthday_adventure>/${item}
      COMMENT "Copying ${item}")
  endif()
endforeach()
if(EXISTS ${CMAKE_SOURCE_DIR}/birthday_config.json)
  add_custom_command(TARGET birthday_adventure POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/birthday_config.json $<TARGET_FILE_DIR:birthday_adventure>/birthday_config.json)
endif()
