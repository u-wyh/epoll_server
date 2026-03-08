# 基础镜像
FROM ubuntu:22.04

# 安装依赖
RUN apt update && apt install -y g++ make nlohmann-json3-dev

# 工作目录
WORKDIR /app

# 拷贝代码
COPY server.cpp /app/

# 编译
RUN g++ server.cpp -o server -std=c++17 -pthread -O2

# 暴露端口
EXPOSE 8080

CMD ["./server"]