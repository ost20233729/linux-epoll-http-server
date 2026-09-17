#include "http.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * HTTP 请求头以空行结束，即字节序列 \r\n\r\n。
 * 找到后返回 1，并通过 request_length 给出完整请求头长度。
 * buffer：接收缓冲区；length：已收到的字节数；request_length：输出参数指针
 */
int http_find_request_end(const char *buffer, size_t length, size_t *request_length)
{
    for (size_t i = 0; i + 3 < length; ++i)
    {
        if (memcmp(buffer + i, "\r\n\r\n", 4) == 0)
        {
            *request_length = i + 4;//i 是 \r\n\r\n 中第一个 \r 的位置。i + 4 = 跳过这四个字节，即完整请求头包含结束标记的总长度。
            return 1;//找到了
        }
    }
    return 0;
}

/* 去掉请求头名称和值两侧的空格和制表符。

GET /index.html HTTP/1.1       ← 请求行
Host: localhost                 ← 请求头（多行，每行格式为 "名称: 值"）
Connection:    keep-alive         ← 注意值前面可能有空格
Range: bytes=0-99
                                ← 空行（\r\n\r\n，请求头结束）
[这里是请求体，GET 请求没有]      ← POST 才有
*/
static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t')//*text 就是 text[0] 指针指向的那个字符
        ++text;
    char *end = text + strlen(text);//strlen(text) 从 text（当前指向 'k'）开始数到 \0 ，end最后指向\0
    while (end > text && (end[-1] == ' ' || end[-1] == '\t'))//end[-1] 就是 *(end - 1)——end 左边的那个字符
        --end;
    *end = '\0';
    return text;
}

/*
 * 解析单段 Range 请求头，支持 bytes=N-M、bytes=N- 和 bytes=-N。
 * 功能：它只负责把字符串中的数字保存到 HttpRequest，此时还不知道具体文件有多大。
 *
    GET /video.mp4 HTTP/1.1        ← 请求行
    Host: example.com               ← 请求头 1
    Range: bytes=0-1048575          ← 请求头 2（这一行就是 Range）
    Connection: keep-alive          ← 请求头 3
                                ← 空行（\r\n\r\n）
                                ← 请求体（GET 请求没有体）
 */
static int parse_range(const char *value, HttpRequest *request)
{
    // 拒绝不支持的格式
    // strncasecmp 功能： 逐个字符比较 s1 和 s2 的前 n 个字符
    // strncasecmp 函数名拆解： str（字符串） + n（限定比较个数） + case（大小写） + cmp（比较）
    // strchr的功能是从第一个参数的位置开始查找第二个参数是否出现，返回值为NULL时，表示range的值中没有逗号，不是多段range，可以进行后续操作
    if (strncasecmp(value, "bytes=", 6) != 0 || strchr(value + 6, ',') != NULL)
        return -1;
    // 跳过 "bytes=" 这 6 个字符指针后移 6 字节
    // "bytes=0-99" → number 指向 '0'
    // "bytes=-200" → number 指向 '-'   "bytes=0-99" → number 指向 "0-99"
    const char *number = value + 6;
    // char *end：strtoll 用 end 存"数字解析到哪停了"的位置
    char *end;
    //  先对C 标准库的全局错误码变量清零。因为库函数成功时不主动清零，所以调用前必须手动清零
    //  如果 errno 非零，说明库函数执行时出错了， strtoll 溢出了
    errno = 0;

    // 后缀范围 bytes=-N
    if (*number == '-')
    {
        // strtoll 功能：str（字符串） to（转成） l（long） l（long），字符串转 64 位整数
        // number + 1：跳过 '-'，从后面的数字开始解析  "-200" → 从 '2' 开始
        // &end：strtoll 把解析停止位置写入 end（输出参数，传地址）  end 指向 \0 则*end = '\0' = 0（假）
        // 10：十进制
        // 传入的是空字符串时，suffix = 0 ；
        long long suffix = strtoll(number + 1, &end, 10);

        if (errno || *end || suffix <= 0)
            return -1;

        // 填写 HttpRequest 的四个 Range 字段
        request->range_suffix = true;        // 标记：这是后缀范围，默认为false，也就是默认是bytes=N-M 或 bytes=N-这种范围
        request->range_start = (off_t)suffix; // 此时不知道真实文件大小，先把后缀长度 200 存进 range_start ，后续在以此计算出实际start的值
        request->range_end = -1;              // 此时还不知道文件大小，暂填-1
        /*
        // http_parse_range() 内部，处理后缀范围的分支：
        if (request->range_suffix)
        {
            // 此时 file_size 已知，比如 1000
            // range_start 还是 200（后缀长度）
            request->range_start = file_size - request->range_start;  // 1000 - 200 = 800
            // range_end 从 -1 替换为 file_size - 1 = 999
        }
        // 最终：start=800, end=999，返回文件最后 200 字节
        */
    }

    // 正常范围 bytes=N-M 或 bytes=N-
    else
    {
        long long start = strtoll(number, &end, 10);
        // strtoll对于N-M这个，是只解析到 - 就停止
        if (errno || end == number || start < 0 || *end != '-')
            return -1;
        request->range_start = (off_t)start;
        request->range_end = -1;
        if (end[1] != '\0')
        {
            char *final;
            long long last = strtoll(end + 1, &final, 10);
            if (errno || *final || last < start)
                return -1;
            request->range_end = (off_t)last;
        }
    }
    request->has_range = true;// 在prepare_file()中，has_range 为 false → 返回整个文件，不走 Range 分支
    return 0;
}

