# Linux Epoll HTTP Server

**在线演示**：http://ost101.cn （阿里云 ECS · Ubuntu 24.04）

使用 C 语言从底层实现的 Linux HTTP/1.1 静态文件服务器，通过单线程事件循环管理并发连接，重点实践 `epoll`、非阻塞 I/O、连接状态机和 `sendfile` 文件传输。

## 技术栈

| 模块 | 技术 |
| --- | --- |
| 系统语言 | C11 |
| 网络编程 | Linux Socket API、TCP、非阻塞 I/O |
| I/O 多路复用 | `epoll`、Level Triggered |
| 文件传输 | `sendfile` |
| 应用协议 | HTTP/1.0、HTTP/1.1 |
| 存储 | Linux 文件系统，无数据库 |
| 构建与测试 | GCC、GNU Make、Bash、curl |

## 核心功能

- 支持 `GET`、`HEAD` 请求和常见 HTTP 状态码。
- 支持 `POST` 上传：原始请求体流式写入 `uploads/` 目录，限制 64 MiB，返回 `201`/`411`/`413`/`501`。
- 支持配置文件加载与 `SIGHUP` 热加载（运行中重载端口/根目录/日志路径/超时，失败回退旧配置）。
- 支持 CGI：`/cgi-bin/` 下的可执行脚本通过 `fork`/`exec`/`pipe` 运行，环境变量与 stdin 传递请求，stdout 作为响应返回。
- 提供静态文件、MIME 类型识别、目录浏览和 `index.html` 优先返回。
- 实现 HTTP/1.0 与 HTTP/1.1 Keep-Alive 语义及空闲连接超时清理。
- 支持单段 Range 请求：`bytes=N-M`、`bytes=N-`、`bytes=-N`，返回 `206` 或 `416`。
- 支持 TCP 半包、请求缓冲、部分读取、部分发送和同连接连续请求。
- 记录访问日志和错误日志，响应 `SIGINT`、`SIGTERM` 完成安全退出。
- 对 URL 进行解码和 HTML 转义，并防止 `..` 路径穿越及符号链接越界访问。

### epoll 事件循环

服务器只创建一个 `epoll` 实例。监听 socket 和客户端 socket 均设置为非阻塞模式，并注册到同一个事件循环：

```text
socket -> bind -> listen -> epoll_wait
                          |
                          +-> accept4(SOCK_NONBLOCK)
                          +-> EPOLLIN  -> recv / HTTP parse
                          +-> EPOLLOUT -> send / sendfile
```

每个客户端由独立的 `Connection` 结构保存文件描述符、读缓冲区、请求对象、响应进度、文件偏移和最后活跃时间。`epoll_event.data.ptr` 直接关联连接对象，事件返回后无需再次通过文件描述符查表。

当前实现采用 Level Triggered 模式。`accept4()` 和 `recv()` 仍会循环处理到 `EAGAIN`，及时消费当前已就绪的数据；当发送缓冲区暂时写满时保留进度，等待下一次 `EPOLLOUT` 后继续。

### 非阻塞连接状态机

TCP 不保证一次 `recv()` 得到完整 HTTP 请求，也不保证一次 `send()` 发完响应。服务器显式维护以下状态：

```text
CONN_READING
    -> CONN_READING_BODY（POST 上传接收）
    -> CONN_WRITING_HEADER
    -> CONN_WRITING_MEMORY
    -> CONN_WRITING_FILE
    -> CONN_READING（Keep-Alive）或关闭
```

- 读取阶段持续累积字节，直到检测到 `\r\n\r\n` 再解析请求。
- POST 阶段按 `Content-Length` 把请求体边收边写盘，超出部分保留在缓冲区供下一个请求解析。
- 响应头、内存响应体和文件正文分别保存已发送位置。
- `send()` 或 `sendfile()` 返回 `EAGAIN` 时不阻塞线程，也不丢失传输进度。
- Keep-Alive 响应完成后重置请求与响应状态；缓冲区中若已有下一条请求，可立即继续解析。
- 主循环每秒从 `epoll_wait` 超时返回一次，扫描并关闭超过配置时限的空闲连接。

### sendfile 文件传输

静态文件正文通过：

```c
sendfile(client_fd, file_fd, &file_offset, block_size);
```

直接从文件描述符传输到 socket，避免先 `read()` 到用户态缓冲区再调用 `send()`。这减少了用户态数据拷贝和系统调用组织成本，也是服务器处理大文件时的关键优化。

Range 请求会先计算实际的闭区间，随后用 `file_offset` 和 `file_remaining` 记录起始位置与剩余字节数；每次最多提交 1 MiB，实际进度以 `sendfile()` 返回值为准。

### HTTP 与安全处理

- 解析请求行、`Connection`、`Range` 和 `Content-Length` 请求头，拒绝不支持的方法和版本。
- 上传仅限 `/uploads/` 子目录，规范化父目录并校验仍在网站根目录内，失败时删除残留文件。
- 使用 `realpath()` 规范化文件路径，并验证最终路径仍位于网站根目录。
- 即使网站目录中的符号链接指向外部路径，也不能借此读取任意系统文件。
- 动态目录页在输出文件名时进行 HTML 转义，在链接中进行 URL 编码。
- 忽略 `SIGPIPE` 并在 `send()` 中使用 `MSG_NOSIGNAL`，防止单个客户端中途断开导致整个进程退出。

## 个人工作

