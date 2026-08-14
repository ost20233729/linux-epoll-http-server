#ifndef MINIHTTPD_COMMON_H
#define MINIHTTPD_COMMON_H
#define _GNU_SOURCE
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/* 服务器名称、缓冲区大小和默认运行参数。 */
#define SERVER_NAME "minihttpd/1.0"
#define READ_BUFFER_SIZE 32768
#define HEADER_BUFFER_SIZE 4096
#define MAX_EVENTS 128
#define MAX_CONNECTIONS 4096
#define DEFAULT_PORT 8080
#define DEFAULT_KEEPALIVE_TIMEOUT 10

/* 当前核心版本只实现 GET 和 HEAD，其他方法统一返回 405。 */
typedef enum
{
    METHOD_GET,
    METHOD_HEAD,
    METHOD_UNSUPPORTED
} HttpMethod;

/* HTTP 请求解析后的结构化结果，不再直接操作原始请求字符串。 */
typedef struct
{
    HttpMethod method;     /* 请求方法。 */
    char target[PATH_MAX]; /* URL 中的请求目标，例如 /index.html。 */
    int http_minor;        /* HTTP/1.0 为 0，HTTP/1.1 为 1。 */
    bool keep_alive;       /* 响应结束后是否复用 TCP 连接。 */
    bool has_range;        /* 是否存在 Range 请求头。 */
    bool range_suffix;     /* 是否为 bytes=-N 形式的后缀范围。 */
    off_t range_start;     /* 起始位置；后缀范围中表示末尾字节数。 */
    off_t range_end;       /* 结束位置，-1 表示直到文件末尾。 */
} HttpRequest;

/*
 * 一个连接在事件循环中的处理阶段。
 * epoll 只通知“可读/可写”，具体该读请求还是写文件由该状态决定。
 */
typedef enum
{
    CONN_READING,// 正在读取 HTTP 请求
    CONN_WRITING_HEADER,// 正在发送响应头
    CONN_WRITING_MEMORY,
    CONN_WRITING_FILE
} ConnectionState;

/*
 * 每个客户端对应一个 Connection，保存跨多次 epoll 事件的处理进度。
 * 非阻塞 recv/send/sendfile 可能只完成一部分，因此必须保留各类偏移量。
 */
typedef struct Connection
{
    /* 客户端和当前状态。 */
    int fd;
    ConnectionState state;
    char client_ip[64];

    /* 请求读取缓冲区：read_length 表示已经收到的字节数。 */
    char read_buffer[READ_BUFFER_SIZE];
    size_t read_length;

    /* HTTP 响应头及其发送进度。 */
    char header_buffer[HEADER_BUFFER_SIZE];
    size_t header_length;
    size_t header_sent;

    /* 动态生成的目录页面或错误页面及其发送进度。 */
    char *body;
    size_t body_length;
    size_t body_sent;

    /* 静态文件描述符、下一次发送位置和剩余字节数。 */
    int file_fd;
    off_t file_offset;
    off_t file_remaining;

    /* 连接控制、日志信息和当前请求。 */
    bool keep_alive;
    bool close_after_response;
    time_t last_active;
    int status_code;
    off_t response_bytes;
    HttpRequest request;
} Connection;

/* 由 main() 准备并传入服务器事件循环的运行配置。 */
typedef struct
{
    int port;
    char document_root[PATH_MAX];
    char access_log[PATH_MAX];
    char error_log[PATH_MAX];
    int keepalive_timeout;
} ServerConfig;
#endif
