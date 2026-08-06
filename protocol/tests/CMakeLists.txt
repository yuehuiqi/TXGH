# 协议层单元测试。重点：粘包 / 半包的逐字节验证，以及长度上限保护。
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "protocol/tests: 未找到系统 GTest，跳过协议测试构建")
    return()
endif()

include(GoogleTest)

set(_protocol_tests
    line_framer
    message
)

foreach(_t ${_protocol_tests})
    add_executable(test_${_t} test_${_t}.cpp)
    target_link_libraries(test_${_t} PRIVATE thgh_protocol GTest::gtest_main)
    gtest_discover_tests(test_${_t} DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 120)
endforeach()
