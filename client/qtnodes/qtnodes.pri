# ===========================================================================
#  QtNodes (nodeeditor) —— 以源码方式静态集成到 TXGH 项目
#  来源: https://github.com/paceholder/nodeeditor (master)
#  仅保留 src / include / resources，去掉 examples/test/docs/cmake。
#  跨平台: Windows(MinGW) + Linux，Qt >= 5.11（本项目 Qt5.12）。
# ===========================================================================

QT += core gui widgets opengl

# 以静态源码方式编入，关闭 dll 导入导出（见 include/.../Export.hpp）
DEFINES += NODE_EDITOR_STATIC

# QtNodes 需要 C++14 及以上
CONFIG += c++17

INCLUDEPATH += \
    $$PWD/include \
    $$PWD/include/QtNodes/internal

SOURCES += $$files($$PWD/src/*.cpp)

HEADERS += $$files($$PWD/include/QtNodes/internal/*.hpp)

RESOURCES += $$PWD/resources/resources.qrc
