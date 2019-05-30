#ifndef _EVENT_H
#define _EVENT_H

#include <stdint.h>
#include <ev.h>
//sid == socket number, it will be remap in silly_socket, not a real socket fd

typedef void (*silly_finalizer_t)(void *ptr);

//ev_io的回调函数，用于异步接受回复
void ev_io_cb(struct ev_loop *loop, ev_io* io_w,int e);
void ev_io_reset(struct ev_loop* loop, struct ev_io* w, int fd, int ev);

int silly_socket_init();
void silly_socket_exit();
void silly_socket_terminate();

int silly_socket_listen(const char *ip, const char *port, int backlog);
int silly_socket_connect(const char *ip, const char *port,
		const char *bindip, const char *bindport);
int silly_socket_udpbind(const char *ip, const char *port);
int silly_socket_udpconnect(const char *ip, const char *port,
		const char *bindip, const char *bindport);
int silly_socket_salen(const void *data);
const char *silly_socket_ntop(const void *data, int *size);

int silly_socket_send(int sid, uint8_t *buff, size_t sz,
	silly_finalizer_t finalizer);
int silly_socket_udpsend(int sid, uint8_t *buff, size_t sz,
	const uint8_t *addr, size_t addrlen, silly_finalizer_t finalizer);
int silly_socket_close(int sid);

int silly_socket_poll();

const char *silly_socket_pollapi();

#endif


