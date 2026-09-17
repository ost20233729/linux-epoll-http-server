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
/* 单个上传文件的大小上限（64 MiB）。 */
#define MAX_UPLOAD_SIZE (64 * 1024 * 1024)
/* CGI 请求体缓冲上限（64 KiB，等于 Linux 管道容量，父进程可一次性写完不阻塞）。 */
#define CGI_BODY_MAX (64 * 1024)
/* CGI 输出缓冲上限（1 MiB），防止失控脚本耗尽服务器内存。 */
#define CGI_OUTPUT_MAX (1024 * 1024)
/* CGI 脚本最长执行时间（秒），超时强杀子进程并返回 504。 */
#define CGI_TIMEOUT 5

/* 请求方法；POST 用于上传文件，其余未支持的方法统一返回 405。 */
typedef enum
{
    METHOD_GET,
    METHOD_HEAD,
    METHOD_POST,
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

    /* 请求体：POST 上传的数据；其他方法携带请求体按错误处理。 */
    bool has_content_length; /* 是否带有 Content-Length 请求头。 */
    bool has_body;           /* Content-Length 是否大于 0。 */
    bool chunked;            /* 是否为 chunked 传输编码（暂不支持）。 */
    off_t content_length;    /* 请求体长度（字节）。 */
    char content_type[128];  /* Content-Type 请求头，透传给 CGI 环境变量。 */
} HttpRequest;

/*
 * 一个连接在事件循环中的处理阶段。
 * epoll 只通知“可读/可写”，具体该读请求还是写文件由该状态决定。
 */
typedef enum
{
    CONN_READING,// 正在读取 HTTP 请求头
    CONN_READING_BODY,// 正在接收 POST 请求体（上传写文件 / CGI 缓冲进内存）
    CONN_RUNNING_CGI,// CGI 子进程运行中，等待其 stdout 管道输出
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

    /* 请求读取缓冲区：read_length 表示已经收到的字节数。
       接收 POST 请求体时，缓冲区同时用作传输中转。 */
    char read_buffer[READ_BUFFER_SIZE];
    size_t read_length;

    /* POST 请求体接收进度：边收边写入上传文件，不整体缓存进内存。 */
    off_t content_received;     /* 已收到的请求体字节数。 */
    int upload_fd;              /* 上传目标文件描述符，-1 表示未打开。 */
    char upload_path[PATH_MAX]; /* 上传文件绝对路径，失败时用于删除残留。 */

    /* CGI 执行状态：/cgi-bin/ 下的脚本通过 fork/exec 运行。 */
    int cgi_pid;                /* CGI 子进程 PID，-1 表示当前没有运行。 */
    int cgi_input_fd;           /* 子进程 stdin 的写端（POST 请求体），-1 表示无。 */
    int cgi_output_fd;          /* 子进程 stdout 的读端，加入 epoll 等待输出。 */
    time_t cgi_started;         /* 子进程启动时间，用于超时终止。 */
    int cgi_status;             /* CGI 响应头中的 Status，默认 200。 */
    char cgi_content_type[64];  /* CGI 响应头中的 Content-Type。 */
    bool cgi_request;           /* 请求体是否属于 CGI（缓冲进内存而非写文件）。 */
    char *request_body;         /* CGI 请求体缓冲（≤ 64 KiB）。 */
    size_t request_body_length; /* 已缓冲的请求体字节数。 */

    /* HTTP 响应头及其发送进度。 */
    char header_buffer[HEADER_BUFFER_SIZE];
    size_t header_length;
    size_t header_sent;

    /* 动态生成的页面或 CGI 输出缓冲及其发送进度。 */
    char *body;
    size_t body_length;
    size_t body_sent;
    size_t body_capacity; /* body 缓冲区的已分配容量（CGI 增量追加时用）。 */

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
