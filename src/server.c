#include "server.h"
#include "config.h"
#include "http.h"
#include "logger.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*epoll 只通知哪个 socket 可以读写，epoll实例只有一个。Connection 负责保存这个 socket 应该做什么以及已经做到哪里。*/

/*
 * 服务器采用单线程 epoll 事件循环。
 * running 由信号处理函数修改，其余全局状态只在事件循环线程中访问。
 */
static volatile sig_atomic_t running = 1; //running — 服务器的开关。初始值是 1（运行）。当用户按 Ctrl+C，内核发 SIGINT 信号，stop_server 被调用，把 running 改成 0。主循环的 while (running) 看到 0 就退出，开始清理。          volatile 告诉编译器：这个变量可能在你不知道的时候被改（信号处理函数是在正常执行流之外被调用的），每次用到它必须从内存重新读，不能用寄存器里的缓存值。
/* SIGHUP 时置 1，主循环看到后执行热加载。 */
static volatile sig_atomic_t reload_requested = 0;
/*
 * 当前生效的配置。server_config 始终指向它，热加载时整体替换内容。
 * 事件循环是单线程，替换和读取都发生在同一线程，无需加锁。
 */
static ServerConfig current_config;
static const ServerConfig *server_config;
static const char *config_file;
static int epoll_fd = -1;   // epoll_fd — epoll 实例的文件描述符。整个服务器只有一个 epoll 实例，server_run 创建它
static Connection *connections[MAX_CONNECTIONS];

/* 信号处理函数只修改退出标志，真正的资源释放由主循环完成。 */
static void stop_server(int signal_number)
{
    (void)signal_number;
    running = 0;
}

/* SIGHUP 处理函数：只置重载标志，真正的重载由主循环完成（信号处理函数必须简单）。 */
static void reload_server(int signal_number)
{
    (void)signal_number;
    reload_requested = 1;
}

/* 修改客户端 socket 在 epoll 中关注的事件。 */
static int change_events(Connection *connection, uint32_t events)
{
    struct epoll_event event = {.events = events, .data.ptr = connection};
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, connection->fd, &event);
}

/*
 * 释放一次响应拥有的文件和动态内存，但保留客户端连接。
 * Keep-Alive 复用连接时会在下一次请求前调用这里清理旧响应。
 */
static void release_response(Connection *connection)
{
    if (connection->file_fd >= 0)
        close(connection->file_fd);
    free(connection->body);
    connection->file_fd = -1;
    connection->body = NULL;
    connection->body_length = connection->body_sent = connection->body_capacity = 0;
    connection->header_length = connection->header_sent = 0;
}

/*
 * 中止未完成的上传：关闭文件并删除残留。
 * 上传失败（客户端提前断开、磁盘写入失败）时调用，避免留下半个文件。
 */
static void abort_upload(Connection *connection)
{
    if (connection->upload_fd >= 0)
    {
        close(connection->upload_fd);
        unlink(connection->upload_path);
        connection->upload_fd = -1;
        connection->upload_path[0] = '\0';
    }
}

/*
 * 终止仍在运行的 CGI 子进程，关闭管道并回收资源。
 * 连接关闭、CGI 出错或超时时调用；cgi_pid == -1 时不做任何事。
 */
static void kill_cgi(Connection *connection)
{
    if (connection->cgi_pid > 0)
    {
        kill(connection->cgi_pid, SIGKILL);
        waitpid(connection->cgi_pid, NULL, 0);
        connection->cgi_pid = -1;
    }
    if (connection->cgi_input_fd >= 0)
    {
        close(connection->cgi_input_fd);
        connection->cgi_input_fd = -1;
    }
    if (connection->cgi_output_fd >= 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, connection->cgi_output_fd, NULL);
        close(connection->cgi_output_fd);
        connection->cgi_output_fd = -1;
    }
    /* 一并释放 CGI 请求体缓冲，调用方无需重复清理。 */
    free(connection->request_body);
    connection->request_body = NULL;
    connection->request_body_length = 0;
    connection->cgi_request = false;
}

/* 把 CGI stdout 管道读端加入 epoll，开始接收脚本输出。 */
static int start_cgi_output(Connection *connection)
{
    connection->state = CONN_RUNNING_CGI;
    struct epoll_event event = {.events = EPOLLIN | EPOLLHUP, .data.ptr = connection};
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, connection->cgi_output_fd, &event);
}

/* 从 epoll、连接表和操作系统中彻底移除一个客户端。 */
static void close_connection(Connection *connection)
{
    if (connection == NULL)
        return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, connection->fd, NULL);
    if (connection->fd >= 0 && connection->fd < MAX_CONNECTIONS)
        connections[connection->fd] = NULL;
    close(connection->fd);
    abort_upload(connection);
    kill_cgi(connection); // 回收 CGI 子进程、管道和请求体缓冲
    release_response(connection);
    free(connection);
}

/* HTTP Date 响应头使用 GMT，而访问日志使用本地时间。 */
static void http_date(char *buffer, size_t buffer_size, time_t timestamp)
{
    struct tm utc;
    gmtime_r(&timestamp, &utc);
    strftime(buffer, buffer_size, "%a, %d %b %Y %H:%M:%S GMT", &utc);
}

/*
 * 构造标准响应头，并把连接从读取阶段切换到等待可写阶段。
 * extra 用于附加 Location、Content-Range、Allow 等响应头。
 */
static int build_headers(Connection *connection, int status, const char *content_type, off_t length, const char *extra)
{
    char date[64];
    http_date(date, sizeof(date), time(NULL));
    connection->status_code = status;
    connection->response_bytes = length;
    connection->keep_alive = connection->request.keep_alive;
    int written = snprintf(connection->header_buffer,
                           sizeof(connection->header_buffer),
                           "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: %s\r\n"
                           "Content-Type: %s\r\nContent-Length: %lld\r\n"
                           "Connection: %s\r\n%s\r\n",
                           status,
                           http_reason_phrase(status),
                           date,
                           SERVER_NAME,
                           content_type,
                           (long long)length,
                           connection->keep_alive ? "keep-alive" : "close",
                           extra == NULL ? "" : extra);
    if (written < 0 || (size_t)written >= sizeof(connection->header_buffer))
        return -1;
    connection->header_length = (size_t)written;
    connection->header_sent = 0;
    connection->state = CONN_WRITING_HEADER;

    /*
    响应准备完成后，修改这个 fd 在 epoll 里关注的事件类型。
    调 change_events() 让 epoll 从"等 EPOLLIN"改为"等 EPOLLOUT
    */
    return change_events(connection, EPOLLOUT | EPOLLRDHUP);
}

