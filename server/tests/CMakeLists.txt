# 服务端单元测试。定时器与缓冲区是纯逻辑，可脱离网络精确验证。
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "server/tests: 未找到系统 GTest，跳过服务端测试构建")
    return()
endif()
include(GoogleTest)
set(_server_tests
    timer_queue
    buffer
    conn_pool
    graph
    planner
    compute_pool
    json_codec
)
foreach(_t ${_server_tests})
    add_executable(test_${_t} test_${_t}.cpp)
    target_link_libraries(test_${_t} PRIVATE thgh_server_core GTest::gtest_main)
    gtest_discover_tests(test_${_t} DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 120)
endforeach()
