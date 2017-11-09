#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <http_parser.h>

#include "uhttp.h"
#include "uhttp_internal.h"

static struct {
	int code;
	const char *reason;
} http_status_message[] = {
	{200, "OK"},
	{301, "Moved"},
	{302, "Found"},
	{400, "Bad Request"},
	{401, "Unauthorized"},
	{403, "Forbidden"},
	{404, "Not Found"},
	{500, "Internal Server Error"},
	{501, "Not Implemented"},
	{502, "Bad Gateway"},
	{503, "Service Unavailable"},
	{-1, NULL}
};

const char *uh_version()
{
	return UHTTP_VERSION_STRING;
}

static const char *get_http_status_message(int code)
{
	int i;
	const char *reason = "OK";

	for (i = 0; http_status_message[i].reason; i++) {
		if (code == http_status_message[i].code)
			reason = http_status_message[i].reason;
	}
	return reason;
}

static void uh_connection_destroy(struct uh_connection *con)
{
	if (con) {
		struct ev_loop *loop = con->srv->loop;
	
		if (con->sock > 0)
			close(con->sock);
		
		uh_buf_free(&con->read_buf);
		uh_buf_free(&con->write_buf);
		
		ev_io_stop(loop, &con->read_watcher);
		ev_io_stop(loop, &con->write_watcher);
		ev_timer_stop(loop, &con->timer_watcher);

		list_del(&con->list);
		free(con);
	}
}

static void connection_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
	struct uh_connection *con = container_of(w, struct uh_connection, timer_watcher);
	uh_log_info("connection(%p) timeout", con);
	uh_connection_destroy(con);
}

static int on_message_begin(http_parser *parser)
{
	struct uh_connection *con = container_of(parser, struct uh_connection, parser);
	struct ev_loop *loop = con->srv->loop;
	ev_timer *timer_watcher = &con->timer_watcher;
	
	uh_buf_init(&con->read_buf, UH_BUFFER_SIZE);
	uh_buf_init(&con->write_buf, UH_BUFFER_SIZE);

	memset(&con->req, 0, sizeof(struct uh_request));

	ev_timer_stop(loop, timer_watcher);
	ev_timer_init(timer_watcher, connection_timeout_cb, UH_CONNECTION_TIMEOUT, 0);
	ev_timer_start(loop, timer_watcher);
	
	return 0;
}

static int on_url(http_parser *parser, const char *at, size_t len)
{
	struct uh_connection *con = container_of(parser, struct uh_connection, parser);
	
	if (!con->req.url.at)
		con->req.url.at = at;

	con->req.url.len += len;	
    return 0;
}

static int on_header_field(http_parser *parser, const char *at, size_t len)
{
	struct uh_connection *con = container_of(parser, struct uh_connection, parser);
	struct uh_header *header = con->req.header;

	if (!header[con->req.header_num].field.at) {
		header[con->req.header_num].field.at = at;
	}
	header[con->req.header_num].field.len += len;
	
    return 0;
}

static int on_header_value(http_parser *parser, const char *at, size_t len)
{
	struct uh_connection *con = container_of(parser, struct uh_connection, parser);
	struct uh_header *header = con->req.header;

	con->req.header_num += 1;
	
	if (!header[con->req.header_num - 1].value.at)
		header[con->req.header_num - 1].value.at = at;
	header[con->req.header_num - 1].value.len += len;
	
    return 0;
}

static int on_body(http_parser *parser, const char *at, size_t len)
{
	struct uh_connection *con = container_of(parser, struct uh_connection, parser);
	
	if (!con->req.body.at)
		con->req.body.at = at;

	con->req.body.len += len;
    return 0;
}


/* Return 1 for equal */
static int uh_value_cmp(struct uh_value *uv, const char *str)
{
	if (uv->len != strlen(str))
		return 0;

	return (!strncasecmp(uv->at, str, uv->len));
}

static int on_message_complete(http_parser *parser)
{
	struct uh_connection *con = container_of(parser, struct uh_connection, parser);
	struct uh_route *r;
#ifdef UH_DEBUG	
	int i;
	struct uh_header *header = con->req.header;
	
	printf("Url:[%.*s]\n", (int)con->req.url.len, con->req.url.at);
	
	for (i = 0; i < con->req.header_num; i++) {
		printf("[%.*s:%.*s]\n", (int)header[i].field.len, header[i].field.at,
			(int)header[i].value.len, header[i].value.at);	
	}

	printf("Body:[%.*s]\n", (int)con->req.body.len, con->req.body.at);
#endif

	list_for_each_entry(r, &con->srv->routes, list) {
		if (uh_value_cmp(&con->req.url, r->path)) {
			r->cb(con);
			return 0;
		}
	}

	uh_send_error(con, 404, NULL);
	
	return 0;
}