/* 动态生成 HTML 错误页面；HEAD 请求仍有 Content-Length，但不发送页面正文。 */
static int prepare_error(Connection *connection, int status, const char *detail)
{
    char page[1024];
    int length = snprintf(
        page,
        sizeof(page),
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>%d %s</title>"
        "<style>body{font-family:sans-serif;max-width:720px;margin:80px auto;padding:0 20px}"
        "h1{border-bottom:1px solid #ccc;padding-bottom:16px}</style></head>"
        "<body><h1>%d %s</h1><p>%s</p><hr><small>%s</small></body></html>",
        status,
        http_reason_phrase(status),
        status,
        http_reason_phrase(status),
        detail,
        SERVER_NAME);
    if (length < 0)
        return -1;
    connection->body = malloc((size_t)length);
    if (connection->body == NULL)
        return -1;
    memcpy(connection->body, page, (size_t)length);
    connection->body_length = connection->request.method == METHOD_HEAD ? 0 : (size_t)length;
    connection->body_sent = 0;

    /* 错误请求响应后关闭连接，避免在异常协议状态上继续复用。 */
    if (status >= 400)
        connection->keep_alive = false;
    connection->request.keep_alive = connection->keep_alive;
    return build_headers(connection,
                         status,
                         "text/html; charset=utf-8",
                         length,
                         status == 405 ? "Allow: GET, HEAD, POST\r\n" : NULL);
}

/* 向动态缓冲区追加指定长度的数据，空间不足时按倍数扩容。 */
static int append_bytes(char **buffer,
                        size_t *length,
                        size_t *capacity,
                        const char *data,
                        size_t data_length)
{
    if (*length + data_length + 1 > *capacity)
    {
        size_t next = *capacity == 0 ? 4096 : *capacity;
        while (next < *length + data_length + 1)
            next *= 2;// 翻倍直到足够大

        // 用 realloc 按需扩容
        char *grown = realloc(*buffer, next);
        if (grown == NULL)
            return -1;
        *buffer = grown;
        *capacity = next;
    }

    memcpy(*buffer + *length, data, data_length); // 追加到末尾
    *length += data_length;
    (*buffer)[*length] = '\0';
    return 0;
}

/* 向动态字符串末尾追加内容（NUL 结尾文本版，append_bytes 的封装）。 */
static int append_text(char **buffer, size_t *length, size_t *capacity, const char *text)
{
    return append_bytes(buffer, length, capacity, text, strlen(text));
}

/*
 * 遍历目录并动态生成 HTML 列表。

你访问 http://服务器/docs/
         ↓
服务器打开硬盘上的 docs 文件夹，看看里面有什么：
docs/
  ├── 报告.pdf      (2.3 MB, 2026-07-01 10:30)
  ├── 图片/          (文件夹)
  └── 说明.txt      (15 KB, 2026-07-02 14:20)
         ↓
把这些内容拼成一个网页的 HTML 文本，发给你
         ↓
浏览器渲染出：一个带表格的页面，每行一个文件，可以点击

 */
static int prepare_directory(Connection *connection, const char *path, const char *url)
{
    // 打开文件夹
    DIR *directory = opendir(path);
    if (directory == NULL)
        return prepare_error(connection, 403, "Directory cannot be opened.");
    char *page = NULL;
    size_t length = 0, capacity = 0;
    char escaped_url[PATH_MAX * 2];
    if (http_escape_html(url, escaped_url, sizeof(escaped_url)) != 0)
        strcpy(escaped_url, "/");
    char line[PATH_MAX * 6];

    //生成 HTML 头部（标题 "Index of /docs/"）
    snprintf(line,
             sizeof(line),
             "<!doctype html><html><head><meta charset=\"utf-8\"><title>Index of %s</title>"
             "<style>body{font-family:system-ui;max-width:900px;margin:40px auto;padding:0 20px}"
             "table{width:100%%;border-collapse:collapse}th,td{text-align:left;padding:9px;border-"
             "bottom:1px solid #ddd}"
             "a{color:#075985;text-decoration:none}</style></head><body><h1>Index of %s</h1>"
             "<table><tr><th>Name</th><th>Size</th><th>Modified</th></tr>",
             escaped_url,
             escaped_url);

    if (append_text(&page, &length, &capacity, line) != 0)
        goto failed;

    // 加一行 "../" 返回上级的链接（根目录除外）
    if (strcmp(url, "/") != 0 &&
        append_text(&page,
                    &length,
                    &capacity,
                    "<tr><td><a href=\"../\">../</a></td><td>-</td><td>-</td></tr>") != 0)
        goto failed;

    struct dirent *entry;

    // 循环逐个读目录项， 每个文件写成一行表格
    while ((entry = readdir(directory)) != NULL)
    {
        /* “.” 和 “..” 由服务器自行处理，不直接显示为普通目录项。 */
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;// "." 和 ".." 跳过，单独处理

        char full_path[PATH_MAX], encoded[PATH_MAX * 3], escaped[PATH_MAX * 2];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name) >=
            (int)sizeof(full_path))
            continue;

        // 拿每个文件的属性
        struct stat info;
        if (stat(full_path, &info) != 0)
            continue;

        // 显示在表格的的文件名做 HTML 转义，链接地址做 URL 编码
        if (http_url_encode(entry->d_name, encoded, sizeof(encoded)) != 0 ||
            http_escape_html(entry->d_name, escaped, sizeof(escaped)) != 0)
            continue;
        char modified[64];
        struct tm local;
        localtime_r(&info.st_mtime, &local);
        strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M", &local);
        char size_text[32] = "-";
        if (!S_ISDIR(info.st_mode))
            snprintf(size_text, sizeof(size_text), "%lld", (long long)info.st_size);
        snprintf(line,
                 sizeof(line),
                 "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>",
                 encoded,
                 S_ISDIR(info.st_mode) ? "/" : "",
                 escaped,
                 S_ISDIR(info.st_mode) ? "/" : "",
                 size_text,
                 modified);
        if (append_text(&page, &length, &capacity, line) != 0)
            goto failed;
    }
    closedir(directory);

    // 写网页尾部：补上表格结尾、分隔线、服务器名页脚，闭合 HTML 文档；失败则释放内存并回 500"。
    if (append_text(&page,
                    &length,
                    &capacity,
                    "</table><hr><small>" SERVER_NAME "</small></body></html>") != 0)
        goto failed_closed;

    // 生成好的 HTML 存在 connection->body，之后 handle_write 用 send 发出去
    connection->body = page;
    connection->body_length = connection->request.method == METHOD_HEAD ? 0 : length;
    connection->body_sent = 0;
    return build_headers(connection, 200, "text/html; charset=utf-8", (off_t)length, NULL);
failed:// 关闭目录
    closedir(directory);
failed_closed: // 把拼了一半的网页内存释放掉
    free(page);
    return prepare_error(connection, 500, "Cannot build directory listing.");
}

/*
 * 准备普通文件响应。
 * Range 决定初始文件偏移和总长度，实际发送进度由 handle_write() 更新。
 */
