# ---------------------------------------------------------------------------
# graph_database_server —— 单仓库整体构建
#
# 本仓库已把三个原本独立的项目合并为一个工程：
#   src/minigraph      + include/minigraph      —— 图数据库内核（原 minigraph）
#   src/http           + include/*.hpp          —— HTTP 服务器与子进程桥接（原 cpp_http_bridge）
#   src/graph_server   + include/graph_server   —— 业务层：路由 + CRUD + 算法（原 graph_server 胶水层）
# 三者不再各自独立编译，而是由本文件统一编译、统一产出。
#
# 构建： cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.14)
project(graph_database_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# 所有目标统一从这里找头文件：include/minigraph/...、include/graph_server/...、include/*.hpp
set(PROJECT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${PROJECT_INCLUDE_DIR})

# ---------------------------------------------------------------------------
# 第一层：图数据库内核（minigraph）
# ---------------------------------------------------------------------------
add_library(minigraph_core STATIC
    src/minigraph/Value.cpp
    src/minigraph/Storage.cpp
    src/minigraph/GraphDB.cpp
)
target_include_directories(minigraph_core PUBLIC ${PROJECT_INCLUDE_DIR})

# ---------------------------------------------------------------------------
# 第二层：HTTP 服务器 + 子进程桥接（cpp_http_bridge）
# ---------------------------------------------------------------------------
add_library(httpbridge_core STATIC
    src/http/http_server.cpp
    src/http/child_process.cpp
)
target_include_directories(httpbridge_core PUBLIC ${PROJECT_INCLUDE_DIR})
target_link_libraries(httpbridge_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# 第三层：业务层（图数据库服务）—— JSON<->Value 转换 + 路由 + 多图管理
# ---------------------------------------------------------------------------
add_library(graphservice_core STATIC
    src/graph_server/JsonValue.cpp
    src/graph_server/GraphService.cpp
)
target_include_directories(graphservice_core PUBLIC ${PROJECT_INCLUDE_DIR})
target_link_libraries(graphservice_core PUBLIC minigraph_core httpbridge_core)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------

# 1) 图数据库网络服务（主程序，对外暴露 HTTP CRUD 接口 + 可视化控制台）
add_executable(graph_server src/graph_server_main.cpp)
target_link_libraries(graph_server PRIVATE graphservice_core)

# 2) 控制台程序 HTTP 桥接服务
add_executable(cpp_bridge src/cpp_bridge_main.cpp)
target_link_libraries(cpp_bridge PRIVATE httpbridge_core)

# 3) 图数据库内核演示程序（含交互式 shell）
add_executable(minigraph_demo examples/minigraph_demo.cpp)
target_link_libraries(minigraph_demo PRIVATE minigraph_core)

# 4) 桥接示例：被桥接的两个控制台程序 + 嵌入式 HTTP 范例
add_executable(echo_worker      examples/echo_worker.cpp)
add_executable(calc_worker      examples/calc_worker.cpp)
add_executable(embedded_server  examples/embedded_server.cpp)
target_link_libraries(embedded_server PRIVATE httpbridge_core)

# 5) JSON 库自测
add_executable(json_tests tests/test_json.cpp)

# 统一把产物放到 bin/
set_target_properties(
    graph_server cpp_bridge minigraph_demo echo_worker calc_worker embedded_server json_tests
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin
)

# ---------------------------------------------------------------------------
# 警告开关
# ---------------------------------------------------------------------------
if(MSVC)
    set(WARNING_FLAGS /W4)
else()
    set(WARNING_FLAGS -Wall -Wextra)
endif()
foreach(_t minigraph_core httpbridge_core graphservice_core
           graph_server cpp_bridge minigraph_demo
           echo_worker calc_worker embedded_server json_tests)
    target_compile_options(${_t} PRIVATE ${WARNING_FLAGS})
endforeach()

# ---------------------------------------------------------------------------
# 测试与安装
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME json_tests COMMAND json_tests)

install(TARGETS graph_server cpp_bridge minigraph_demo RUNTIME DESTINATION bin)
install(DIRECTORY web/ DESTINATION share/graph_database_server/web)
install(DIRECTORY include/ DESTINATION include)
