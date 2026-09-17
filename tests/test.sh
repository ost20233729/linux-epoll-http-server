#!/usr/bin/env bash
set -euo pipefail
PORT="${PORT:-18080}"
BASE="http://127.0.0.1:${PORT}"

cleanup()
{
    kill "${SERVER_PID:-0}" 2>/dev/null || true
    wait "${SERVER_PID:-0}" 2>/dev/null || true
    kill "${RELOAD_PID:-0}" 2>/dev/null || true
    wait "${RELOAD_PID:-0}" 2>/dev/null || true
    rm -f www/uploads/upload-test.txt www/uploads/piped.txt
    rm -f /tmp/minihttpd-reload.conf logs/reload-access.log logs/reload-error.log
}

trap cleanup EXIT

./minihttpd -p "$PORT" -r ./www > /tmp/minihttpd-test.out 2>&1 &
SERVER_PID=$!

for _ in {1..30}
do
    curl -fsS "$BASE/" >/dev/null 2>&1 && break
    sleep 0.1
done

test "$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/")" = "200"
test "$(curl -sS -I -o /dev/null -w '%{http_code}' "$BASE/sample.txt")" = "200"
test "$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/missing")" = "404"
test "$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/downloads/")" = "200"
test "$(curl -sS -o /dev/null -w '%{http_code}' \
    -H 'Range: bytes=0-3' "$BASE/sample.txt")" = "206"
test "$(curl -sS -H 'Range: bytes=0-3' "$BASE/sample.txt")" = "This"
test "$(curl -sS -o /dev/null -w '%{http_code}' \
    --path-as-is "$BASE/../etc/passwd")" = "403"

# POST 上传：原始请求体写入 uploads/，再用 GET 读回校验内容。
test "$(curl -sS -o /dev/null -w '%{http_code}' \
    --data-binary 'hello upload' "$BASE/uploads/upload-test.txt")" = "201"
test "$(curl -sS "$BASE/uploads/upload-test.txt")" = "hello upload"

# 上传接口限制在 /uploads/ 子目录内。
test "$(curl -sS -o /dev/null -w '%{http_code}' \
    --data-binary 'x' "$BASE/sample.txt")" = "403"

# 缺少 Content-Length 的 POST 返回 411（裸 socket，curl 会自动补 Content-Length）。
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf '%b' 'POST /uploads/x.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' >&3
NO_LENGTH_RESPONSE="$(cat <&3)"
exec 3<&- 3>&-
test "$(grep -o 'HTTP/1.1 [0-9]*' <<<"$NO_LENGTH_RESPONSE" | head -1)" = "HTTP/1.1 411"

# 声明的长度超过 64 MiB 上限时，服务器不等待请求体直接返回 413。
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf '%b' 'POST /uploads/big.bin HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100000000\r\nConnection: close\r\n\r\n' >&3
OVERSIZE_RESPONSE="$(cat <&3)"
exec 3<&- 3>&-
test "$(grep -o 'HTTP/1.1 [0-9]*' <<<"$OVERSIZE_RESPONSE" | head -1)" = "HTTP/1.1 413"

# 同一连接上 POST 后紧跟 GET：请求体之后的数据必须留给下一个请求解析。
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf '%b' \
    'POST /uploads/piped.txt HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello' \
    'GET /uploads/piped.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' >&3
PIPELINED_UPLOAD="$(cat <&3)"
exec 3<&- 3>&-
test "$(grep -c 'HTTP/1.1 201' <<<"$PIPELINED_UPLOAD")" = "1"
test "$(grep -c 'HTTP/1.1 200 OK' <<<"$PIPELINED_UPLOAD")" = "1"
test "$(grep -c 'hello' <<<"$PIPELINED_UPLOAD")" = "1"

# CGI：/cgi-bin/ 下的脚本通过 fork/exec 执行，请求信息经环境变量传递。
chmod +x www/cgi-bin/hello.sh www/cgi-bin/echo.sh www/cgi-bin/slow.sh
test "$(curl -sS "http://127.0.0.1:$PORT/cgi-bin/hello.sh?name=tom")" = \
    "Hello from CGI: method=GET query=name=tom"

# CGI + POST：请求体经 stdin 传给脚本并原样回显。
test "$(curl -sS --data-binary 'ping' "http://127.0.0.1:$PORT/cgi-bin/echo.sh")" = "ping"

# 不存在的脚本返回 404，不可执行的文件返回 403。
test "$(curl -sS -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$PORT/cgi-bin/missing.sh")" = "404"
test "$(curl -sS -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$PORT/cgi-bin/noexec.txt")" = "403"

# 超时脚本被强杀，返回 504（约 5 秒）。
test "$(curl -sS -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$PORT/cgi-bin/slow.sh")" = "504"

# Send two pipelined requests through one TCP connection. Receiving two status
# lines proves that the server resets its state and reuses a Keep-Alive socket.
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf '%b' \
    'GET /sample.txt HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n' \
    'GET /downloads/ HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' >&3
PIPELINED_RESPONSE="$(cat <&3)"
exec 3<&- 3>&-
test "$(grep -c 'HTTP/1.1 200 OK' <<<"$PIPELINED_RESPONSE")" = "2"

# —— 配置热加载：独立实例，端口由配置文件驱动，验证 SIGHUP 后生效 ——
RELOAD_PORT=$((PORT + 1))
RELOAD_PORT2=$((PORT + 2))
cat > /tmp/minihttpd-reload.conf <<EOF
port $RELOAD_PORT
document_root ./www
access_log ./logs/reload-access.log
error_log ./logs/reload-error.log
keepalive_timeout 5
EOF

./minihttpd -c /tmp/minihttpd-reload.conf > /tmp/minihttpd-reload.out 2>&1 &
RELOAD_PID=$!

for _ in {1..30}
do
    curl -fsS "http://127.0.0.1:$RELOAD_PORT/" >/dev/null 2>&1 && break
    sleep 0.1
done
test "$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$RELOAD_PORT/")" = "200"

# 修改配置端口后发 SIGHUP：新端口生效，旧端口关闭。
sed -i "s/^port .*/port $RELOAD_PORT2/" /tmp/minihttpd-reload.conf
kill -HUP "$RELOAD_PID"
for _ in {1..30}
do
    curl -fsS "http://127.0.0.1:$RELOAD_PORT2/" >/dev/null 2>&1 && break
    sleep 0.1
done
test "$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$RELOAD_PORT2/")" = "200"
if curl -sS -o /dev/null "http://127.0.0.1:$RELOAD_PORT/" 2>/dev/null
then
    printf 'old port still reachable after reload\n'
    exit 1
fi

# 配置内容非法时 SIGHUP 被忽略，服务器保持旧配置运行。
printf 'garbage without value\n' > /tmp/minihttpd-reload.conf
kill -HUP "$RELOAD_PID"
sleep 0.5
test "$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$RELOAD_PORT2/")" = "200"

# Exercise multiple epoll-managed client sockets concurrently.
seq 1 40 | xargs -P 8 -I{} curl -fsS "$BASE/sample.txt" -o /dev/null
printf 'All HTTP tests passed.\n'