static int prepare_file(Connection *connection, const char *path, const struct stat *info)
{
    off_t start, end;

    // 解析 Range 头
    if (http_parse_range(&connection->request, info->st_size, &start, &end) != 0)
    {
        char extra[128];
        snprintf(extra, sizeof(extra), "Content-Range: bytes */%lld\r\n", (long long)info->st_size);
        connection->request.keep_alive = false;
        return build_headers(connection, 416, "text/plain; charset=utf-8", 0, extra);
    }

    // open() 打开文件
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return prepare_error(connection, errno == EACCES ? 403 : 404, "File cannot be opened.");

    // 计算要发多少字节。没有 Range 是整文件，有 Range 是 end - start + 1；
    off_t length = info->st_size == 0 ? 0 : end - start + 1;

    // 生成额外响应头
    char extra[256];
    // 有 Range：
    if (connection->request.has_range)
    {
        /*
        示例：
        Accept-Ranges: bytes    // Accept-Ranges: bytes 是一个能力声明，告诉客户端可以发 Range 请求
        Content-Range: bytes 100-199/10485760     ← 发的是 100~199，文件总共 10485760 字节
        */
        snprintf(extra,
                 sizeof(extra),
                 "Accept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%lld\r\n",
                 (long long)start,
                 (long long)end,
                 (long long)info->st_size);
    }
    // 没有range
    else
        strcpy(extra, "Accept-Ranges: bytes\r\n");

    //  把发送进度存进连接结构
    connection->file_fd = fd;// 打开的文件
    connection->file_offset = start;// 从文件的哪个位置开始发（后续由sendfile维护）。 handle_write 用 sendfile 从 offset 处发，分几次发完，

    /* 对于 HTTP 的 HEAD 请求方法，保留真实 Content-Length，用于响应给客户端文件正文的真实大小，但不发送文件正文。 build_headers 收到的倒数第二个参数Length是真实的 length，写进 Content-Length 头。 */
    connection->file_remaining = connection->request.method == METHOD_HEAD ? 0 : length;// 总共还剩多少正文字节要响应

    // build_headers 调用：把响应头（状态行 + Content-Type + Content-Length + 上面拼的 extra 头）写进 connection->header_buffer
    int result = build_headers(
        connection, connection->request.has_range ? 206 : 200, http_mime_type(path), length, extra);
    if (result != 0)   // build_headers 失败（比如 epoll_ctl 失败）
    {
        close(fd);
        connection->file_fd = -1;// 把账本改成"没有打开的文件"
    }
    return result;
}

/* prepare_upload 需要直接消费缓冲区里已到达的请求体，先在这里声明。 */
static int handle_read_body(Connection *connection);

/*
 * 准备 POST 上传：把请求体流式写入 document_root/uploads/ 下的目标文件。
 * 校验顺序：目录限制 → 传输编码 → Content-Length 存在性 → 大小上限 → 路径安全。
 * 目标文件在请求完成前不存在，realpath 不能解析它，因此先规范化父目录再拼接文件名。
 */
static int prepare_upload(Connection *connection, const char *decoded)
{
    /* 上传只允许写入 /uploads/ 子目录，避免覆盖站点其他文件。 */
    if (strncmp(decoded, "/uploads/", 9) != 0 || strlen(decoded) <= 9)
        return prepare_error(connection, 403, "Uploads are only allowed under /uploads/.");

    if (connection->request.chunked)
        return prepare_error(connection, 501, "Chunked request bodies are not supported.");
    if (!connection->request.has_content_length)
        return prepare_error(connection, 411, "POST requires a Content-Length header.");
    if (connection->request.content_length > MAX_UPLOAD_SIZE)
        return prepare_error(connection, 413, "Upload exceeds the 64 MiB limit.");

    char candidate[PATH_MAX];
    if (snprintf(candidate, sizeof(candidate), "%s%s", server_config->document_root, decoded) >=
        (int)sizeof(candidate))
        return prepare_error(connection, 400, "Path is too long.");

    char *slash = strrchr(candidate, '/');
    if (slash == NULL || slash[1] == '\0')
        return prepare_error(connection, 400, "Upload path must end with a file name.");

    char parent[PATH_MAX];
    size_t parent_length = (size_t)(slash - candidate);
    memcpy(parent, candidate, parent_length);
    parent[parent_length] = '\0';

    char resolved_parent[PATH_MAX];
    if (realpath(parent, resolved_parent) == NULL)
        return prepare_error(connection, 404, "Upload directory does not exist.");

    /* 与普通请求相同的约束：规范化后的父目录必须仍位于网站根目录内。 */
    size_t root_length = strlen(server_config->document_root);
    if (strncmp(resolved_parent, server_config->document_root, root_length) != 0 ||
        (resolved_parent[root_length] != '\0' && resolved_parent[root_length] != '/'))
        return prepare_error(connection, 403, "Upload directory is outside the document root.");

    if (snprintf(connection->upload_path,
                 sizeof(connection->upload_path),
                 "%s/%s",
                 resolved_parent,
                 slash + 1) >= (int)sizeof(connection->upload_path))
        return prepare_error(connection, 400, "Path is too long.");

    /* 同名文件直接覆盖（O_TRUNC）。 */
    int fd = open(connection->upload_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return prepare_error(connection,
                             errno == EACCES ? 403 : 500,
                             "Upload file cannot be created.");

    connection->upload_fd = fd;
    connection->content_received = 0;
    connection->state = CONN_READING_BODY;

    /* 缓冲区里可能已经带有部分请求体（甚至流水线中的下一个请求），立即处理。 */
    if (connection->read_length > 0)
        return handle_read_body(connection);

    /* 请求体尚未到达，继续等待 EPOLLIN。 */
    return change_events(connection, EPOLLIN | EPOLLRDHUP);
}

/*
 * CGI 子进程：把标准输入输出接到管道后 exec 执行脚本，永远不返回。
 * 请求信息通过环境变量传递——这是 CGI 协议的核心约定。
 */
static void run_cgi_child(const Connection *connection,
                          const char *path,
                          const char *decoded,
                          const int *input_pipe,
                          const int *output_pipe)
{
    dup2(input_pipe[0], STDIN_FILENO);   /* 请求体从管道读入 */
    dup2(output_pipe[1], STDOUT_FILENO); /* 输出写回管道 */
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);

    /* socket、epoll 等 fd 创建时都带 CLOEXEC，exec 时自动关闭；这里显式关闭本连接的。 */
    close(connection->fd);
    close(epoll_fd);

    const char *query = strchr(connection->request.target, '?');
    char content_length[32] = "";
    if (connection->request.has_content_length)
        snprintf(content_length,
                 sizeof(content_length),
                 "%lld",
                 (long long)connection->request.content_length);
    char protocol[16];
    snprintf(protocol, sizeof(protocol), "HTTP/1.%d", connection->request.http_minor);

    setenv("REQUEST_METHOD",
           connection->request.method == METHOD_POST ? "POST" : "GET",
           1);
    setenv("QUERY_STRING", query != NULL ? query + 1 : "", 1);
    setenv("SCRIPT_NAME", decoded, 1);
    setenv("CONTENT_LENGTH", content_length, 1);
    setenv("CONTENT_TYPE", connection->request.content_type, 1);
    setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
    setenv("SERVER_PROTOCOL", protocol, 1);
    setenv("REMOTE_ADDR", connection->client_ip, 1);
    setenv("SERVER_SOFTWARE", SERVER_NAME, 1);

    execl(path, path, (char *)NULL);

    /* exec 失败：按 CGI 格式输出 500，父进程解析响应头时会看到状态码。 */
    const char message[] = "Status: 500 Internal Server Error\r\n"
                           "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                           "Cannot execute CGI script.\n";
    ssize_t ignored = write(STDOUT_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    _exit(1);
}

/*
 * 准备 CGI 执行：校验脚本、创建管道、fork 子进程。
 * GET 直接进入输出接收阶段；POST 的请求体先缓冲（≤ 64 KiB），
 * 收满后一次性写进子进程 stdin——上限恰好等于 Linux 管道容量，
 * 保证写管道不会阻塞事件循环。
 */
static int prepare_cgi(Connection *connection, const char *decoded)
{
    /* 脚本必须存在且位于网站根目录内。 */
    char candidate[PATH_MAX];
    if (snprintf(candidate, sizeof(candidate), "%s%s", server_config->document_root, decoded) >=
        (int)sizeof(candidate))
        return prepare_error(connection, 400, "Path is too long.");
    char path[PATH_MAX];
    if (realpath(candidate, path) == NULL)
        return prepare_error(connection, 404, "CGI script was not found.");
    size_t root_length = strlen(server_config->document_root);
    if (strncmp(path, server_config->document_root, root_length) != 0 ||
        (path[root_length] != '\0' && path[root_length] != '/'))
        return prepare_error(connection, 403, "CGI script is outside the document root.");

    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
        return prepare_error(connection, 404, "CGI script was not found.");
    if (access(path, X_OK) != 0)
        return prepare_error(connection, 403, "CGI script is not executable.");

    if (connection->request.chunked)
        return prepare_error(connection, 501, "Chunked request bodies are not supported.");
    if (connection->request.method == METHOD_POST && !connection->request.has_content_length)
        return prepare_error(connection, 411, "POST requires a Content-Length header.");
    if (connection->request.content_length > CGI_BODY_MAX)
        return prepare_error(connection, 413, "CGI request body exceeds the 64 KiB limit.");

    /* 两条管道：input 连接脚本 stdin，output 连接脚本 stdout。 */
    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe2(input_pipe, O_CLOEXEC) != 0 || pipe2(output_pipe, O_CLOEXEC) != 0)
    {
        if (input_pipe[0] >= 0)
        {
            close(input_pipe[0]);
            close(input_pipe[1]);
        }
        if (output_pipe[0] >= 0)
        {
            close(output_pipe[0]);
            close(output_pipe[1]);
        }
        return prepare_error(connection, 500, "Cannot create CGI pipes.");
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return prepare_error(connection, 500, "Cannot fork CGI process.");
    }

    if (pid == 0)
        run_cgi_child(connection, path, decoded, input_pipe, output_pipe);

    /* 父进程：关闭不用的管道端，把脚本输出管道设为非阻塞并接管。 */
    close(input_pipe[0]);
    close(output_pipe[1]);
    int flags = fcntl(output_pipe[0], F_GETFL, 0);
    fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);

    connection->cgi_pid = (int)pid;
    connection->cgi_output_fd = output_pipe[0];
    connection->cgi_started = time(NULL);
    connection->cgi_status = 200;
    connection->cgi_content_type[0] = '\0';

    /* POST：请求体缓冲进内存，收满后写进脚本 stdin。 */
    if (connection->request.has_body)
    {
        connection->cgi_input_fd = input_pipe[1];
        connection->cgi_request = true;
        connection->request_body = malloc((size_t)connection->request.content_length);
        if (connection->request_body == NULL)
        {
            kill_cgi(connection);
            return prepare_error(connection, 500, "Out of memory.");
        }
        connection->request_body_length = 0;
        connection->state = CONN_READING_BODY;
        if (connection->read_length > 0)
            return handle_read_body(connection);
        return change_events(connection, EPOLLIN | EPOLLRDHUP);
    }

    /* GET：没有请求体，关闭写端让脚本读到 EOF，直接进入输出接收。 */
    close(input_pipe[1]);
    return start_cgi_output(connection);
}

