#include "config.h"
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
    printf("Usage: %s [-p port] [-r document_root] [-t timeout] [-c config]\n", program);
    printf("  -p port           Listen port (default: 8080)\n");
    printf("  -r document_root  Static file directory (default: ./www)\n");
    printf("  -t seconds        Keep-Alive timeout (default: 10)\n");
    printf("  -c config         Config file (default: ./minihttpd.conf)\n");
}

int main(int argc, char **argv)
{
    /* 先建立默认配置，保存服务器启动所需的配置。再使用配置文件、命令行参数逐层覆盖。 */
    ServerConfig config = {
        .port = DEFAULT_PORT,
        .document_root = "./www",
        .access_log = "./logs/access.log",
        .error_log = "./logs/error.log",
        .keepalive_timeout = DEFAULT_KEEPALIVE_TIMEOUT,
    };
    int option;

    /*
     * 第一遍解析只找 -c：配置优先级是 命令行参数 > 配置文件 > 默认值，
     * 所以配置文件必须先加载，之后第二遍解析再用命令行参数覆盖。
     * getopt 的游标 optind 复位后可以重新解析一遍 argv。
     */
    const char *config_path = DEFAULT_CONFIG_PATH;
    while ((option = getopt(argc, argv, "p:r:t:c:h")) != -1)
    {
        if (option == 'c')
            config_path = optarg;
        else if (option == 'h')
        {
            usage(argv[0]);
            return EXIT_SUCCESS;
        }
        else if (option == '?')
        {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    optind = 1;

    /*
     * 配置文件不存在（-1）时继续用默认值；内容错误（-2）则拒绝启动，
     * 防止服务器带着一份残缺配置运行。
     */
    int config_result = config_parse_file(config_path, &config);
    if (config_result == -2)
    {
        fprintf(stderr, "Cannot parse config file: %s\n", config_path);
        return EXIT_FAILURE;
    }
    if (config_result == -1)
        fprintf(stderr, "Config file %s not found, using defaults.\n", config_path);

    /* -p 指定端口，-r 指定网站根目录，-t 指定长连接超时，-c 指定配置文件。 */
    // 循环每次拿到一个选项字符，就用 switch 分发
    while ((option = getopt(argc, argv, "p:r:t:c:h")) != -1)
    // getopt 会把参数赋值给optarg
    {
        switch (option)
        {
        case 'p':
            if (config_parse_number(optarg, 1, 65535, &config.port) != 0)
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
            if (config_parse_number(optarg, 1, 3600, &config.keepalive_timeout) != 0)
            {
                fprintf(stderr, "Invalid timeout: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'c':
            break; // 第一遍解析已经处理过。

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

    /* 上传目录和 CGI 脚本目录不存在时创建；已经存在不属于错误。 */
    char sub_dir[PATH_MAX];
    if (snprintf(sub_dir, sizeof(sub_dir), "%s/uploads", config.document_root) >=
        (int)sizeof(sub_dir))
    {
        fprintf(stderr, "Document root is too long.\n");
        return EXIT_FAILURE;
    }
    if (mkdir(sub_dir, 0755) != 0 && errno != EEXIST)
    {
        perror("create uploads directory");
        return EXIT_FAILURE;
    }
    if (snprintf(sub_dir, sizeof(sub_dir), "%s/cgi-bin", config.document_root) >=
        (int)sizeof(sub_dir))
    {
        fprintf(stderr, "Document root is too long.\n");
        return EXIT_FAILURE;
    }
    if (mkdir(sub_dir, 0755) != 0 && errno != EEXIST)
    {
        perror("create cgi-bin directory");
        return EXIT_FAILURE;
    }

    // 初始化日志：logger_init() 会以追加模式打开两个文件
    if (logger_init(config.access_log, config.error_log) != 0)
    {
        fprintf(stderr, "Cannot initialize log files.\n");
        return EXIT_FAILURE;
    }

    /* server_run() 进入 epoll 事件循环，直到收到退出信号。 */
    int result = server_run(&config, config_path); //server_run() 返回后，服务器已经停止
    logger_close(); //关闭日志access.log和error.log

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