/*
 * 将原始 HTTP 请求头解析为 HttpRequest。
 * 返回值：0 成功，-1 格式错误，-2 HTTP 版本不支持，-3 Range 非法。
 * 参数：buffer：接收缓冲区中的请求头数据；length：请求头长度；request：要填充的 HttpRequest 指针
 */
int http_parse_request(const char *buffer, size_t length, HttpRequest *request)
{
    // 空请求或超长请求，直接拒绝
    if (length == 0 || length >= READ_BUFFER_SIZE)
        return -1;

    // sscanf 和 strstr 需要可写的字符串，所以拷贝一份到局部数组，末尾手动补 '\0'
    char copy[READ_BUFFER_SIZE];
    memcpy(copy, buffer, length);
    copy[length] = '\0';

    // memset 把 request 所有字段清零
    // sizeof(*request) = sizeof(HttpRequest)，不用手动算
    // Keep-Alive 复用连接时，request 里残留上次请求的值，必须清掉
    memset(request, 0, sizeof(*request));

    /* 先解析请求行：METHOD TARGET HTTP/VERSION。 */
    // strstr 在 copy 中查找 "\r\n"，返回第一个匹配位置的指针
    // 请求行以 \r\n 结束，找到后在该位置写 '\0' 截断
    // 截断后 copy 指向的字符串就是 "GET /index.html HTTP/1.1"
    char *line_end = strstr(copy, "\r\n");
    if (line_end == NULL)
        return -1;
    *line_end = '\0';

    // sscanf 从 copy 中按格式拆出 method、target、version
    // %15s：最多读 15 个字符，防止 method 数组溢出
    // %4095s：target 对应 HttpRequest.target[PATH_MAX]，4096-1=4095
    // %15s：同上，读 version
    // %c 读到 extra：如果还有第五个字段，说明请求行多了不该有的东西
    // 只有 3 个字段时，%c 读不到任何字符，sscanf 在第三个 %s 匹配完后遇到 \0 就停了，返回 3
    // 返回值 != 3 = 请求行只拆出 2 个或更少字段，格式错误
    char method[16], version[16], extra;
    if (sscanf(copy, "%15s %4095s %15s %c", method, request->target, version, &extra) != 3)
        return -1;

    if (strcmp(method, "GET") == 0)
        request->method = METHOD_GET;
    else if (strcmp(method, "HEAD") == 0)
        request->method = METHOD_HEAD;
    else if (strcmp(method, "POST") == 0)
        request->method = METHOD_POST;
    else
        request->method = METHOD_UNSUPPORTED;

    // HTTP/1.1 默认长连接，发完响应不立即断开，等着下一个请求
    // request->keep_alive = true;
    if (strcmp(version, "HTTP/1.1") == 0)
    {
        request->http_minor = 1; // HTTP 小版本号
        request->keep_alive = true;
    }
    // HTTP/1.0 默认短连接，发完响应就关闭
    // request->keep_alive = false;
    else if (strcmp(version, "HTTP/1.0") == 0)
    {
        request->http_minor = 0;
        request->keep_alive = false;
    }
    // HTTP 版本不支持
    else
        return -2;

    // 请求路径必须以 / 开头
    if (request->target[0] != '/')
        return -1;

    /* 逐行解析请求头；当前业务只关心 Connection 和 Range。 */

    // line_end + 2 跳过请求行末尾的 \r\n，指向第一行请求头
    // cursor 是当前正在处理的请求头行的起点
    char *cursor = line_end + 2;

    // 遇到空行（\r\n\r\n 的后半段）→ 请求头结束，退出循环
    // cursor[0] == '\r' && cursor[1] == '\n' 即当前行是空行
    while (!(cursor[0] == '\r' && cursor[1] == '\n'))
    {
        // 找当前行的 \r\n，截断
        line_end = strstr(cursor, "\r\n");
        if (line_end == NULL)
            return -1;
        *line_end = '\0';

        // strchr 找 ':'，按冒号把一行拆成"名称"和"值"
        // "Connection: keep-alive" → 名称在冒号左边，值在右边
        char *colon = strchr(cursor, ':');//这里为什么不用strstr而是使用strchr？strchr 只比对一个字符，内部循环非常简单——走一步，当前字节 == ':' ？是就返回，不是继续走。strstr 是在字符串里找另一个字符串，内部有两层循环，对于找单字符这种场景完全是多余的。
        if (colon == NULL)
            return -1;      // 没有冒号，非法请求头行
        *colon = '\0';       // 冒号位置写 '\0'，把名称和值切开

        // trim() 去掉名称和值两端的空格、\t、\r
        char *name = trim(cursor);
        char *value = trim(colon + 1);

        if (strcasecmp(name, "Connection") == 0)
        {
            if (strcasecmp(value, "close") == 0)
                request->keep_alive = false;
            else if (strcasecmp(value, "keep-alive") == 0)
                request->keep_alive = true;
        }
        else if (strcasecmp(name, "Content-Length") == 0)
        {
            // 与 parse_range 同理：strtoll 调用前先清 errno，再用 *end 检查是否完整解析
            errno = 0;
            char *end;
            long long body = strtoll(value, &end, 10);
            if (errno || *end || body < 0)
                return -1;
            request->content_length = (off_t)body;
            request->has_content_length = true;
            request->has_body = body > 0;
        }
        else if (strcasecmp(name, "Content-Type") == 0)
            snprintf(request->content_type, sizeof(request->content_type), "%s", value);
        else if (strcasecmp(name, "Transfer-Encoding") == 0 && strcasecmp(value, "chunked") == 0)
            request->chunked = true;
        else if (strcasecmp(name, "Range") == 0 && parse_range(value, request) != 0)
            return -3;

        cursor = line_end + 2;
    }
    return 0;
}