/*
 * 将 URL 映射到网站根目录中的真实资源，并选择文件或目录响应。
 * 返回 0 表示响应已经准备好，返回非 0 表示该连接无法继续处理。
 */
static int prepare_request(Connection *connection)
{
    if (connection->request.method == METHOD_UNSUPPORTED)
        return prepare_error(connection, 405, "Only GET, HEAD and POST are supported.");

    // URL解码
    char decoded[PATH_MAX];
    int decode = http_url_decode_path(connection->request.target, decoded, sizeof(decoded));
    if (decode == -2)
        return prepare_error(connection, 403, "Parent directory traversal is forbidden.");
    if (decode != 0)
        return prepare_error(connection, 400, "Invalid URL encoding.");

    /* /cgi-bin/ 下的脚本交给 CGI 执行，优先于静态文件和上传逻辑。 */
    if (strncmp(decoded, "/cgi-bin/", 9) == 0)
    {
        if (connection->request.method != METHOD_GET && connection->request.method != METHOD_POST)
            return prepare_error(connection, 405, "CGI scripts only support GET and POST.");
        return prepare_cgi(connection, decoded);
    }

    /* POST 上传走独立路径，其余逻辑只服务 GET 和 HEAD。 */
    if (connection->request.method == METHOD_POST)
        return prepare_upload(connection, decoded);

    /* GET/HEAD 不允许携带请求体：多余的字节会被当成下一个请求解析。 */
    if (connection->request.has_body)
        return prepare_error(connection, 400, "Request body is only supported for POST.");

    char candidate[PATH_MAX];
    if (snprintf(candidate, sizeof(candidate), "%s%s", server_config->document_root, decoded) >=
        (int)sizeof(candidate))
        return prepare_error(connection, 400, "Path is too long.");
    char path[PATH_MAX];

    /* realpath 消除 .、.. 和符号链接，得到最终真实文件路径。 */
    if (realpath(candidate, path) == NULL)
        return prepare_error(connection, 404, "The requested resource was not found.");
    size_t root_length = strlen(server_config->document_root);

    /* 即使网站目录中存在符号链接，也不允许最终路径逃出 document_root。 */
    if (strncmp(path, server_config->document_root, root_length) != 0 ||
        (path[root_length] != '\0' && path[root_length] != '/'))
        return prepare_error(connection, 403, "Resource is outside the document root.");

    // 看要的是目录还是文件
    struct stat info;
    if (stat(path, &info) != 0)
        return prepare_error(connection, 404, "The requested resource was not found.");
    if (S_ISDIR(info.st_mode))
    {
        size_t url_length = strlen(decoded);

        /* 目录 URL 需要以 / 结尾，否则浏览器会错误解析页面中的相对链接。 */
        if (url_length == 0 || decoded[url_length - 1] != '/')
        {
            char location[PATH_MAX + 32];
            snprintf(location, sizeof(location), "Location: %s/\r\n", connection->request.target);
            return build_headers(connection, 301, "text/html; charset=utf-8", 0, location);// URL 结尾没 / → 301 重定向补上
        }
        char index_path[PATH_MAX];

        /* 访问目录时，如果有 index.html 那就返回index.html ，没有 index.html 时才生成目录浏览页面。 */
        if (snprintf(index_path, sizeof(index_path), "%s/index.html", path) <
                (int)sizeof(index_path) &&
            stat(index_path, &info) == 0 && S_ISREG(info.st_mode))
            return prepare_file(connection, index_path, &info);
        return prepare_directory(connection, path, decoded);
    }
    if (!S_ISREG(info.st_mode))
        return prepare_error(connection, 403, "Resource is not a regular file.");
    // 请求的是普通文件，则调用prepare_file()来返回普通文件
    return prepare_file(connection, path, &info);
}

