#!/bin/bash
# CGI 测试脚本：把 stdin（POST 请求体）原样回显。
printf 'Content-Type: text/plain; charset=utf-8\r\n\r\n'
cat
