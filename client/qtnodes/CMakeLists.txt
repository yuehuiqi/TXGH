# ─────────────────────────────────────────────────────────────────────────────
# QtNodes (nodeeditor) —— 以静态库方式集成
#
# 来源: https://github.com/paceholder/nodeeditor (master)
# 仅保留 src / include / resources，去掉 examples / test / docs。
#
# 这里刻意不用上游自带的 CMakeLists：上游那套带了 examples、测试、安装规则和
# 版本探测，对我们只是噪音。自己写一份最小的静态库定义，构建更快也更可控。
# ─────────────────────────────────────────────────────────────────────────────

# 用 GLOB 收集源码：这批文件来自第三方且我们不会逐个增删，
# 用显式列表反而会在同步上游时漏文件。CONFIGURE_DEPENDS 让 CMake 在
# 文件增减时自动重新配置，避免"加了文件却没编译"的经典坑。
file(GLOB QTNODES_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB QTNODES_HEADERS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/QtNodes/internal/*.hpp")

add_library(qtnodes STATIC
    ${QTNODES_SOURCES}
    ${QTNODES_HEADERS}
    resources/resources.qrc
)

# PUBLIC：使用方（客户端主程序）也需要这些头文件路径与宏
target_include_directories(qtnodes PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/QtNodes/internal"
)

# 静态集成，关闭 dll 导入导出（见 include/QtNodes/internal/Export.hpp）
target_compile_definitions(qtnodes PUBLIC NODE_EDITOR_STATIC)

target_link_libraries(qtnodes PUBLIC
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::OpenGL
)

target_compile_features(qtnodes PUBLIC cxx_std_17)

# 第三方代码不开我们自己的严格告警：它的告警我们既不该改也修不完，
# 混在输出里只会淹没本项目自己的告警。
if(MSVC)
    target_compile_options(qtnodes PRIVATE /W0)
else()
    target_compile_options(qtnodes PRIVATE -w)
endif()