/*
 * 一次响应发送完成后的统一收尾入口。
 * Keep-Alive 时恢复读取状态；否则返回 -1 让事件循环关闭连接。
 */
static int finish_response(Connection *connection)
{
    // 记录访问日志
    log_access(connection);

    // 决定去留
    bool keep = connection->keep_alive && !connection->close_after_response;

    // 释放这次响应的资源（关文件 fd、free 内存），但保留连接本身；
    release_response(connection);

    if (!keep)// 不复用
        return -1;
    connection->state = CONN_READING;// 复用 -> 状态切回 CONN_READING
    connection->last_active = time(NULL);
    if (change_events(connection, EPOLLIN | EPOLLRDHUP) != 0)
        return -1;

    /*
     * 缓冲区中可能已经带有同一 TCP 连接的下一个请求。
     * 这种情况下无需等待新的 EPOLLIN，可以立即继续解析。
     */
    if (connection->read_length > 0)// 缓冲区里还有没处理的数据？
    {
        size_t request_length;
        if (http_find_request_end( // 而且已经攒出一个完整请求头？
                connection->read_buffer, connection->read_length, &request_length))
        {
            int parsed =
                http_parse_request(connection->read_buffer, request_length, &connection->request);
            memmove(connection->read_buffer,
                    connection->read_buffer + request_length,
                    connection->read_length - request_length);
            connection->read_length -= request_length;
            if (parsed == -2)
                return prepare_error(connection, 505, "Unsupported HTTP version.");
            if (parsed != 0)
                return prepare_error(connection, 400, "Malformed HTTP request.");
            return prepare_request(connection);// 立刻处理它
        }
    }
    return 0;// 数据不完整 → 乖乖等下次 EPOLLIN 继续攒
}

/*
 * 解析 CGI 脚本输出的响应头（空行之前的部分），只关心 Status 和 Content-Type。
 * 解析直接在缓冲区里写 '\0' 截断；返回空行后的正文偏移。
 */
static void cgi_parse_headers(Connection *connection, size_t *body_offset)
{
    bool found = false;
    size_t header_end = 0;
    for (size_t i = 0; i + 3 < connection->body_length; ++i)
    {
        if (memcmp(connection->body + i, "\r\n\r\n", 4) == 0)
        {
            found = true;
            header_end = i;
            break;
        }
    }
    if (!found)
    {
        /* 没有空行分隔：全部输出都按正文处理，状态码和类型用默认值。 */
        *body_offset = 0;
        return;
    }

    connection->body[header_end] = '\0'; /* 临时截断，逐行解析 */
    char *line = connection->body;
    while (*line != '\0')
    {
        char *next = strstr(line, "\r\n");
        if (next != NULL)
            *next = '\0';
        char *colon = strchr(line, ':');
        if (colon != NULL)
        {
            *colon = '\0';
            char *value = colon + 1;
            while (*value == ' ' || *value == '\t')
                ++value;
            if (strcasecmp(line, "Status") == 0)
            {
                errno = 0;
                long status = strtol(value, NULL, 10);
                if (errno == 0 && status >= 100 && status <= 999)
                    connection->cgi_status = (int)status;
            }
            else if (strcasecmp(line, "Content-Type") == 0)
                snprintf(connection->cgi_content_type,
                         sizeof(connection->cgi_content_type),
                         "%s",
                         value);
        }
        if (next == NULL)
            break;
        line = next + 2;
    }
    *body_offset = header_end + 4;
}

/* 脚本输出读取完毕：回收子进程，解析响应头，转入正常的响应发送路径。 */
static int finish_cgi(Connection *connection)
{
    if (connection->cgi_pid > 0)
    {
        waitpid(connection->cgi_pid, NULL, 0);
        connection->cgi_pid = -1;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, connection->cgi_output_fd, NULL);
    close(connection->cgi_output_fd);
    connection->cgi_output_fd = -1;

    /* 去掉响应头部分，正文从空行后开始。 */
    size_t body_offset;
    cgi_parse_headers(connection, &body_offset);
    if (connection->body_length > body_offset)
        memmove(connection->body,
                connection->body + body_offset,
                connection->body_length - body_offset);
    connection->body_length -= body_offset;
    connection->body_sent = 0;
    return build_headers(connection,
                         connection->cgi_status,
                         connection->cgi_content_type[0] != '\0'
                             ? connection->cgi_content_type
                             : "text/html; charset=utf-8",
                         (off_t)connection->body_length,
                         NULL);
}

/*
 * 处理 CGI stdout 管道可读：把脚本输出追加进 body 缓冲，直到 EOF。
 * 输出超过 1 MiB 时终止子进程并回 500，防止失控脚本耗尽服务器内存。
 */
static int handle_cgi_read(Connection *connection)
{
    char buffer[4096];
    while (1)
    {
        ssize_t count = read(connection->cgi_output_fd, buffer, sizeof(buffer));
        if (count > 0)
        {
            connection->last_active = time(NULL);
            if (connection->body_length + (size_t)count > CGI_OUTPUT_MAX)
            {
                kill_cgi(connection);
                release_response(connection);
                return prepare_error(connection, 500, "CGI output exceeds the 1 MiB limit.");
            }
            if (append_bytes(&connection->body,
                             &connection->body_length,
                             &connection->body_capacity,
                             buffer,
                             (size_t)count) != 0)
            {
                kill_cgi(connection);
                release_response(connection);
                return prepare_error(connection, 500, "Out of memory.");
            }
            continue;
        }
        if (count == 0)
            return finish_cgi(connection); /* 脚本关闭 stdout，输出完毕 */
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            kill_cgi(connection);
            release_response(connection);
            return prepare_error(connection, 500, "Cannot read CGI output.");
        }
        return 0; /* 暂时没有更多输出，等下一次 EPOLLIN。 */
    }
}

/* POST 请求体收满：一次性写进 CGI stdin 后关闭，转入输出接收。 */
static int finish_cgi_body(Connection *connection)
{
    /* 请求体 ≤ 64 KiB（prepare_cgi 已校验），写入空管道不会阻塞。 */
    size_t written = 0;
    while (written < connection->request_body_length)
    {
        ssize_t result = write(connection->cgi_input_fd,
                               connection->request_body + written,
                               connection->request_body_length - written);
        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            break; /* EPIPE：脚本提前退出，忽略剩余数据。 */
        }
        written += (size_t)result;
    }
    close(connection->cgi_input_fd);
    connection->cgi_input_fd = -1;
    free(connection->request_body);
    connection->request_body = NULL;
    connection->request_body_length = 0;
    connection->cgi_request = false;
    return start_cgi_output(connection);
}

/* 把一段已收到的请求体写入上传文件，处理部分写入和 EINTR。 */
static int write_upload(Connection *connection, const char *data, size_t length)
{
    size_t written = 0;
    while (written < length)
    {
        ssize_t result = write(connection->upload_fd, data + written, length - written);
        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            return -1; /* 磁盘满等错误：关闭连接，abort_upload 删除残留文件。 */
        }
        written += (size_t)result;
    }
    return 0;
}

