# Makefile for epoll_server project

# 编译器
CXX := g++
CXXFLAGS := -std=c++17 -O2 -pthread -Iinclude -Wall -Wextra

# 目标文件
SERVER := server
CLIENT := client

# 源文件
SERVER_SRC := server.cpp
CLIENT_SRC := client.cpp

# 默认目标
all: $(SERVER) $(CLIENT)

# 编译 server
$(SERVER): $(SERVER_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

# 编译 client
$(CLIENT): $(CLIENT_SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

# 清理
clean:
	rm -f $(SERVER) $(CLIENT)

# 伪目标
.PHONY: all clean