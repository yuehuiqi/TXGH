# ─────────────────────────────────────────────────────────────────────────────
# TXGH 客户端 —— Qt 图形界面
#
# 由 qmake(TXGH.pro) 迁移而来。迁移理由：
#   1. 服务端在 Linux 上用 CMake 构建，客户端跟着统一，一套构建体系
#   2. CMake 才能接 CI（GitHub Actions）、ctest、Sanitizer 开关
#   3. protocol/ 是两端共享的库，用 CMake 才能被两边干净地引用
#
# 全局设置（C++ 标准、构建类型、Sanitizer）在顶层 CMakeLists.txt 里统一定义，
# 本文件只管客户端自己的事。
# ─────────────────────────────────────────────────────────────────────────────

# Qt 的三件套代码生成：moc（信号槽）、uic（.ui → ui_*.h）、rcc（.qrc）
# 只在本目录作用域打开，不影响服务端等其它子项目
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS
    Core Gui Widgets Sql Charts Network Svg OpenGL
)

# ── 第三方：QtNodes 节点编辑器 ──────────────────────────────────────────────
add_subdirectory(qtnodes)

# ── 客户端主程序 ────────────────────────────────────────────────────────────
# 这里用显式文件列表而不是 GLOB：本项目的源码是我们自己维护的，
# 显式列表能让"新增文件忘了加进构建"在 code review 时就暴露出来。
set(TXGH_SOURCES
    main.cpp
    mainwindow.cpp
    dbmanager.cpp
    modelcodec.cpp
    remotedatastore.cpp
    devicetopologyview.cpp
    dialoglink.cpp
    dialognode.cpp
    dialogplanning.cpp
    dialogscene.cpp
    framelessdialog.cpp
    simbridge.cpp
    topoview.cpp
)

set(TXGH_HEADERS
    datamodel.h
    dbmanager.h
    idatastore.h
    modelcodec.h
    remotedatastore.h
    devicetopologyview.h
    dialoglink.h
    dialognode.h
    dialogplanning.h
    dialogscene.h
    framelessdialog.h
    mainwindow.h
    simbridge.h
    tableutils.h
    topoview.h
)

set(TXGH_FORMS
    dialoglink.ui
    dialognode.ui
    dialogplanning.ui
    dialogscene.ui
    mainwindow.ui
)

# 注意：dialogdevice.{cpp,h,ui} 存在于源码目录，但原 TXGH.pro 并未将其纳入构建，
# 属于未被编译的历史遗留文件。这里保持与 qmake 行为一致不纳入，
# 待确认无用后再删除，避免本次构建迁移引入行为变化。
# （运行时那条 "No matching signal for on_btnDeviceDetails_clicked()" 警告
#   即源于此——.ui 里留了按钮，但对应的槽实现没被编译进来。）

add_executable(TXGHClient
    ${TXGH_SOURCES}
    ${TXGH_HEADERS}
    ${TXGH_FORMS}
    res.qrc
)

target_link_libraries(TXGHClient PRIVATE
    # 协议共享层。之前客户端没有链它，消息类型和字段名在 simbridge.cpp 里
    # 是一堆字符串字面量 —— 那正是 protocol/thgh/message.h 开头警告的情况：
    # 一端写 "plan_result"、另一端写 "plan_results"，编译器不报错、
    # 两边单测各自也都能过，只有联调时才发现，症状还是"发出去了但对面没反应"。
    # v3.0 新增 ack/progress 两种消息，重复的字面量更多了，这次把它收口。
    thgh_protocol
    qtnodes
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Sql
    Qt5::Charts
    Qt5::Network
    Qt5::Svg
)

# AUTOUIC 生成的 ui_*.h 落在构建目录，源码里写的是 #include "ui_mainwindow.h"，
# 需要把构建目录加进搜索路径
target_include_directories(TXGHClient PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Windows 下发布时不弹控制台窗口；调试期保留控制台方便看 qDebug 输出
option(TXGH_CLIENT_CONSOLE "保留控制台窗口以查看调试输出" ON)
if(WIN32 AND NOT TXGH_CLIENT_CONSOLE)
    set_target_properties(TXGHClient PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# 严格告警：只对我们自己的代码开，第三方库已在其 CMakeLists 里关掉
if(MSVC)
    target_compile_options(TXGHClient PRIVATE /W4)
else()
    target_compile_options(TXGHClient PRIVATE -Wall -Wextra)
endif()