/* 根据状态码返回 HTTP 原因短语，用于响应状态行。 */
const char *http_reason_phrase(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 206:
        return "Partial Content";
    case 301:
        return "Moved Permanently";
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 416:
        return "Range Not Satisfiable";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 504:
        return "Gateway Timeout";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Unknown";
    }
}

/* 根据文件扩展名选择 Content-Type，未知类型按二进制流处理。 */
const char *http_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";
    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm"))
        return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".css"))
        return "text/css; charset=utf-8";
    if (!strcasecmp(ext, ".js"))
        return "application/javascript; charset=utf-8";
    if (!strcasecmp(ext, ".json"))
        return "application/json; charset=utf-8";
    if (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".md"))
        return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".png"))
        return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(ext, ".gif"))
        return "image/gif";
    if (!strcasecmp(ext, ".svg"))
        return "image/svg+xml";
    if (!strcasecmp(ext, ".pdf"))
        return "application/pdf";
    if (!strcasecmp(ext, ".zip"))
        return "application/zip";
    if (!strcasecmp(ext, ".mp4"))
        return "video/mp4";
    return "application/octet-stream";
}

/* 把一个十六进制字符转换为数值，非法字符返回 -1。 */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    // 16进制中的a等于十进制中的10，所以要加10
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/*
 * 解码 URL 路径中的 %XX，并忽略 ? 后的查询字符串。
 * 解码后再检查 ..，防止使用 %2e%2e 绕过路径穿越检测。
 */
