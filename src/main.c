#include "logger.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 打印命令行参数说明。 */
// static 表示这个函数只在本文件内部使用
static void usage(const char *program)
{
    printf("Usage: %s [-p port] [-r document_root] [-t timeout]\n", program);
    printf("  -p port           Listen port (default: 8080)\n");
    printf("  -r document_root  Static file directory (default: ./www)\n");
    printf("  -t seconds        Keep-Alive timeout (default: 10)\n");
}

/*
 * 将字符串严格转换为指定范围内的整数。
 * 返回 0 表示成功，返回 -1 表示格式错误、溢出或超出范围。
 */
static int parse_number(const char *text, int minimum, int maximum, int *result)
{
    char *end;
    long value;

    errno = 0; // strtol 不会主动把 errno 清零——它只在发生溢出时把 errno 设为 ERANGE。errno不等于0时为溢出
    value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value < minimum || value > maximum)
    {
        return -1;
    }

    *result = (int)value;// result 解引用
    return 0;  // 调用成功
}

int main(int argc, char **argv)
{
    /* 先建立默认配置，保存服务器启动所需的配置。再使用命令行参数覆盖其中的字段。 */
    ServerConfig config = {
        .port = DEFAULT_PORT,
        .document_root = "./www",
        .access_log = "./logs/access.log",
        .error_log = "./logs/error.log",
        .keepalive_timeout = DEFAULT_KEEPALIVE_TIMEOUT,
    };
    int option;

    /* -p 指定端口，-r 指定网站根目录，-t 指定长连接超时。 */
    // 循环每次拿到一个选项字符，就用 switch 分发
    while ((option = getopt(argc, argv, "p:r:t:h")) != -1)
    // getopt 会把参数赋值给optarg
    {
        switch (option)
        {
        case 'p':
            if (parse_number(optarg, 1, 65535, &config.port) != 0)
            {
                // stderr 是标准错误输出
                fprintf(stderr, "Invalid port: %s\n", optarg);
                return EXIT_FAILURE; // 立即结束 main()，并告诉操作系统程序启动失败
            }
            break; // 离开 switch，继续解析其他参数。

        case 'r':
            if (strlen(optarg) >= sizeof(config.document_root))
            {
                fprintf(stderr, "Document root is too long.\n");
                return EXIT_FAILURE;
            }
            strcpy(config.document_root, optarg);
            break;

        case 't':
            if (parse_number(optarg, 1, 3600, &config.keepalive_timeout) != 0)
            {
                fprintf(stderr, "Invalid timeout: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;

        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /*
     * 将网站根目录转换为绝对规范路径。
     * 后续处理请求时会用它判断真实文件是否仍位于网站根目录内。
     */
    // 把真实路径保存到resolved里面
    char resolved[PATH_MAX];
    if (realpath(config.document_root, resolved) == NULL)
    {
        perror("document root");
        return EXIT_FAILURE;
    }

    // 获取目录信息
    struct stat info;
    // stat(resolved, &info)表示获取 resolved 对应资源的信息，并写入 info。
    // S_ISDIR() 判断资源是不是目录。如果不是目录，S_ISDIR() 返回假
    if (stat(resolved, &info) != 0 || !S_ISDIR(info.st_mode))
    {
        fprintf(stderr, "Document root is not a directory.\n");
        return EXIT_FAILURE;
    }

    // 保存规范的绝对路径
    strcpy(config.document_root, resolved);

    /* 日志目录不存在时创建；已经存在不属于错误。 */
    // 这里 "logs" 是一个相对路径，创建在哪取决于你在哪个目录下运行程序。
    if (mkdir("logs", 0755) != 0 && errno != EEXIST)
    {
        perror("create logs directory");
        return EXIT_FAILURE;
    }

    // 初始化日志：logger_init() 会以追加模式打开两个文件
    if (logger_init(config.access_log, config.error_log) != 0)
    {
        fprintf(stderr, "Cannot initialize log files.\n");
        return EXIT_FAILURE;
    }

    /* server_run() 进入 epoll 事件循环，直到收到退出信号。 */
    int result = server_run(&config);  //server_run() 返回后，服务器已经停止
    logger_close(); //关闭日志access.log和error.log

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