/* 上传完成后的收尾：关闭文件，生成 201 Created 响应。 */
static int finish_upload(Connection *connection)
{
    close(connection->upload_fd);
    connection->upload_fd = -1;
    connection->upload_path[0] = '\0'; /* 文件完整落盘，无需再删除。 */

    char page[PATH_MAX + 64];
    int length = snprintf(page, sizeof(page), "Created: %s\n", connection->request.target);
    if (length < 0)
        return -1;
    connection->body = malloc((size_t)length);
    if (connection->body == NULL)
        return -1;
    memcpy(connection->body, page, (size_t)length);
    connection->body_length = (size_t)length;
    connection->body_sent = 0;
    return build_headers(connection, 201, "text/plain; charset=utf-8", (off_t)length, NULL);
}

/*
 * 接收 POST 请求体：把 read_buffer 中属于请求体的字节写入上传文件
 * （或 CGI 请求时缓冲进内存）。一次 recv 可能把请求体之后的流水线请求
 * 也带进来，超出 Content-Length 的字节必须原样留在缓冲区里，
 * 供 Keep-Alive 复用时解析下一个请求。
 */
static int handle_read_body(Connection *connection)
{
    while (connection->content_received < connection->request.content_length)
    {
        /* 缓冲区为空时继续收数据。 */
        if (connection->read_length == 0)
        {
            ssize_t count = recv(connection->fd,
                                 connection->read_buffer,
                                 sizeof(connection->read_buffer),
                                 0);
            if (count > 0)
            {
                connection->read_length = (size_t)count;
                connection->last_active = time(NULL);
                continue;
            }
            if (count == 0)
                return -1; /* 客户端提前断开，上传不完整。 */
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;
            return 0; /* 请求体尚未收完，等下一次 EPOLLIN。 */
        }

        /* 只处理属于请求体的字节，剩余部分留给流水线中的下一个请求。 */
        off_t remaining = connection->request.content_length - connection->content_received;
        size_t take = remaining < (off_t)connection->read_length
                          ? (size_t)remaining
                          : connection->read_length;

        if (connection->cgi_request)
        {
            /* CGI：请求体缓冲进内存，收满后交给子进程 stdin。 */
            memcpy(connection->request_body + connection->request_body_length,
                   connection->read_buffer,
                   take);
            connection->request_body_length += take;
        }
        else if (write_upload(connection, connection->read_buffer, take) != 0)
            return -1;
        connection->content_received += (off_t)take;
        memmove(connection->read_buffer,
                connection->read_buffer + take,
                connection->read_length - take);
        connection->read_length -= take;
    }
    return connection->cgi_request ? finish_cgi_body(connection) : finish_upload(connection);
}

/*
 * 处理 EPOLLIN：循环读取到 EAGAIN，并在缓冲区中寻找完整请求头。
 * 这样既能处理一个请求分多次到达，也能处理多个请求连续到达。
 */
static int handle_read(Connection *connection)
{
    /* POST 请求体阶段不解析请求头，按 Content-Length 接收剩余字节。 */
    if (connection->state == CONN_READING_BODY)
        return handle_read_body(connection);

    // 循环 recv() 把数据往 read_buffer 里攒，读到 EAGAIN（暂时没数据了）为止；
    while (connection->read_length < sizeof(connection->read_buffer))
    {
        ssize_t count = recv(connection->fd,
                             connection->read_buffer + connection->read_length,
                             sizeof(connection->read_buffer) - connection->read_length,
                             0);
        if (count > 0)
        {
            connection->read_length += (size_t)count;
            connection->last_active = time(NULL);
            continue;
        }
        if (count == 0)
        {
            /* 客户端关闭写端；已有请求仍可响应，但响应后必须关闭连接。 */
            if (connection->read_length == 0)
                return -1;
            connection->close_after_response = true;
            break;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        break;
    }
    size_t request_length;

    // 用 http_find_request_end() 在缓冲区里找 \r\n\r\n（请求头的结束标志）；
    if (!http_find_request_end(connection->read_buffer, connection->read_length, &request_length))
    {
        /* 32 KiB 缓冲区已满仍没有空行，说明请求头超过限制。 */
        if (connection->read_length == sizeof(connection->read_buffer))
        {
            connection->request.method = METHOD_GET;
            connection->request.http_minor = 1;
            connection->request.keep_alive = false;
            strcpy(connection->request.target, "-");
            return prepare_error(connection, 431, "Request headers exceed 32 KiB.");
        }
        return 0;
    }

    // 在缓冲区里找到了请求头的结束标志 → http_parse_request()进行解析并把解析内容放到connection->request -> 交给 prepare_request()处理
    // 返回值为错误码，用于后续分流处理，0表示解析成功
    int parsed = http_parse_request(connection->read_buffer, request_length, &connection->request);

    /* 完整请求之后的数据可能属于下一个流水线请求，必须保留下来。 */
    memmove(connection->read_buffer,
            connection->read_buffer + request_length,
            connection->read_length - request_length);
    connection->read_length -= request_length;

    if (parsed == -2)
        return prepare_error(connection, 505, "Unsupported HTTP version.");
    if (parsed == -3)
        return prepare_error(connection, 416, "Invalid Range header.");
    if (parsed != 0)
        return prepare_error(connection, 400, "Malformed HTTP request.");

    return prepare_request(connection);
}

/*
 * 处理 EPOLLOUT：分阶段依次发送响应头、内存响应体或文件正文。
 * 非阻塞发送可能只完成一部分，因此每一种数据都有独立的发送进度。
 */
static int handle_write(Connection *connection)
{
    // 发送响应头：send() 发 header_buffer，进度记在 header_sent ， 根据send的返回值count来做后续处理
    while (connection->header_sent < connection->header_length)
    {
        ssize_t count = send(connection->fd,
                             connection->header_buffer + connection->header_sent,// 从未发的位置继续
                             connection->header_length - connection->header_sent,// 剩下的字节数
                             MSG_NOSIGNAL);// send 的第四个参数（server.c:481）。假如客户端在服务器发送中途拔线，内核会向服务器进程发 SIGPIPE 信号，默认行为是杀掉进程——一个人拔线，全服陪葬。MSG_NOSIGNAL 告诉内核："别发 SIGPIPE，send 返回 -1 + EPIPE 就行，我自己处理。

        // 发出去了，进度字段往前挪，接着发剩下的
        if (count > 0)
        {
            /* 下一次从 header_sent 位置继续，不重复发送已经写出的部分。 */
            connection->header_sent += (size_t)count;// 记账：又发出去了 count 字节
            connection->last_active = time(NULL);
            continue;
        }

        // EINTR：信号打断，重试；
        if (count < 0 && errno == EINTR)
            continue;

        // EAGAIN：发送缓冲区满了，这是正常的暂时状态。等后续的EPOLLOUT
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;

        // 其他错误
        return -1;
    }

    // 发送内存正文（目录页、错误页）：发 connection->body，进度记在 body_sent；
    if (connection->body != NULL && connection->body_sent < connection->body_length)
    {
        // 将state状态设置为内存状态（CONN_WRITING_MEMORY），主要是给人看的标记，程序并不需要这个也可以运行
        connection->state = CONN_WRITING_MEMORY;

        while (connection->body_sent < connection->body_length)
        {
            ssize_t count = send(connection->fd,
                                 connection->body + connection->body_sent,
                                 connection->body_length - connection->body_sent,
                                 MSG_NOSIGNAL);
            if (count > 0)
            {
                /* 目录页面和错误页面位于内存中，用 body_sent 保存进度。 */
                connection->body_sent += (size_t)count;
                connection->last_active = time(NULL);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            return -1;
        }
    }

    // 发送文件正文：用 sendfile() 零拷贝直接从文件发到 socket（数据不经过用户态内存，这是服务器性能关键），每次最多提交 1 MiB，进度记在 file_remaining（倒计数）
    else if (connection->file_fd >= 0 && connection->file_remaining > 0)
    {
        connection->state = CONN_WRITING_FILE;
        while (connection->file_remaining > 0)
        {
            /* 每次最多提交 1 MiB，实际完成量仍以 sendfile 返回值为准。 */
            size_t block = connection->file_remaining > 1024 * 1024
                               ? 1024 * 1024
                               : (size_t)connection->file_remaining;

            // 用 sendfile 而不是 send（内核直接从文件读，不用把文件内容拷进用户内存）
            ssize_t count = sendfile(connection->fd, connection->file_fd, &connection->file_offset, block);
            if (count > 0)
            {
                connection->file_remaining -= count;// 手动维护file_remaining
                connection->last_active = time(NULL);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            return -1;
        }
    }

    // 全部发完 → 调 finish_response()。
    return finish_response(connection);
}

/* 按 socket → setsockopt → bind → listen 流程创建非阻塞监听 socket。 */
static int create_listener(int port)
{
    //SOCK_CLOEXEC作用是在exec 时自动关闭fd 。具体：创建 Socket 的同时，原子性地为该文件描述符设置 FD_CLOEXEC（close-on-exec）属性，保证进程调用 exec 系列函数加载新程序时，自动关闭这个 Socket 文件描述符，避免被子程序使用
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int option = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) != 0)
        goto failed;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);

    // 把socket跟IP地址和端口绑定起来,并调用listen开始监听socket
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, SOMAXCONN) != 0)
        goto failed;
    return fd;
