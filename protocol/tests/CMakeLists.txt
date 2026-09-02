# 协议层单元测试。重点：粘包 / 半包的逐字节验证，以及长度上限保护。
#
# ⚠️ Windows 上**跳过** find_package：踩过一次坑 —— 开发机装了 Anaconda，
#    它的 Library/lib/cmake/GTest 会被 CMake 找到（GTest_DIR 指过去），
#    但那是 **MSVC 编译的 .lib**，用 MinGW 链接时会炸出一屏
#    "undefined reference to testing::Message::Message()"。
#    Windows 上本来也没有可用的系统 GTest，直接跳过最稳
#    —— 协议层测试在 Linux 侧（与服务端同构建）跑，覆盖不受影响。
if(WIN32)
    message(STATUS "protocol/tests: Windows 上跳过（系统 GTest 不可用，见本文件注释）")
    return()
endif()

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
