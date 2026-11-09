# MiniVectorDB —— 多阶段构建，运行期只依赖 glibc。
# 构建：docker build -t minivectordb .
# 运行：docker run -i --rm -v "$PWD/data:/data" minivectordb
ARG BASE=ubuntu:22.04

FROM ${BASE} AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && DESTDIR=/stage cmake --install build --prefix /usr/local

FROM ${BASE} AS runtime
COPY --from=build /stage/usr/local /usr/local
RUN mkdir -p /data \
 && ldd /usr/local/bin/minivectordb \
 && /usr/local/bin/minivectordb --help >/dev/null 2>&1 || true
# 所有数据、日志与索引文件都在 /data 下；前台运行，仅用 stdin/stdout 通信。
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir=/data"]
