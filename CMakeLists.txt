cmake_minimum_required(VERSION 3.20)
project(wwe2k19_30fps_cinematic CXX)

set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.3)
FetchContent_MakeAvailable(minhook)

add_library(wwe2k19_30fps SHARED src/dllmain.cpp)
target_link_libraries(wwe2k19_30fps PRIVATE minhook d3d11 dxgi)
# .asi works with Ultimate ASI Loader; rename to .dll if you use an injector
set_target_properties(wwe2k19_30fps PROPERTIES SUFFIX ".asi")
