#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 将字符串严格转换为指定范围内的整数。
 * 返回 0 表示成功，返回 -1 表示格式错误、溢出或超出范围。
 * main.c 的 -p/-t 和本文件的 port/keepalive_timeout 共用此函数。
 */
int config_parse_number(const char *text, int minimum, int maximum, int *result)
{
    char *end;
    long value;

    errno = 0; // strtol 不会主动把 errno 清零——它只在发生溢出时把 errno 设为 ERANGE。errno不等于0时为溢出
    value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value < minimum || value > maximum)
    {
        return -1;
    }

    *result = (int)value; // result 解引用
    return 0;             // 调用成功
}

/* 去掉一行两端的空格和制表符。 */
static char *trim_line(char *text)
{
    while (*text == ' ' || *text == '\t')
        ++text;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    *end = '\0';
    return text;
}

/* 读一行并去掉行尾换行（兼容 \n 和 \r\n），返回 0 成功，-1 到文件末尾。 */
static int read_line(FILE *file, char *line, size_t size)
{
    if (fgets(line, (int)size, file) == NULL)
        return -1;
    line[strcspn(line, "\r\n")] = '\0';
    return 0;
}

/* 把字符串值复制到定长字符数组字段，超长时报错并返回 -1。 */
static int copy_path_field(char *destination,
                           size_t size,
                           const char *key,
                           const char *value,
                           const char *path,
                           int line_number)
{
    if (strlen(value) >= size)
    {
        fprintf(stderr, "%s:%d: %s is too long\n", path, line_number, key);
        return -1;
    }
    strcpy(destination, value);
    return 0;
}

int config_parse_file(const char *path, ServerConfig *config)
{
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return -1;

    int failed = 0;
    int line_number = 0;
    char line[1024];
    while (read_line(file, line, sizeof(line)) == 0)
    {
        ++line_number;

        char *text = trim_line(line);
        if (*text == '\0' || *text == '#')
            continue; // 空行和注释行

        /* 按第一个空白字符拆成 key 和 value，value 内部允许再出现空格（如路径）。 */
        char *space = strpbrk(text, " \t");
        if (space == NULL)
        {
            fprintf(stderr, "%s:%d: missing value for '%s'\n", path, line_number, text);
            failed = 1;
            continue;
        }
        *space = '\0';
        char *key = text;
        char *value = trim_line(space + 1);
        if (*value == '\0')
        {
            fprintf(stderr, "%s:%d: missing value for '%s'\n", path, line_number, key);
            failed = 1;
            continue;
        }

        if (strcmp(key, "port") == 0)
        {
            if (config_parse_number(value, 1, 65535, &config->port) != 0)
            {
                fprintf(stderr, "%s:%d: invalid port '%s'\n", path, line_number, value);
                failed = 1;
            }
        }
        else if (strcmp(key, "document_root") == 0)
        {
            if (copy_path_field(config->document_root, sizeof(config->document_root), key,
                                value, path, line_number) != 0)
                failed = 1;
        }
        else if (strcmp(key, "access_log") == 0)
        {
            if (copy_path_field(config->access_log, sizeof(config->access_log), key, value, path,
                                line_number) != 0)
                failed = 1;
        }
        else if (strcmp(key, "error_log") == 0)
        {
            if (copy_path_field(config->error_log, sizeof(config->error_log), key, value, path,
                                line_number) != 0)
                failed = 1;
        }
        else if (strcmp(key, "keepalive_timeout") == 0)
        {
            if (config_parse_number(value, 1, 3600, &config->keepalive_timeout) != 0)
            {
                fprintf(stderr, "%s:%d: invalid keepalive_timeout '%s'\n", path, line_number,
                        value);
                failed = 1;
            }
        }
        else
        {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, line_number, key);
            failed = 1;
        }
    }

    fclose(file);
    return failed ? -2 : 0;
}
