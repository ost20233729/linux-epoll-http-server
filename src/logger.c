#include "logger.h"
#include <stdarg.h>
#include <stdio.h>

//FILE 是文件描述符的用户态包装，内部保存着一个内核层的文件描述符。用来表示一个已经打开的文件，可以把它理解成一个"文件操作手柄"
/*
为什么需要两层？
因为裸的 read()/write() 每次都要进内核，很慢。
FILE 层加了一个用户态缓冲区，攒一批再进内核，性能好很多。
代价是多了格式化、缓冲这套包装。

用户态操作与内核态操作的区别：
FILE *f = fopen("log", "a");   // FILE *，用 fprintf / fclose
int  fd = open("log", O_APPEND);   // int 描述符，用 write / close
*/

//static 表示这个变量只在：logger.c中可见。其他文件不能直接这样使用：
/*
    文件作用域的静态变量如果没有显式初始化，会自动初始化为 0。
    对于指针来说就是：
    NULL
    所以程序刚启动时：
    access_file = NULL
    error_file  = NULL
    调用 logger_init() 后，它们才指向打开的文件。
*/
static FILE *error_file;
static FILE *access_file;

/* 以追加模式打开日志，避免服务器重启时覆盖已有记录。 */
int logger_init(const char *access_path, const char *error_path)
{
    access_file = fopen(access_path, "a");
    if (access_file == NULL)
        return -1;
    error_file = fopen(error_path, "a");
    if (error_file == NULL)
    {
        fclose(access_file); // 第二个文件打开失败时的清理第一个
        access_file = NULL;
        return -1;
    }

    /*
    使用文件内容缓冲
    stdio 的做法是：先把数据攒在内存里，攒够一批再一次性交给内核写磁盘。这个内存区就叫"缓冲区"。
    日志文件要实时可见，所以选行缓冲。
    */
    setvbuf(access_file, NULL, _IOLBF, 0);//行缓冲 _IOLBF：遇到 \n 就写
    setvbuf(error_file, NULL, _IOLBF, 0);
    return 0;
}

/*
 * 运行中切换日志文件（SIGHUP 热加载调用）。
 * 先打开两个新文件，全部成功后才关闭旧文件并替换指针：
 * 任何一步失败都保留旧日志，保证日志记录不中断。
 */
int logger_reload(const char *access_path, const char *error_path)
{
    FILE *new_access = fopen(access_path, "a");
    if (new_access == NULL)
        return -1;
    FILE *new_error = fopen(error_path, "a");
    if (new_error == NULL)
    {
        fclose(new_access);
        return -1;
    }
    setvbuf(new_access, NULL, _IOLBF, 0);
    setvbuf(new_error, NULL, _IOLBF, 0);

    if (access_file != NULL)
        fclose(access_file);
    if (error_file != NULL)
        fclose(error_file);
    access_file = new_access;
    error_file = new_error;
    return 0;
}

/* 关闭两个日志文件，由 main() 在服务器退出后调用。 */
void logger_close(void)
{
    if (access_file != NULL)
        fclose(access_file);
    if (error_file != NULL)
        fclose(error_file);
}

/* 把当前本地时间格式化为日志中使用的时间字符串。 */
static void timestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local); // _r 后缀 = reentrant（可重入），即线程安全版
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S %z", &local);
}

/* 每完成一次 HTTP 响应，记录客户端、请求、状态码和响应长度。 */
void log_access(const Connection *connection)//const Connection *  只能读取，不能修改连接结构体
{
    if (access_file == NULL)
        return;
    char stamp[64];
    timestamp(stamp, sizeof(stamp));
    // 进行字符转换：request.method 是 HttpMethod 枚举（整数），而 fprintf 要的是字符串，所以转换
    const char *method = connection->request.method == METHOD_GET    ? "GET"
                         : connection->request.method == METHOD_HEAD ? "HEAD"
                         : connection->request.method == METHOD_POST ? "POST"
                                                                     : "UNKNOWN";
    fprintf(access_file,
            "%s [%s] \"%s %s HTTP/1.%d\" %d %lld %s\n",
            connection->client_ip,
            stamp,
            method,
            connection->request.target,
            connection->request.http_minor,
            connection->status_code,
            (long long)connection->response_bytes,
            connection->keep_alive ? "keep-alive" : "close");
}

/* 支持 printf 风格参数的错误日志；日志未初始化时退回 stderr。 */
void log_error(const char *format, ...)// ...为可变参数，必须放在所有参数的末尾，且前面至少要有一个固定参数（这里是 format）
{
    FILE *stream = error_file != NULL ? error_file : stderr;//error_file 没打开（日志初始化失败）时，错误信息写到标准错误输出
    char stamp[64];
    timestamp(stamp, sizeof(stamp));
    fprintf(stream, "[%s] ", stamp); // 写入一个时间戳
    va_list args;              // 1. 声明一个"参数读取器"
    va_start(args, format);    // 2. 初始化：从 format 后面开始读取
    // vfprintf 是"拿 va_list 干活"的 fprintf
    vfprintf(stream, format, args);  // 3. 把args交给 vfprintf 按 format 逐一取出
    va_end(args);              // 4. 清理，必须成对出现
    fputc('\n', stream);// 补一个换行
}