failed:
    close(fd);
    return -1;
}

/*
 * 持续 accept4() 直到返回 EAGAIN，确保取完监听队列中的现有连接。
 * 新客户端初始状态为 CONN_READING，并在 epoll 中关注 EPOLLIN。
 */
static void accept_connections(int listener)
{
    while (1)
    {
        struct sockaddr_in address;
        socklen_t size = sizeof(address);

        // 为新连接创建socket fd ，用于跟用户做一对一的连接
        int fd = accept4(listener, (struct sockaddr *)&address, &size, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                log_error("accept failed: %s", strerror(errno));
            return;
        }
        if (fd >= MAX_CONNECTIONS || connections[fd] != NULL)
        {
            close(fd);
            continue;
        }

        /*
        calloc(1, sizeof(*connection)) — 向操作系统申请一块内存，大小刚好装下一个 Connection 结构体，并且全部清零
        返回这块内存的首地址，比如 0x7f...abc0
        把这个地址存进局部变量 connection 里
        */
        Connection *connection = calloc(1, sizeof(*connection));//返回指向这块内存的指针
        if (connection == NULL)// 处理内存分配失败的情况
        {
            close(fd);// 直接关掉，客户端会收到连接被关闭。
            continue;
        }
        connection->fd = fd;
        /* fd 类字段必须显式设为 -1：calloc 清零后 0 是合法的 stdin，会误关。 */
        connection->file_fd = -1;
        connection->upload_fd = -1;
        connection->cgi_pid = -1;
        connection->cgi_input_fd = -1;
        connection->cgi_output_fd = -1;
        connection->state = CONN_READING; // 等待客户端发送请求
        connection->last_active = time(NULL);
        if (inet_ntop(
                AF_INET, &address.sin_addr, connection->client_ip, sizeof(connection->client_ip)) ==
            NULL)
            strcpy(connection->client_ip, "unknown");

        //然后把这个连接加入 epoll 和全局数组connections[]，之后全靠 data.ptr 找回它。
        struct epoll_event event = {.events = EPOLLIN | EPOLLRDHUP, .data.ptr = connection};
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0)
        {
            close(fd);
            free(connection);
            continue;
        }
        connections[fd] = connection;
    }
}

/* 定期清理：CGI 脚本超时强杀并回 504；关闭超过 Keep-Alive 空闲时间的客户端。 */
static void close_idle_connections(void)
{
    time_t now = time(NULL);
    for (int fd = 0; fd < MAX_CONNECTIONS; ++fd)
    {
        Connection *connection = connections[fd];
        if (connection == NULL)
            continue;

        /* CGI 超时以子进程启动时间为准，与连接是否活跃无关。
           cgi_request 覆盖"POST 请求体还没收完"的阶段，此时子进程同样在运行。 */
        if ((connection->state == CONN_RUNNING_CGI || connection->cgi_request) &&
            now - connection->cgi_started >= CGI_TIMEOUT)
        {
            log_error("CGI script timed out after %d seconds, killing pid %d",
                      CGI_TIMEOUT,
                      connection->cgi_pid);
            kill_cgi(connection);
            release_response(connection);
            if (prepare_error(connection, 504, "CGI script timed out.") != 0)
                close_connection(connection);
            continue;
        }

        if (now - connection->last_active >= server_config->keepalive_timeout)
            close_connection(connection);
    }
}

/*
 * SIGHUP 热加载：重新解析配置文件并应用新配置。
 * 顺序：解析 → 校验根目录 → 上传目录 → 重建监听 socket（端口变化）→ 重开日志。
 * 任何一步失败立即放弃，保持旧配置不变——要么全部生效，要么全部不生效。
 */