int http_url_decode_path(const char *target, char *decoded, size_t decoded_size)
{
    // used 跟踪已写入 decoded 的字节数，i 跟踪正在读取 target 的位置
    size_t used = 0;

    // 原始请求：GET /my%20file.txt HTTP/1.1
    // 条件：target[i]  ： 当前字符非 \0 。C 语言用它标记字符串结束。 \0 就是 ASCII 值为 0 的字符，在if中为假）。
    // 条件：target[i] != '?'  ： 遇到 '?' 就停止，后面是查询字符串（?key=value），不是路径的一部分，直接忽略
    for (size_t i = 0; target[i] && target[i] != '?'; ++i)
    {
        // (unsigned char) 强转，防止 char 为负值（大于 127 的字节）时下标或比较出错
        unsigned char c = (unsigned char)target[i];

        if (c == '%')
        {
            int high = hex_value(target[i + 1]), low = hex_value(target[i + 2]);
            if (high < 0 || low < 0)
                return -1;

            // (high << 4) | low 把两个 4 位值拼成一个字节
            // high=2 → 0010，左移 4 位 → 0010 0000
            // low=0  → 0000 0000
            // 按位或 → 0010 0000 = 0x20 = 32 = 空格
            // 最后的 unsigned char 强转是为了消除编译器警告——位运算的结果默认提升为 int 类型，赋给 unsigned char 时需要显式转一下。
            c = (unsigned char)((high << 4) | low);

            i += 2;
        }

        // 拒绝三类危险字符：
        // c == 0：空字符（刻意构造的攻击，\0 不应出现在 URL 中）
        // c == '\\'：反斜杠（Windows 路径分隔符，Linux 不安全）
        // c < 32：控制字符（\r、\n、\t 等，ASCII 0~31）
        if (c == 0 || c == '\\' || c < 32 || used + 1 >= decoded_size)
            return -1;

        // 字符合法，写入输出缓冲区
        decoded[used++] = (char)c;
    }
    decoded[used] = '\0';

    char copy[PATH_MAX];
    if (used >= sizeof(copy))
        return -1;
    memcpy(copy, decoded, used + 1);//把 decoded 里写好的 used+1 个字节（包括结尾的 \0）复制到 copy
    char *save;
    for (char *part = strtok_r(copy, "/", &save); part;part = strtok_r(NULL, "/", &save))
    {
        if (!strcmp(part, ".."))
            return -2;
    }
    return 0;
}

/*
 * 将请求中的 Range 转换为文件的实际闭区间 [start, end]。
 * 该函数需要文件大小，才能处理 bytes=-N 和超过文件末尾的情况。
 */
int http_parse_range(const HttpRequest *request, off_t file_size, off_t *start, off_t *end)
{
    // 分支1：没有 Range 头 → 返回整个文件
    if (!request->has_range)
    {
        *start = 0;
        *end = file_size > 0 ? file_size - 1 : 0;
        return 0;
    }

    if (file_size <= 0)
        return -1;

    if (request->range_suffix)// 后缀型
    {
        // count 要和 file_size 取 min——要最后 200 字节但文件只有 100，count 修正为 100
        off_t count = request->range_start > file_size ? file_size : request->range_start;
        *start = file_size - count;
        *end = file_size - 1;
    }
    else
    {
        if (request->range_start >= file_size)
            return -1;
        *start = request->range_start;
        *end = request->range_end < 0 || request->range_end >= file_size ? file_size - 1 : request->range_end;
    }
    return 0;
}

/* 转义目录页面中的文件名，防止文件名被浏览器当作 HTML 标签。 */
int http_escape_html(const char *input, char *output, size_t size)
{
    size_t used = 0;
    for (; *input; ++input)
    {
        const char *text = *input == '&'   ? "&amp;"
                           : *input == '<' ? "&lt;"
                           : *input == '>' ? "&gt;"
                           : *input == '"' ? "&quot;"
                                           : NULL;

        // // text 非 NULL：要写入的是实体字符串，用 strlen 取长度
        // text == NULL：要写入的是原始字符，长度为 1
        size_t length = text ? strlen(text) : 1;

        // // 写入前检查缓冲区是否够用，used + length >= size 会溢出（因为还要留 \0）
        if (used + length >= size)
            return -1;

        if (text)
            memcpy(output + used, text, length);// 拷入 HTML 实体
        else
            output[used] = *input;
        used += length;
    }
    output[used] = '\0';
    return 0;
}

/* 把文件名转换成合法 URL。对目录链接中的文件名进行百分号编码，生成合法 URL。 */
int http_url_encode(const char *input, char *output, size_t size)
{
    // hex 数组：用下标取值，免去调 hex_value
    // hex[0]='0', hex[10]='A', hex[15]='F'
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (; *input; ++input)
    {
        unsigned char c = (unsigned char)*input;

        // isalnum(c)：字母或数字为 true
        // '-' '_' '.' '~' 是 URL 的合法字符，不用编码
        bool safe = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';

        // 安全字符写 1 字节，非安全字符写 3 字节（%XX）
        if (used + (safe ? 1 : 3) >= size)
            return -1;

        if (safe)
            output[used++] = (char)c;
        else
        {
            output[used++] = '%';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15];
        }
    }
    output[used] = '\0';
    return 0;
}
