QT       += core gui sql charts network svg

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# QtNodes 需要 C++14 及以上，统一使用 C++17
CONFIG += c++17

# 集成 QtNodes (nodeeditor) 源码库（设备拓扑可视化）
include(qtnodes/qtnodes.pri)

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    dbmanager.cpp \
    devicetopologyview.cpp \
    dialoglink.cpp \
    dialognode.cpp \
    dialogplanning.cpp \
    dialogscene.cpp \
    framelessdialog.cpp \
    main.cpp \
    mainwindow.cpp \
    simbridge.cpp \
    topoview.cpp

HEADERS += \
    datamodel.h \
    dbmanager.h \
    devicetopologyview.h \
    dialoglink.h \
    dialognode.h \
    dialogplanning.h \
    dialogscene.h \
    framelessdialog.h \
    mainwindow.h \
    simbridge.h \
    tableutils.h \
    topoview.h

FORMS += \
    dialoglink.ui \
    dialognode.ui \
    dialogplanning.ui \
    dialogscene.ui \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    res.qrc

DISTFILES += \
    README.md \
    TXGH_Qt_Python.md