static void apply_reload(int *listener)
{
    /* 以当前配置为底，配置文件只覆盖出现的字段。 */
    ServerConfig fresh = current_config;
    if (config_parse_file(config_file, &fresh) != 0)
    {
        log_error("reload failed: cannot read config file %s, keeping old configuration",
                  config_file);
        return;
    }

    /* 校验新的网站根目录并转换为绝对路径。 */
    char resolved[PATH_MAX];
    if (realpath(fresh.document_root, resolved) == NULL)
    {
        log_error("reload failed: document root %s does not exist", fresh.document_root);
        return;
    }
    struct stat info;
    if (stat(resolved, &info) != 0 || !S_ISDIR(info.st_mode))
    {
        log_error("reload failed: document root %s is not a directory", fresh.document_root);
        return;
    }
    strcpy(fresh.document_root, resolved);

    /* 新根目录下可能没有上传和 CGI 目录，补建。 */
    char sub_dir[PATH_MAX];
    if (snprintf(sub_dir, sizeof(sub_dir), "%s/uploads", resolved) >=
        (int)sizeof(sub_dir))
    {
        log_error("reload failed: document root path is too long");
        return;
    }
    if (mkdir(sub_dir, 0755) != 0 && errno != EEXIST)
    {
        log_error("reload failed: cannot create %s: %s", sub_dir, strerror(errno));
        return;
    }
    if (snprintf(sub_dir, sizeof(sub_dir), "%s/cgi-bin", resolved) >= (int)sizeof(sub_dir))
    {
        log_error("reload failed: document root path is too long");
        return;
    }
    if (mkdir(sub_dir, 0755) != 0 && errno != EEXIST)
    {
        log_error("reload failed: cannot create %s: %s", sub_dir, strerror(errno));
        return;
    }

    /* 端口变化：先建好新监听 socket 并加入 epoll，再拆除旧的，监听不中断。 */
    if (fresh.port != current_config.port)
    {
        int new_listener = create_listener(fresh.port);
        if (new_listener < 0)
        {
            log_error("reload failed: cannot bind port %d, keeping old configuration",
                      fresh.port);
            return;
        }
        struct epoll_event event = {.events = EPOLLIN, .data.ptr = NULL};
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_listener, &event) != 0)
        {
            log_error("reload failed: cannot register new listener: %s", strerror(errno));
            close(new_listener);
            return;
        }
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, *listener, NULL);
        close(*listener);
        *listener = new_listener;
    }

    /* 日志路径变化时重开日志文件；失败则保留旧路径，下次 reload 再试。 */
    if (strcmp(fresh.access_log, current_config.access_log) != 0 ||
        strcmp(fresh.error_log, current_config.error_log) != 0)
    {
        if (logger_reload(fresh.access_log, fresh.error_log) != 0)
        {
            log_error("reload: cannot reopen log files, keeping old ones");
            strcpy(fresh.access_log, current_config.access_log);
            strcpy(fresh.error_log, current_config.error_log);
        }
    }

    current_config = fresh;
    printf("configuration reloaded: port=%d root=%s\n",
           current_config.port,
           current_config.document_root);
    log_error("configuration reloaded: port=%d root=%s",
              current_config.port,
              current_config.document_root);
}

/* 创建监听 socket 和 epoll 实例，并运行服务器主事件循环。 */
int server_run(const ServerConfig *config, const char *config_path)
{
    server_config = &current_config;
    current_config = *config; // 拷贝一份：热加载时原地替换，调用方的 config 不再使用
    config_file = config_path;

    // 创建监听 socket
    int listener = create_listener(config->port);
    if (listener < 0)
    {
        perror("create listener");
        return -1;
    }

    // 创建 epoll 实例
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("epoll_create1");
        close(listener);
        return -1;
    }

    // 把 listener 加入 epoll 。
    // data.ptr = NULL 标记它是监听 socket，当data.ptr为NULL时，说明是listener
    // data.ptr 让事件处理变成 O(1) 且不需要经过数组查表这一步
    struct epoll_event listener_event = {.events = EPOLLIN, .data.ptr = NULL};

    // epoll_ctl(ADD, listener) — 告诉 epoll："帮我盯着这个 listener，有新连接就通知我。另外，这个 connection 指针我寄存在你这，通知我的时候还给我。"
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener, &listener_event) != 0)
    {
        perror("epoll_ctl");
        close(epoll_fd);
        close(listener);
        return -1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_server;// 指定处理函数
    sigemptyset(&action.sa_mask);

    // 注册信号处理，用户按 Ctrl+C 时 running 会被改成 0
    sigaction(SIGINT, &action, NULL);        // 注册 Ctrl+C
    sigaction(SIGTERM, &action, NULL);       // 注册 kill 命令默认发送的信号

    /* SIGHUP 只置重载标志，真正的重载在主循环里执行。 */
    action.sa_handler = reload_server;
    sigaction(SIGHUP, &action, NULL);        // 注册热加载信号

    /* 忽略SIGPIPE ，防止个别断线客户端干掉整个服务器 */
    signal(SIGPIPE, SIG_IGN);

    // 最后打印一行启动信息
    printf("%s listening on http://0.0.0.0:%d, root=%s\n",
           SERVER_NAME,
           config->port,
           config->document_root);

    struct epoll_event events[MAX_EVENTS];
    time_t last_cleanup = time(NULL);

    // 主循环：里面就一件事：epoll_wait 等事件
    while (running)
    {
        /* SIGHUP 触发热加载：重新读取配置文件并应用。 */
        if (reload_requested)
        {
            reload_requested = 0;
            apply_reload(&listener);
        }

        /* epoll_wait 用来等待、获取内核投递的 I/O 就绪事件 */
        int count = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000); // 阻塞等事件。1 秒超时不是白等的，它保证主循环每秒至少醒来一次，去清理空闲的 Keep-Alive 连接。
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            log_error("epoll_wait failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < count; ++i)
        {
            // 当某个 fd 就绪时，epoll_wait 把事件填进 events 数组。  这一行就是从内核拿回当初寄存的那个指针
            Connection *connection = events[i].data.ptr;

            // 为什么 NULL 一定是 listener 因为 server_run 注册 listener 时，故意传了 NULL
            if (connection == NULL) // ptr==NULL → 新连接来了，accept_connections()
            {
                /* 监听 socket 可读表示至少有一个新连接等待 accept。 */
                accept_connections(listener);
                continue;
            }
            uint32_t flags = events[i].events;
            int result = 0;
            if (connection->state == CONN_RUNNING_CGI)
            {
                /* CGI 运行中：事件可能来自 stdout 管道（输出/EOF），也可能来自
                   客户端 socket（此时读管道只会得到 EAGAIN，无害）。
                   管道上的 EPOLLHUP 是子进程关闭写端的正常现象，不是连接错误。 */
                if (flags & (EPOLLIN | EPOLLERR | EPOLLHUP))
                    result = handle_cgi_read(connection);
                if (flags & EPOLLRDHUP)
                    connection->close_after_response = true;
            }
            else
            {
                if (flags & (EPOLLERR | EPOLLHUP))
                    result = -1;

                // 客户端有数据（EPOLLIN）→ handle_read()（读请求头或 POST 请求体）
                else if ((flags & EPOLLIN) && (connection->state == CONN_READING ||
                                               connection->state == CONN_READING_BODY))
                    result = handle_read(connection);

                if (flags & EPOLLRDHUP)
                    connection->close_after_response = true;

                // 客户端可写（EPOLLOUT）→ handle_write()
                // CONN_READING_BODY 阶段只注册 EPOLLIN，这里做防御性排除
                if (result == 0 && (flags & EPOLLOUT) && connection->state != CONN_READING &&
                    connection->state != CONN_READING_BODY)
                    result = handle_write(connection);
            }

            // 出错/断开 → close_connection()
            if (result != 0)
                close_connection(connection);
        }
        if (time(NULL) != last_cleanup)
        {
            close_idle_connections();//每秒清理一次超时空闲连接;
            last_cleanup = time(NULL);
        }
    }

    /* 收到 SIGINT 或 SIGTERM 后，统一释放所有连接和核心文件描述符。 */
    for (int fd = 0; fd < MAX_CONNECTIONS; ++fd)
        if (connections[fd] != NULL)
            close_connection(connections[fd]);
    // 退出清理，关闭所有连接
    close(epoll_fd);
    close(listener);
    return 0;
}
