# Linux Epoll HTTP Server

`minihttpd` 是一个使用 C 语言实现、运行于 Linux 的轻量级 HTTP/1.1 静态文件服务器。项目基于非阻塞 socket、`epoll` 和事件驱动状态机管理并发连接，重点实践 Linux 网络编程、HTTP 协议解析与安全文件访问。

## 已实现功能

- `GET` 和 `HEAD` 请求
- 静态文件服务与 MIME 类型判断
- 目录浏览，目录内存在 `index.html` 时优先返回首页
- HTTP/1.0 与 HTTP/1.1 Keep-Alive 语义
- 单段 Range：`bytes=N-M`、`bytes=N-`、`bytes=-N`
- `sendfile()` 零拷贝发送文件
- 非阻塞部分读取、部分发送和请求缓冲
- Keep-Alive 空闲连接超时清理
- 访问日志与错误日志
- URL 解码、HTML 转义、`..` 和符号链接越界防护
- `SIGINT`、`SIGTERM` 安全退出

当前核心版不包含 POST、文件上传、CGI、线程池、配置文件和热加载。这些属于下一阶段扩展。

## 环境与编译

需要 Linux、GCC、GNU Make 和 curl。Ubuntu 可安装：

```bash
sudo apt update
sudo apt install build-essential curl
```

进入项目目录后编译：

```bash
make
```

严格编译选项包含 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`。调试内存问题时可使用：

```bash
make debug
```

## 运行

```bash
./minihttpd
```

默认监听 `8080` 端口，以 `./www` 为网站根目录，Keep-Alive 超时为 10 秒。

```bash
./minihttpd -p 9000 -r ./www -t 15
```

参数：

| 参数 | 含义 |
|---|---|
| `-p` | 监听端口，范围 1 到 65535 |
| `-r` | 网站根目录 |
| `-t` | Keep-Alive 超时秒数 |
| `-h` | 显示帮助 |

浏览器访问 `http://127.0.0.1:8080/`。按 `Ctrl+C` 关闭服务器。

## 测试

```bash
make test
```

自动测试覆盖首页、HEAD、404、目录列表、Range、路径穿越、同连接连续请求和并发请求。也可以手动执行：

```bash
curl -i http://127.0.0.1:8080/
curl -I http://127.0.0.1:8080/sample.txt
curl -i -H 'Range: bytes=0-9' http://127.0.0.1:8080/sample.txt
curl -i http://127.0.0.1:8080/downloads/
```

日志保存在：

```text
logs/access.log
logs/error.log
```

## 程序结构

```text
http_server/
├── include/
│   ├── common.h       公共数据结构和常量
│   ├── http.h         HTTP 解析与编码接口
│   ├── logger.h       日志接口
│   └── server.h       服务器接口
├── src/
│   ├── main.c         参数解析和初始化
│   ├── server.c       socket、epoll、连接状态机和响应
│   ├── http.c         请求、Range、URL 和 MIME 处理
│   └── logger.c       访问日志和错误日志
├── tests/test.sh      自动测试
├── www/               示例网站根目录
└── Makefile
```

## 核心流程

服务器启动遵循课堂中的 socket 编程流程：

```text
socket -> setsockopt -> bind -> listen -> epoll_wait
                                      |
                                      +-> accept4 -> recv -> parse
                                                   -> send/sendfile
                                                   -> close or keep-alive
```

每个客户端由一个 `Connection` 结构保存状态：

```text
CONN_READING
    -> CONN_WRITING_HEADER
    -> CONN_WRITING_MEMORY 或 CONN_WRITING_FILE
    -> CONN_READING（Keep-Alive）或关闭
```

非阻塞 socket 的 `recv()`、`send()` 和 `sendfile()` 可能一次只处理部分数据。程序保存已经处理的位置，等待下一次 `epoll` 事件后继续，不能假设一次系统调用完成全部工作。

## 建议阅读顺序

1. `src/main.c`：理解参数、网站根目录和日志初始化。
2. `src/server.c` 中的 `create_listener()`：对应课堂 socket 服务端流程。
3. `server_run()` 与 `accept_connections()`：理解 `epoll` 如何管理连接。
4. `handle_read()` 和 `http_parse_request()`：理解 TCP 半包与 HTTP 请求解析。
5. `prepare_request()`、`prepare_file()`、`prepare_directory()`：理解资源映射。
6. `handle_write()` 和 `finish_response()`：理解部分发送与 Keep-Alive。
7. `tests/test.sh`：对照每项功能观察真实 HTTP 行为。

## 安全边界

请求路径先进行 URL 解码，再拒绝 `..` 路径段。文件通过 `realpath()` 得到规范路径，并验证结果仍位于网站根目录，因此网站目录内指向外部的符号链接也不能用于读取任意文件。

本项目用于学习，不应直接部署到公网。生产服务器还需要更完整的 HTTP 语法验证、资源限制、权限隔离、TLS 和安全审计。
