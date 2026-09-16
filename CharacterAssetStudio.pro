QT += widgets
CONFIG += c++17 windows
TEMPLATE = app
TARGET = CharacterAssetStudio

isEmpty(AES_ROOT): AES_ROOT = $$PWD/AES
SOURCES += main.cpp $$AES_ROOT/src/AES.cpp
HEADERS += $$AES_ROOT/src/AES.h
INCLUDEPATH += $$AES_ROOT/src
INCLUDEPATH += $$PWD/zstd-v1.5.7-win64/include
INCLUDEPATH += $$PWD/lz4_win64_v1_10_0/include
LIBS += $$PWD/zstd-v1.5.7-win64/dll/libzstd.dll.a
LIBS += $$PWD/lz4_win64_v1_10_0/static/liblz4_static.lib

