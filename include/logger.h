#ifndef MINIHTTPD_LOGGER_H
#define MINIHTTPD_LOGGER_H
#include "common.h"
int logger_init(const char *access_path, const char *error_path);
/* 热加载时切换日志文件：全部打开成功才替换，失败时保留旧文件。 */
int logger_reload(const char *access_path, const char *error_path);
void logger_close(void);
void log_access(const Connection *connection);
void log_error(const char *format, ...);
#endif
