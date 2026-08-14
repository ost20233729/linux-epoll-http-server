#ifndef MINIHTTPD_HTTP_H
#define MINIHTTPD_HTTP_H
#include "common.h"
int http_find_request_end(const char *buffer, size_t length, size_t *request_length);
int http_parse_request(const char *buffer, size_t length, HttpRequest *request);
const char *http_reason_phrase(int status);
const char *http_mime_type(const char *path);
int http_url_decode_path(const char *target, char *decoded, size_t decoded_size);
int http_parse_range(const HttpRequest *request, off_t file_size, off_t *start, off_t *end);
int http_escape_html(const char *input, char *output, size_t output_size);
int http_url_encode(const char *input, char *output, size_t output_size);
#endif
