#!/usr/bin/env bash
set -euo pipefail
PORT="${PORT:-18080}"
BASE="http://127.0.0.1:${PORT}"

cleanup()
{
    kill "${SERVER_PID:-0}" 2>/dev/null || true
    wait "${SERVER_PID:-0}" 2>/dev/null || true
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

# Send two pipelined requests through one TCP connection. Receiving two status
# lines proves that the server resets its state and reuses a Keep-Alive socket.
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf '%b' \
    'GET /sample.txt HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n' \
    'GET /downloads/ HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' >&3
PIPELINED_RESPONSE="$(cat <&3)"
exec 3<&- 3>&-
test "$(grep -c 'HTTP/1.1 200 OK' <<<"$PIPELINED_RESPONSE")" = "2"

# Exercise multiple epoll-managed client sockets concurrently.
seq 1 40 | xargs -P 8 -I{} curl -fsS "$BASE/sample.txt" -o /dev/null
printf 'All HTTP tests passed.\n'