static http_parser_settings parser_settings = {
	.on_message_begin	 = on_message_begin,
	.on_url              = on_url,
	.on_header_field     = on_header_field,
	.on_header_value     = on_header_value,
	.on_body             = on_body,
	.on_message_complete = on_message_complete
};

static void connection_read_cb(struct ev_loop *loop, ev_io *w, int revents)
{
	struct uh_connection *con = container_of(w, struct uh_connection, read_watcher);
	struct uh_buf *buf = &con->read_buf;
	char *base;
	int len, parsered;

	if (uh_buf_available(buf) < UH_BUFFER_SIZE)
		uh_buf_grow(buf, UH_BUFFER_SIZE);

	base = buf->base + buf->len;
	
	len = read(w->fd, base, UH_BUFFER_SIZE);
	if (unlikely(len < 0)) {
		if (errno == EINTR)
			return;
		uh_log_err("read");
		uh_send_error(con, 500, NULL);
		return;
	}

	if (len == 0) {
		uh_connection_destroy(con);
		return;
	}

	buf->len += len;

#ifdef UH_DEBUG
	printf("read:[%.*s]\n", len, base);
#endif

	parsered = http_parser_execute(&con->parser, &parser_settings, base, len);
	if (unlikely(parsered != len)){
		uh_log_err("http parser failed:%s", http_errno_description(HTTP_PARSER_ERRNO(&con->parser)));
		uh_send_error(con, 400, NULL);
	}
}

static void connection_write_cb(struct ev_loop *loop, ev_io *w, int revents)
{
	struct uh_connection *con = container_of(w, struct uh_connection, write_watcher);
	struct uh_buf *buf = &con->write_buf;
	
	if (buf->len > 0) {
		int len = write(w->fd, buf->base, buf->len);
		uh_buf_remove(buf, len);
	}

	if (buf->len == 0) {
		ev_io_stop(loop, w);

		if (!http_should_keep_alive(&con->parser))
			con->flags |= UH_CONNECTION_CLOSE;
	}

	if (con->flags & UH_CONNECTION_CLOSE)
		uh_connection_destroy(con);
}

