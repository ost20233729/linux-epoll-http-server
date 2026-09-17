#!/bin/bash
# CGI 测试脚本：休眠超过服务器超时时间（5 秒），用于验证 504 处理。
sleep 10
printf 'Content-Type: text/plain\r\n\r\n'
printf 'This should never be sent\n'
