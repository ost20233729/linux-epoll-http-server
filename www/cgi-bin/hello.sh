#!/bin/bash
# CGI 测试脚本：回显请求方法和查询字符串（来自环境变量）。
printf 'Content-Type: text/plain; charset=utf-8\r\n\r\n'
printf 'Hello from CGI: method=%s query=%s\n' "$REQUEST_METHOD" "$QUERY_STRING"