static void uh_accept_cb(struct ev_loop *loop, ev_io *w, int revents)
{
	int sock = -1;
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	struct uh_server *srv = container_of(w, struct uh_server, read_watcher);
	struct uh_connection *con = NULL;
	ev_io *read_watcher, *write_watcher;
	ev_timer *timer_watcher;
	
	con = calloc(1, sizeof(struct uh_connection));
	if (unlikely(!con)) {
		uh_log_err("calloc");
		return;
	}

	con->srv = srv;
	list_add(&con->list, &srv->connections);
		
	sock = accept4(w->fd, (struct sockaddr *)&addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (unlikely(sock < 0)) {
		if (errno != EINTR)
			uh_log_err("accept");
		goto err;
	}

	con->sock = sock;

	read_watcher = &con->read_watcher;
	ev_io_init(read_watcher, connection_read_cb, sock, EV_READ);
	ev_io_start(loop,read_watcher);

	write_watcher = &con->write_watcher;
	ev_io_init(write_watcher, connection_write_cb, sock, EV_WRITE);

	timer_watcher = &con->timer_watcher; 
	ev_timer_init(timer_watcher, connection_timeout_cb, UH_CONNECTION_TIMEOUT, 0);
	ev_timer_start(loop, timer_watcher);
		
	http_parser_init(&con->parser, HTTP_REQUEST);
	
	uh_log_info("new connection:%p", con);
	return;
err:
	uh_connection_destroy(con);
}

struct uh_server *uh_server_new(struct ev_loop *loop, const char *ipaddr, int port)
{
	struct uh_server *srv = NULL;
	struct sockaddr_in addr;
	int sock = -1, on = 1;
	ev_io *read_watcher;
	
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	
	if (inet_pton(AF_INET, ipaddr, &addr.sin_addr) <= 0) {
		uh_log_err("invalid ipaddr");
		return NULL;
	}
	
	srv = calloc(1, sizeof(struct uh_server));
	if (!srv) {
		uh_log_err("calloc");
		return NULL;
	}

	INIT_LIST_HEAD(&srv->routes);
	INIT_LIST_HEAD(&srv->connections);
	
	sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (sock < 0) {
		uh_log_err("socket");
		goto err;
	}

	srv->sock = sock;
	srv->loop = loop;
	
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	if (bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0) {
		uh_log_err("bind");
		goto err;
	}

	if (listen(sock, SOMAXCONN) < 0) {
		uh_log_err("listen");
		goto err;
	}

	read_watcher = &srv->read_watcher;
	ev_io_init(read_watcher, uh_accept_cb, sock, EV_READ);
	ev_io_start(loop, read_watcher);
	
	return srv;

err:
	uh_server_free(srv);
	return NULL;
}

void uh_server_free(struct uh_server *srv)
{
	if (srv) {
		struct uh_connection *con, *tmp_c;
		struct uh_route *r, *tmp_r;
	
		if (srv->sock > 0)
			close(srv->sock);
		
		ev_io_stop(srv->loop, &srv->read_watcher);
		
		list_for_each_entry_safe(con, tmp_c, &srv->connections, list) {
			uh_connection_destroy(con);
		}

		list_for_each_entry_safe(r, tmp_r, &srv->routes, list) {
			list_del(&r->list);
			free(r->path);
			free(r);
		}
		
		free(srv);
	}
}

int uh_send(struct uh_connection *con, const void *buf, int len)
{
	len = uh_buf_append(&con->write_buf, buf, len);
	if (len > 0)
	    ev_io_start(con->srv->loop, &con->write_watcher);
	return len;
}

int uh_printf(struct uh_connection *con, const char *fmt, ...)
{
	int len = 0;
	va_list ap;
	char *str = NULL;

	assert(fmt);

	if (*fmt) {
		va_start(ap, fmt);
		len = vasprintf(&str, fmt, ap);
		va_end(ap);
	}
	
	if (len >= 0) {
		len = uh_send(con, str, len);
		free(str);
	}
	return len;
}

static void send_status_line(struct uh_connection *con, int code)
{
	const char *reason = get_http_status_message(code);
	uh_printf(con, "HTTP/1.1 %d %s\r\nServer: Libuhttp %s\r\n",
		code, reason, UHTTP_VERSION_STRING);
}

void uh_send_head(struct uh_connection *con, int status, int length, const char *extra_headers)
{
	send_status_line(con, status);
	
	if (length < 0)
		uh_printf(con, "%s", "Transfer-Encoding: chunked\r\n");
	else
		uh_printf(con, "Content-Length: %d\r\n", length);

	if (extra_headers) 
		uh_send(con, extra_headers, strlen(extra_headers));

	uh_send(con, "\r\n", 2);
}

void uh_send_error(struct uh_connection *con, int code, const char *reason)
{
	http_parser *parser = &con->parser;
	
	if (!reason)
		reason = get_http_status_message(code);

	if (http_should_keep_alive(parser) && code < 400) {
		uh_send_head(con, code, strlen(reason), "Content-Type: text/plain\r\nConnection: keep-alive\r\n");
	} else {
		uh_send_head(con, code, strlen(reason), "Content-Type: text/plain\r\nConnection: close\r\n");
	}
	
	if (parser->method != HTTP_HEAD)
		uh_send(con, reason, strlen(reason));

	con->flags |= UH_CONNECTION_CLOSE;
}

void uh_redirect(struct uh_connection *con, int code, const char *location)
{
	char body[128] = "";
	http_parser *parser = &con->parser;
	
	snprintf(body, sizeof(body), "<p>Moved <a href=\"%s\">here</a></p>", location);  

	send_status_line(con, code);

	uh_printf(con,
		"Location: %s\r\n"
		"Content-Type: text/html\r\n"
		"Content-Length: %zu\r\n"
		"Cache-Control: no-cache\r\n", location, strlen(body));
	
	uh_send(con, "\r\n", 2);

	if (parser->method != HTTP_HEAD)
		uh_send(con, body, strlen(body));
}

int uh_send_chunk(struct uh_connection *con, const char *buf, int len)
{
	int slen = 0;
	slen += uh_printf(con, "%X\r\n", len);
	slen += uh_send(con, buf, len);
	slen += uh_send(con, "\r\n", 2);
	return slen;
}

int uh_printf_chunk(struct uh_connection *con, const char *fmt, ...)
{
	int len = 0;
	va_list ap;
	char *str = NULL;

	assert(fmt);

	if (*fmt) {
		va_start(ap, fmt);
		len = vasprintf(&str, fmt, ap);
		va_end(ap);
	}

	if (len >= 0) {
		len = uh_send_chunk(con, str, len);
		free(str);
	}

	return len;
}

int uh_register_route(struct uh_server *srv, const char *path, uh_route_handler_t cb)
{
	struct uh_route *r;

	assert(path);

	r = calloc(1, sizeof(struct uh_route));
	if (!r) {
		uh_log_err("calloc");
		return -1;
	}

	r->path = strdup(path);
	if (!r->path) {
		uh_log_err("strdup");
		free(r);
		return -1;
	}
	
	r->cb = cb;
	list_add(&r->list, &srv->routes);
	
	return 0;	
}

inline struct uh_value *uh_get_url(struct uh_connection *con)
{
	return &con->req.url;
}

struct uh_value *uh_get_header(struct uh_connection *con, const char *name)
{
	int i;
	struct uh_header *header = con->req.header;
	
	for (i = 0; i < con->req.header_num; i++) {
		if (uh_value_cmp(&header[i].field, name))
			return &header[i].value;
	}
	return NULL;
}
