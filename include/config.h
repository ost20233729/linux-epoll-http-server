#ifndef MINIHTTPD_CONFIG_H
#define MINIHTTPD_CONFIG_H
#include "common.h"

/* 默认配置文件路径（相对当前工作目录）。 */
#define DEFAULT_CONFIG_PATH "./minihttpd.conf"

/* 将字符串严格转换为指定范围内的整数。返回 0 成功，-1 失败。 */
int config_parse_number(const char *text, int minimum, int maximum, int *result);

/*
 * 解析 "key value" 格式的配置文件，把读到的字段写入 config。
 * 未在文件中出现的字段保持原值不变，因此调用方可以先填默认值再调用。
 * 返回 0 成功；-1 文件无法打开；-2 内容错误（细节已打印到 stderr）。
 * 内容错误时返回 -2 而不是部分应用，保证调用方不会拿到残缺配置。
 */
int config_parse_file(const char *path, ServerConfig *config);
#endif
