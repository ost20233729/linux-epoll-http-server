#ifndef MINIHTTPD_SERVER_H
#define MINIHTTPD_SERVER_H
#include "common.h"
/* 运行服务器：进入 epoll 事件循环直到收到退出信号。config_path 用于 SIGHUP 热加载。 */
int server_run(const ServerConfig *config, const char *config_path);
#endif
