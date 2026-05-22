# Makefile для userspace-утилиты myfs_tool.
#
# Сборка обычным g++ (C++17). Заголовок myfs.h берётся из текущего каталога
# (это копия общего заголовка модуля).
#
#   make         — собрать myfs_tool
#   make clean   — удалить артефакты

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

TARGET := myfs_tool
SRC    := myfs_tool.cpp

all: $(TARGET)

$(TARGET): $(SRC) myfs.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