本项目由本人独立设计和实现，主要工作包括：

- 从 `socket`、`bind`、`listen`、`accept4` 开始搭建完整 TCP 服务端。
- 设计单线程 `epoll` Reactor 事件循环和每连接状态管理模型。
- 实现非阻塞读写、TCP 半包处理、部分发送恢复和 Keep-Alive 连接复用。
- 编写 HTTP 请求解析、Range 计算、MIME 判断、目录响应和错误响应逻辑。
- 使用 `sendfile` 优化静态文件传输，并处理大文件分段发送与发送缓冲区背压。
- 实现路径规范化、目录越界防护、日志、信号退出和空闲连接回收。
- 实现 `POST` 上传状态机：请求体流式落盘、大小限制、上传目录越界防护与失败残留清理。
- 实现配置文件解析与 `SIGHUP` 热加载：运行中重载端口、根目录和日志路径，失败时保留旧配置。
- 实现 CGI 支持：`fork`/`exec`/`pipe` 运行外部脚本，管道接入 epoll 事件循环，子进程超时与输出上限控制。
- 编写自动化脚本验证状态码、Range、路径穿越、同连接连续请求和并发连接。

## 本地运行

### 环境要求

- Linux 或 WSL
- GCC
- GNU Make
- Bash
- curl

Ubuntu/WSL 安装依赖：

```bash
sudo apt update
sudo apt install build-essential curl
```

### 编译与启动

```bash
make
./minihttpd
```

默认监听 `8080` 端口，以 `./www` 为网站根目录，Keep-Alive 超时为 10 秒。

自定义启动参数：

```bash
./minihttpd -p 9000 -r ./www -t 15
```

| 参数 | 含义 |
| --- | --- |
| `-p` | 监听端口，范围 1 到 65535 |
| `-r` | 静态文件根目录 |
| `-t` | Keep-Alive 超时秒数 |
| `-c` | 配置文件路径，默认 `./minihttpd.conf` |
| `-h` | 显示帮助 |

浏览器访问 `http://127.0.0.1:8080/`，按 `Ctrl+C` 关闭服务器。

### 配置文件与热加载

配置项优先级：命令行参数 > 配置文件 > 默认值。示例 `minihttpd.conf`：

```ini
port 8080
document_root ./www
access_log ./logs/access.log
error_log ./logs/error.log
keepalive_timeout 10
```

运行中修改配置文件后发送 `SIGHUP` 热加载：

```bash
kill -HUP $(pgrep minihttpd)
```

`port`、`document_root`、日志路径和超时均支持热加载；端口变化会重建监听 socket，已有连接不受影响。新配置无效（解析失败、根目录不存在、端口被占用）时保留旧配置继续运行。

### CGI 支持

`/cgi-bin/` 目录下的可执行脚本通过 `fork`/`exec` 运行。请求信息经环境变量（`REQUEST_METHOD`、`QUERY_STRING`、`SCRIPT_NAME`、`CONTENT_LENGTH`、`CONTENT_TYPE` 等）传给脚本，POST 请求体经 stdin 传入，脚本 stdout 作为响应返回（可输出 `Status:` 和 `Content-Type:` 响应头）：

```bash
chmod +x www/cgi-bin/*.sh
curl 'http://127.0.0.1:8080/cgi-bin/hello.sh?name=tom'
curl --data-binary 'ping' http://127.0.0.1:8080/cgi-bin/echo.sh
```

限制：请求体 ≤ 64 KiB、脚本输出 ≤ 1 MiB（超出终止子进程）、执行超过 5 秒强杀并返回 `504`。脚本 stdout 管道以非阻塞方式接入 epoll 事件循环，不会阻塞其他连接。

### 测试

```bash
make test
```

自动测试覆盖：

- 首页、`HEAD`、404、目录列表和 Range 响应。
- `POST` 上传回读校验、`411`/`413` 校验、上传目录限制。
- 配置热加载：改端口生效、非法配置回退。
- CGI：环境变量传递、POST stdin 回显、404/403 校验、超时 504。
- `../etc/passwd` 路径穿越拒绝。
- 一个 TCP 连接内连续发送两条 HTTP 请求（含 POST 后紧跟 GET）。
- 40 个请求、8 路并发的多连接基本回归测试。

调试内存问题时可使用 AddressSanitizer/UndefinedBehaviorSanitizer 构建：

```bash
make debug
```

## 项目结构

```text
linux-epoll-http-server/
├── include/
│   ├── common.h       配置、请求和连接状态结构
│   ├── config.h       配置文件解析接口
│   ├── http.h         HTTP 解析与编码接口
│   ├── logger.h       日志接口
│   └── server.h       服务器入口
├── src/
│   ├── main.c         参数解析与初始化
│   ├── config.c       配置文件解析
│   ├── server.c       socket、epoll、状态机和响应发送
│   ├── http.c         请求、Range、URL 与 MIME 处理
│   └── logger.c       访问日志与错误日志
├── tests/test.sh      HTTP 自动化回归测试
├── www/               示例网站根目录（含 cgi-bin 脚本示例）
├── minihttpd.conf     示例配置（SIGHUP 热加载）
└── Makefile           编译、调试和测试命令
```

## 设计边界

当前版本不包含 multipart 表单解析、TLS、线程池和完整 RFC 语法校验。项目用于学习 Linux 网络编程，不建议未经资源限制、权限隔离和安全审计直接部署到公网。
