#include "silly_conf.h"
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "silly.h"
#include "atomic.h"
#include "compiler.h"
#include "silly_log.h"
#include "silly_worker.h"
#include "silly_malloc.h"
#include "silly_socket.h"

//STYPE == socket type

#if EAGAIN == EWOULDBLOCK
#define ETRYAGAIN EAGAIN
#else
#define ETRYAGAIN EAGAIN: case EWOULDBLOCK
#endif

#define EVENT_SIZE (128)
#define CMDBUF_SIZE (8 * sizeof(struct cmdpacket))
#define MAX_UDP_PACKET (512)
#define MAX_SOCKET_COUNT (1 << SOCKET_MAX_EXP)
#define MIN_READBUF_LEN (64)
#define NAMELEN	(INET6_ADDRSTRLEN + 8)	//[ipv6]:port

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#define HASH(sid) (sid & (MAX_SOCKET_COUNT - 1))

#define PROTOCOL_TCP 1
#define PROTOCOL_UDP 2
#define PROTOCOL_PIPE 3

enum stype {
	STYPE_RESERVE,
	STYPE_ALLOCED,
	STYPE_LISTEN,		//listen fd
	STYPE_UDPBIND,		//listen fd(udp)
	STYPE_SOCKET,		//socket normal status
	STYPE_HALFCLOSE,	//socket is closed
	STYPE_CONNECTING,	//socket is connecting, if success it will be STYPE_SOCKET
	STYPE_CTRL,		//pipe cmd type
};

//replace 'sockaddr_storage' with this struct,
//because we only care about 'ipv6' and 'ipv4'
#define SA_LEN(sa)\
	((sa).sa_family == AF_INET ?\
	sizeof(struct sockaddr_in) :\
	sizeof(struct sockaddr_in6))

union sockaddr_full {
	struct sockaddr	sa;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct wlist {
	struct wlist *next;
	size_t size;
	uint8_t *buf;
	silly_finalizer_t finalizer;
	union sockaddr_full *udpaddress;
};

struct socket {
	int sid;	//socket descriptor
	int fd;
	struct ev_io watcher;
	int presize;
	int protocol;
	enum stype type;
	size_t wloffset;
	struct wlist *wlhead;
	struct wlist **wltail;
};

struct silly_socket {
	struct ev_loop* loop;
	//reverse for accept
	//when reach the limit of file descriptor's number
	int reservefd;
	//socket pool
	struct socket *socketpool;
	//ctrl pipe, call write can be automatic
	//when data less then 64k(from APUE)
	struct ev_io w;
	int ctrlsendfd;
	int ctrlrecvfd;
	int ctrlcount;
	int cmdcap;
	uint8_t *cmdbuf;
	//reserve id(for socket fd remap)
	int reserveid;
	char namebuf[NAMELEN];
};

static struct silly_socket *SSOCKET;

void
ev_io_reset(struct ev_loop* loop, struct ev_io* w, int fd, int ev) {
	ev_io_stop(loop, w);
	ev_io_init(w, ev_io_cb, fd, ev);
	ev_io_start(loop, w);
}

static void
socketpool_init(struct silly_socket *ss)
{
	int i;
	struct socket *pool = silly_malloc(sizeof(*pool) * MAX_SOCKET_COUNT);
	ss->socketpool = pool;
	ss->reserveid = -1;
	for (i = 0; i < MAX_SOCKET_COUNT; i++) {
		pool->sid = -1;
		pool->fd = -1;
		pool->type = STYPE_RESERVE;
		pool->presize = MIN_READBUF_LEN;
		pool->wloffset = 0;
		pool->wlhead = NULL;
		pool->wltail = &pool->wlhead;
		pool++;
	}
	return ;
}

static inline void
wlist_append(struct socket *s, uint8_t *buf, size_t size,
		silly_finalizer_t finalizer)
{
	struct wlist *w;
	w = (struct wlist *)silly_malloc(sizeof(*w));
	w->size = size;
	w->buf = buf;
	w->finalizer = finalizer;
	w->next = NULL;
	w->udpaddress = NULL;
	*s->wltail = w;
	s->wltail = &w->next;
	return ;
}

static inline void
wlist_appendudp(struct socket *s, uint8_t *buf, size_t size,
	silly_finalizer_t finalizer, const union sockaddr_full *addr)
{
	int addrsz;
	struct wlist *w;
	addrsz = addr ? SA_LEN(addr->sa) : 0;
	w = (struct wlist *)silly_malloc(sizeof(*w) + addrsz);
	w->size = size;
	w->buf = buf;
	w->finalizer = finalizer;
	w->next = NULL;
	if (addrsz != 0) {
		w->udpaddress = (union sockaddr_full *)(w + 1);
		memcpy(w->udpaddress, addr, addrsz);
	} else {
		w->udpaddress = NULL;
	}
	*s->wltail = w;
	s->wltail = &w->next;
	return ;
}


static void
wlist_free(struct socket *s)
{
	struct wlist *w;
	struct wlist *t;
	w = s->wlhead;
	while (w) {
		t = w;
		w = w->next;
		assert(t->buf);
		t->finalizer(t->buf);
		silly_free(t);
	}
	s->wlhead = NULL;
	s->wltail = &s->wlhead;
	return ;
}
static inline int
wlist_empty(struct socket *s)
{
	return s->wlhead == NULL ? 1 : 0;
}

static struct socket*
allocsocket(struct silly_socket *ss, int fd, enum stype type, int protocol)
{
	int i;
	int id;
	assert(
		protocol == PROTOCOL_TCP ||
		protocol == PROTOCOL_UDP ||
		protocol == PROTOCOL_PIPE);
	for (i = 0; i < MAX_SOCKET_COUNT; i++) {
		id = atomic_add_return(&ss->reserveid, 1);
		if (unlikely(id < 0)) {
			id = id & 0x7fffffff;
			atomic_and_return(&ss->reserveid, 0x7fffffff);
		}
		struct socket *s = &ss->socketpool[HASH(id)];
		if (s->type == STYPE_RESERVE) {
			if (atomic_swap(&s->type, STYPE_RESERVE, type)) {
				assert(s->wlhead == NULL);
				assert(s->wltail == &s->wlhead);
				s->protocol = protocol;
				s->presize = MIN_READBUF_LEN;
				s->sid = id;
				s->fd = fd;
				s->wloffset = 0;
				return s;
			}
		}
	}
	silly_log("[socket] allocsocket fail, find no empty entry\n");
	return NULL;
}


static inline void
freesocket(struct silly_socket *ss, struct socket *s)
{
	if (unlikely(s->type == STYPE_RESERVE)) {
		const char *fmt = "[socket] freesocket sid:%d error type:%d\n";
		silly_log(fmt, s->sid, s->type);
		return ;
	}
	if (s->fd >= 0) {
		ev_io_stop(ss->loop, &s->watcher);
		s->watcher.fd = -1;
		s->watcher.data = NULL;
		close(s->fd);
		s->fd = -1;
	}
	wlist_free(s);
	assert(s->wlhead == NULL);
	atomic_barrier();
	s->type = STYPE_RESERVE;
}

static void
nonblock(int fd)
{
	int err;
	int flag;
	flag = fcntl(fd, F_GETFL, 0);
	if (unlikely(flag < 0)) {
		silly_log("[socket] nonblock F_GETFL:%s\n", strerror(errno));
		return ;
	}
	flag |= O_NONBLOCK;
	err = fcntl(fd, F_SETFL, flag);
	if (unlikely(err < 0)) {
		silly_log("[socket] nonblock F_SETFL:%s\n", strerror(errno));
		return ;
	}
	return ;
}

static void
nodelay(int fd)
{
	int err;
	int on = 1;
	err = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	if (err >= 0)
		return ;
	silly_log("[socket] nodelay error:%s\n", strerror(errno));
	return ;
}

static void
keepalive(int fd)
{
	int err;
	int on = 1;
	err = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	if (err >= 0)
		return ;
	silly_log("[socket] keepalive error:%s\n", strerror(errno));
}

static int
ntop(struct silly_socket *ss, union sockaddr_full *addr)
{
	uint16_t port;
	int namelen, family;
	char *buf = ss->namebuf;
	family = addr->sa.sa_family;
	if (family == AF_INET) {
		port = addr->v4.sin_port;
		inet_ntop(family, &addr->v4.sin_addr, buf, INET_ADDRSTRLEN);
		namelen = strlen(buf);
	} else {
		buf[0] = '[';
		assert(family == AF_INET6);
		port = addr->v6.sin6_port;
		inet_ntop(family, &addr->v6.sin6_addr, buf+1, INET6_ADDRSTRLEN);
		namelen = strlen(buf);
		buf[namelen++] = ']';
	}
	port = ntohs(port);
	namelen += snprintf(&buf[namelen], NAMELEN - namelen, ":%d", port);
	return namelen;
}

static void
report_accept(struct silly_socket *ss, struct socket *listen)
{
	int fd;
	struct socket *s;
	union sockaddr_full addr;
	struct silly_message_socket *sa;
	socklen_t len = sizeof(addr);
#ifndef USE_ACCEPT4
	fd = accept(listen->fd, &addr.sa, &len);
#else
	fd = accept4(listen->fd, &addr.sa, &len, SOCK_NONBLOCK);
#endif
	if (unlikely(fd < 0)) {
		if (errno != EMFILE && errno != ENFILE)
			return ;
		close(ss->reservefd);
		fd = accept(listen->fd, NULL, NULL);
		close(fd);
		silly_log("[socket] accept reach limit of file descriptor\n");
		ss->reservefd = open("/dev/null", O_RDONLY);
		return ;
	}
#ifndef USE_ACCEPT4
	nonblock(fd);
#endif
	keepalive(fd);
	nodelay(fd);
	s = allocsocket(ss, fd, STYPE_SOCKET, PROTOCOL_TCP);
	if (unlikely(s == NULL)) {
		close(fd);
		return ;
	}

	s->watcher.data = s;
	ev_io_init(&s->watcher, ev_io_cb, fd, EV_READ);
	ev_io_start(ss->loop, &s->watcher);
	
	int namelen = ntop(ss, &addr);
	sa = silly_malloc(sizeof(*sa) + namelen + 1);
	sa->type = SILLY_SACCEPT;
	sa->sid = s->sid;
	sa->ud = listen->sid;
	sa->data = (uint8_t *)(sa + 1);
	*sa->data = namelen;
	memcpy(sa->data + 1, ss->namebuf, namelen);
	silly_worker_push(tocommon(sa));
	return ;
}

static void
report_close(struct silly_socket *ss, struct socket *s, int err)
{
	(void)ss;
	int type;
	struct silly_message_socket *sc;
	if (s->type == STYPE_HALFCLOSE)//don't notify the active close
		return ;
	type = s->type;
	assert(type == STYPE_LISTEN ||
		type == STYPE_SOCKET ||
		type == STYPE_CONNECTING ||
		type == STYPE_ALLOCED);
	sc = silly_malloc(sizeof(*sc));
	sc->type = SILLY_SCLOSE;
	sc->sid = s->sid;
	sc->ud = err;
	silly_worker_push(tocommon(sc));
	return ;
}

static void
report_data(struct silly_socket *ss, struct socket *s, int type, uint8_t *data, size_t sz)
{
	(void)ss;
	assert(s->type == STYPE_SOCKET || s->type == STYPE_UDPBIND);
	struct silly_message_socket *sd = silly_malloc(sizeof(*sd));
	assert(type == SILLY_SDATA || type == SILLY_SUDP);
	sd->type = type;
	sd->sid = s->sid;
	sd->ud = sz;
	sd->data = data;
	silly_worker_push(tocommon(sd));
	return ;
};

static inline int
checkconnected(struct silly_socket *ss, struct socket *s)
{
	int ret, err;
	socklen_t errlen = sizeof(err);
	assert(s->fd >= 0);
	ret = getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
	if (unlikely(ret < 0)) {
		silly_log("[socket] checkconnected:%s\n", strerror(errno));
		goto err;
	}
	if (unlikely(err != 0)) {
		silly_log("[socket] checkconnected:%s\n", strerror(err));
		goto err;
	}
	if (wlist_empty(s)) {
		ev_io_reset(ss->loop, &s->watcher, s->fd, EV_READ);
	}
		
	return 0;
err:
	//occurs error
	report_close(ss, s, errno);
	freesocket(ss, s);
	return -1;
}

static void
report_connected(struct silly_socket *ss, struct socket *s)
{

	if (checkconnected(ss, s) < 0)
		return ;
	struct silly_message_socket *sc;
	sc = silly_malloc(sizeof(*sc));
	sc->type = SILLY_SCONNECTED;
	sc->sid = s->sid;
	silly_worker_push(tocommon(sc));
	return ;
}

static ssize_t
readn(int fd, uint8_t *buf, size_t sz)
{
	for (;;) {
		ssize_t len;
		len = read(fd, buf, sz);
		if (len < 0) {
			switch(errno) {
			case EINTR:
				continue;
			case ETRYAGAIN:
				return 0;
			default:
				return -1;
			}
		} else if (len == 0) {
			return -1;
		}
		return len;
	}
	assert(!"expected return of readn");
	return 0;
}

static ssize_t
sendn(int fd, const uint8_t *buf, size_t sz)
{
	for (;;) {
		ssize_t len;
		len = write(fd, buf, sz);
		assert(len != 0);
		if (len == -1) {
			switch (errno) {
			case EINTR:
				continue;
			case ETRYAGAIN:
				return 0;
			default:
				return -1;
			}
		}
		return len;
	}
	assert(!"never come here");
	return 0;
}

static ssize_t
readudp(int fd, uint8_t *buf, size_t sz,
	union sockaddr_full *addr, socklen_t *addrlen)
{
	ssize_t n;
	for (;;) {
		n = recvfrom(fd, buf, sz, 0, (struct sockaddr *)addr, addrlen);
		if (n >= 0)
			return n;
		switch (errno) {
		case EINTR:
			continue;
		case ETRYAGAIN:
			return -1;
		default:
			return -1;
		}
	}
	return 0;
}

static ssize_t
sendudp(int fd, uint8_t *data, size_t sz, const union sockaddr_full *addr)
{
	ssize_t n;
	socklen_t sa_len;
	const struct sockaddr *sa;
	if (addr != NULL) {
		sa = &addr->sa;
		sa_len = SA_LEN(*sa);
	} else {
		sa = NULL;
		sa_len = 0;
	}
	for (;;) {
		n = sendto(fd, data, sz, 0, sa, sa_len);
		if (n >= 0)
			return n;
		switch (errno) {
		case EINTR:
			continue;
		case ETRYAGAIN:
			return -2;
		default:
			return -1;
		}
	}
	return 0;
}



static int
forward_msg_tcp(struct silly_socket *ss, struct socket *s)
{
	ssize_t sz;
	ssize_t presize = s->presize;
	uint8_t *buf = (uint8_t *)silly_malloc(presize);
	sz = readn(s->fd, buf, presize);
	//half close socket need no data
	if (sz > 0 && s->type != STYPE_HALFCLOSE) {
		report_data(ss, s, SILLY_SDATA, buf, sz);
		//to predict the pakcet size
		if (sz == presize) {
			s->presize *= 2;
		} else if (presize > MIN_READBUF_LEN) {
			//s->presize at leatest is 2 * MIN_READBUF_LEN
			int half = presize / 2;
			if (sz < half)
				s->presize = half;
		}
	} else {
		silly_free(buf);
		if (sz < 0) {
			report_close(ss, s, errno);
			freesocket(ss, s);
			return -1;
		}
		return 0;
	}
	return sz;
}

static int
forward_msg_udp(struct silly_socket *ss, struct socket *s)
{
	uint8_t *data;
	ssize_t n, sa_len;
	union sockaddr_full addr;
	uint8_t udpbuf[MAX_UDP_PACKET];
	socklen_t len = sizeof(addr);
	n = readudp(s->fd, udpbuf, MAX_UDP_PACKET, &addr, &len);
	if (n < 0)
		return 0;
	sa_len = SA_LEN(addr.sa);
	data = (uint8_t *)silly_malloc(n + sa_len);
	memcpy(data, udpbuf, n);
	memcpy(data + n, &addr, sa_len);
	report_data(ss, s, SILLY_SUDP, data, n);
	return n;
}

int
silly_socket_salen(const void *data)
{
	union sockaddr_full *addr;
	addr = (union sockaddr_full *)data;
	return SA_LEN(addr->sa);
}

const char *
silly_socket_ntop(const void *data, int *size)
{
	union sockaddr_full *addr;
	addr = (union sockaddr_full *)data;
	*size = ntop(SSOCKET, addr);
	return SSOCKET->namebuf;
}

static void
send_msg_tcp(struct silly_socket *ss, struct socket *s)
{
	struct wlist *w;
	w = s->wlhead;
	assert(w);
	while (w) {
		ssize_t sz;
		assert(w->size > s->wloffset);
		sz = sendn(s->fd, w->buf + s->wloffset, w->size - s->wloffset);
		if (unlikely(sz < 0)) {
			report_close(ss, s, errno);
			freesocket(ss, s);
			return ;
		}
		s->wloffset += sz;
		if (s->wloffset < w->size) //send some
			return ;
		assert((size_t)s->wloffset == w->size);
		s->wloffset = 0;
		s->wlhead = w->next;
		w->finalizer(w->buf);
		silly_free(w);
		w = s->wlhead;
		if (w == NULL) {//send ok
			s->wltail = &s->wlhead;
			ev_io_reset(ss->loop, &s->watcher, s->fd, EV_READ);
			if (s->type == STYPE_HALFCLOSE)
				freesocket(ss, s);
		}
	}
	return ;
}

static void
send_msg_udp(struct silly_socket *ss, struct socket *s)
{
	struct wlist *w;
	w = s->wlhead;
	assert(w);
	while (w) {
		ssize_t sz;
		sz = sendudp(s->fd, w->buf, w->size, w->udpaddress);
		if (sz == -2)	//EAGAIN, so block it
			break;
		assert(sz == -1 || (size_t)sz == w->size);
		//send fail && send ok will clear
		s->wlhead = w->next;
		w->finalizer(w->buf);
		silly_free(w);
		w = s->wlhead;
		if (w == NULL) {//send all
			s->wltail = &s->wlhead;
			ev_io_reset(ss->loop, &s->watcher, s->fd, EV_READ);
			if (s->type == STYPE_HALFCLOSE)
				freesocket(ss, s);
		}
	}
	return ;
}

//for read one complete packet once system call, fix the packet length
#define cmdcommon	int type
struct cmdlisten {	//'L/B' -> listen or bind
	cmdcommon;
	int sid;
};

struct cmdconnect {	//'C' -> tcp connect
	cmdcommon;
	int sid;
	union sockaddr_full addr;
};

struct cmdopen {	//'O' -> udp connect
	cmdcommon;
	int sid;
};

struct cmdkick {	//'K' --> close
	cmdcommon;
	int sid;
};

struct cmdsend {	//'S' --> tcp send
	cmdcommon;
	int sid;
	int size;
	uint8_t *data;
	silly_finalizer_t finalizer;
};

struct cmdudpsend {
	struct cmdsend send;
	union sockaddr_full addr;
};

struct cmdterm {
	cmdcommon;
};

struct cmdpacket {
	union {
		cmdcommon;
		struct cmdlisten listen;
		struct cmdconnect connect;
		struct cmdopen open;
		struct cmdkick kick;
		struct cmdsend send;
		struct cmdudpsend udpsend;
		struct cmdterm term;
	} u;
};

static void
pipe_blockread(int fd, void *pk, int n)
{
	for (;;) {
		ssize_t err = read(fd, pk, n);
		if (err == -1) {
			if (likely(errno  == EINTR))
				continue;
			silly_log("[socket] pip_blockread error:%s\n",
				strerror(errno));
			return ;
		}
		assert(err == n);
		atomic_sub_return(&SSOCKET->ctrlcount, n);
		return ;
	}
}

static int
pipe_blockwrite(int fd, void *pk, int sz)
{
	for (;;) {
		ssize_t err = write(fd, pk, sz);
		if (err == -1) {
			if (likely(errno == EINTR))
				continue;
			silly_log("[socket] pipe_blockwrite error:%s",
				strerror(errno));
			return -1;
		}
		atomic_add(&SSOCKET->ctrlcount, sz);
		assert(err == sz);
		return 0;
	}
}

struct addrinfo *
getsockaddr(int protocol, const char *ip, const char *port)
{
	int err;
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP)
		hints.ai_socktype = SOCK_STREAM;
	else
		hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = protocol;
	if ((err = getaddrinfo(ip, port, &hints, &res))) {
		silly_log("[socket] bindfd ip:%s port:%s err:%s\n",
			ip, port, gai_strerror(err));
		return NULL;
	}
	return res;
}

static int
bindfd(int fd, int protocol, const char *ip, const char *port)
{
	int err;
	struct addrinfo *info;
	if (ip[0] == '\0' && port[0] == '0')
		return 0;
	info = getsockaddr(protocol, ip, port);
	if (info == NULL)
		return -1;
	err = bind(fd, info->ai_addr, info->ai_addrlen);
	freeaddrinfo(info);
	return err;
}

static int
dolisten(const char *ip, const char *port, int backlog)
{
	int err;
	int fd = -1;
	int reuse = 1;
	struct addrinfo *info = NULL;
	info = getsockaddr(IPPROTO_TCP, ip, port);
	if (unlikely(info == NULL))
		return -1;
	fd = socket(info->ai_family, SOCK_STREAM, 0);
	if (unlikely(fd < 0))
		goto end;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	err = bind(fd, info->ai_addr, info->ai_addrlen);
	if (unlikely(err < 0))
		goto end;
	nonblock(fd);
	err = listen(fd, backlog);
	if (unlikely(err < 0))
		goto end;
	freeaddrinfo(info);
	return fd;
end:
	freeaddrinfo(info);
	if (fd >= 0)
		close(fd);
	silly_log("[socket] dolisten error:%s\n", strerror(errno));
	return -1;

}

int
silly_socket_listen(const char *ip, const char *port, int backlog)
{
	int fd;
	struct socket *s;
	struct cmdlisten cmd;
	fd = dolisten(ip, port, backlog);
	if (unlikely(fd < 0))
		return fd;
	s = allocsocket(SSOCKET, fd, STYPE_ALLOCED, PROTOCOL_TCP);
	if (unlikely(s == NULL)) {
		silly_log("[socket] listen %s:%s:%d allocsocket fail\n",
			ip, port, backlog);
		close(fd);
		return -1;
	} else {
		silly_log("[socket] listen %s:%s\n",ip, port);
	}
	
	cmd.type = 'L';
	cmd.sid = s->sid;
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	return s->sid;
}

int
silly_socket_udpbind(const char *ip, const char *port)
{
	int err;
	int fd = -1;
	struct cmdlisten cmd;
	struct addrinfo *info;
	struct socket *s = NULL;
	info = getsockaddr(IPPROTO_TCP, ip, port);
	if (info == NULL)
		return -1;
	fd = socket(info->ai_family, SOCK_DGRAM, 0);
	if (unlikely(fd < 0))
		goto end;
	err = bind(fd, info->ai_addr, info->ai_addrlen);
	if (unlikely(err < 0))
		goto end;
	nonblock(fd);
	s = allocsocket(SSOCKET, fd, STYPE_ALLOCED, PROTOCOL_UDP);
	if (unlikely(s == NULL)) {
		silly_log("[socket] udpbind %s:%d allocsocket fail\n",
			ip, port);
		goto end;
	}
	freeaddrinfo(info);
	cmd.type = 'B';
	cmd.sid = s->sid;
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	return s->sid;
end:
	if (fd >= 0)
		close(fd);
	freeaddrinfo(info);
	silly_log("[socket] udplisten error:%s\n", strerror(errno));
	return -1;
}

static int
trylisten(struct silly_socket *ss, struct cmdlisten *cmd)
{
	int err = 0;
	struct socket *s;
	int sid = cmd->sid;
	s = &ss->socketpool[HASH(sid)];
	assert(s->sid == sid);
	assert(s->type == STYPE_ALLOCED);
	s->watcher.data = s;
	ev_io_init(&s->watcher, ev_io_cb, s->fd, EV_READ);
	ev_io_start(ss->loop, &s->watcher);
	s->type = STYPE_LISTEN;
	return err;
}

static int
tryudpbind(struct silly_socket *ss, struct cmdlisten *cmd)
{
	int err = 0;
	struct socket *s;
	int sid = cmd->sid;
	s = &ss->socketpool[HASH(sid)];
	assert(s->sid == sid);
	assert(s->type = STYPE_ALLOCED);
	s->watcher.data = s;
	ev_io_init(&s->watcher, ev_io_cb, s->fd, EV_READ);
	ev_io_start(ss->loop, &s->watcher);
	assert(s->protocol == PROTOCOL_UDP);
	s->type = STYPE_UDPBIND;
	assert(err == 0);
	return err;
}

int
silly_socket_connect(const char *ip, const char *port,
		const char *bindip, const char *bindport)
{
	int err, fd = -1;
	struct cmdconnect cmd;
	struct addrinfo *info;
	struct socket *s = NULL;
	assert(ip);
	assert(bindip);
	info = getsockaddr(IPPROTO_TCP, ip, port);
	if (unlikely(info == NULL))
		return -1;
	fd = socket(info->ai_family, SOCK_STREAM, 0);
	if (unlikely(fd < 0))
		goto end;
	err = bindfd(fd, IPPROTO_TCP, bindip, bindport);
	if (unlikely(err < 0))
		goto end;
	s = allocsocket(SSOCKET, fd, STYPE_ALLOCED, PROTOCOL_TCP);
	if (unlikely(s == NULL))
		goto end;
	cmd.type = 'C';
	cmd.sid = s->sid;
	memcpy(&cmd.addr, info->ai_addr, info->ai_addrlen);
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	freeaddrinfo(info);
	return s->sid;
end:
	if (fd >= 0)
		close(fd);
	freeaddrinfo(info);
	return -1;
}

static void
tryconnect(struct silly_socket *ss, struct cmdconnect *cmd)
{
	int err;
	int fd;
	struct socket *s;
	int sid = cmd->sid;
	union sockaddr_full *addr;
	s =  &ss->socketpool[HASH(sid)];
	assert(s->fd >= 0);
	assert(s->sid == sid);
	assert(s->type == STYPE_ALLOCED);
	fd = s->fd;
	nonblock(fd);
	keepalive(fd);
	nodelay(fd);
	addr = &cmd->addr;
	err = connect(fd, &addr->sa, SA_LEN(addr->sa));
	if (unlikely(err == -1 && errno != EINPROGRESS)) {	//error
		const char *fmt = "[socket] connect %s,errno:%d\n";
		report_close(ss, s, errno);
		freesocket(ss, s);
		ntop(ss,  addr);
		silly_log(fmt, ss->namebuf, errno);
		return ;
	} else if (err == 0) {	//connect
		s->type = STYPE_SOCKET;
		s->watcher.data = s;
		ev_io_init(&s->watcher, ev_io_cb, s->fd, EV_READ);
		ev_io_start(ss->loop, &s->watcher);
		report_connected(ss, s);
	} else {	//block
		s->type = STYPE_CONNECTING;
		s->watcher.data = s;
		ev_io_init(&s->watcher, ev_io_cb, s->fd, EV_READ | EV_WRITE);
		ev_io_start(ss->loop, &s->watcher);
	}
	return ;
}

int
silly_socket_udpconnect(const char *ip, const char *port,
	const char *bindip, const char *bindport)
{
	int err;
	int fd = -1;
	struct cmdopen cmd;
	struct addrinfo *info;
	struct socket *s = NULL;
	const char *fmt = "[socket] udpconnect %s:%d, errno:%d\n";
	assert(ip);
	assert(bindip);
	info = getsockaddr(IPPROTO_UDP, ip, port);
	if (unlikely(info == NULL))
		return -1;
	fd = socket(info->ai_family, SOCK_DGRAM, 0);
	if (unlikely(fd < 0))
		goto end;
	err = bindfd(fd, IPPROTO_UDP, bindip, bindport);
	if (unlikely(err < 0))
		goto end;
	//udp connect will return immediately
	err = connect(fd, info->ai_addr, info->ai_addrlen);
	if (unlikely(err < 0))
		goto end;
	s = allocsocket(SSOCKET, fd, STYPE_SOCKET, PROTOCOL_UDP);
	if (unlikely(s == NULL))
		goto end;
	assert(s->type == STYPE_SOCKET);
	cmd.type = 'O';
	cmd.sid = s->sid;
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	freeaddrinfo(info);
	return s->sid;
end:
	if (fd >= 0)
		close(fd);
	freeaddrinfo(info);
	silly_log(fmt, ip, port, errno);
	return -1;
}

static void
tryudpconnect(struct silly_socket *ss, struct cmdopen *cmd)
{
	int sid = cmd->sid;
	struct socket *s =  &ss->socketpool[HASH(sid)];
	assert(s->sid == sid);
	assert(s->fd >= 0);
	assert(s->type == STYPE_SOCKET);
	assert(s->protocol == PROTOCOL_UDP);
	s->watcher.data = s;
	ev_io_init(&s->watcher, ev_io_cb, s->fd, EV_READ);
	ev_io_start(ss->loop, &s->watcher);
	return ;
}


static inline struct socket *
checksocket(struct silly_socket *ss, int sid)
{
	struct socket *s = &ss->socketpool[HASH(sid)];
	if (unlikely(s->sid != sid)) {
		silly_log("[socket] checksocket invalid sid\n");
		return NULL;
	}
	switch (s->type) {
	case STYPE_LISTEN:
	case STYPE_SOCKET:
	case STYPE_UDPBIND:
		return s;
	default:
		silly_log("[socket] checksocket sid:%d unsupport type:%d\n",
			s->sid, s->type);
		return NULL;
	}
	return NULL;
}

int
silly_socket_close(int sid)
{
	struct cmdkick cmd;
	struct socket *s = checksocket(SSOCKET, sid);
	if (s == NULL)
		return -1;
	cmd.type = 'K';
	cmd.sid = sid;
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	return 0;
}

static int
tryclose(struct silly_socket *ss, struct cmdkick *cmd)
{
	struct socket *s;
	s = checksocket(ss, cmd->sid);
	if (s == NULL)
		return -1;
	if (wlist_empty(s)) { //already send all the data, directly close it
		freesocket(ss, s);
		return 0;
	} else {
		s->type = STYPE_HALFCLOSE;
		return -1;
	}
}

int
silly_socket_send(int sid, uint8_t *buf, size_t sz, silly_finalizer_t finalizer)
{
	struct cmdsend cmd;
	struct socket *s = checksocket(SSOCKET, sid);
	finalizer = finalizer ? finalizer : silly_free;
	if (unlikely(s == NULL)) {
		finalizer(buf);
		return -1;
	}
	if (unlikely(sz == 0)) {
		finalizer(buf);
		return -1;
	}
	cmd.type = 'S';
	cmd.sid = sid;
	cmd.data = buf;
	cmd.size = sz;
	cmd.finalizer = finalizer;
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	return 0;
}

int
silly_socket_udpsend(int sid, uint8_t *buf, size_t sz,
	const uint8_t *addr, size_t addrlen,
	silly_finalizer_t finalizer)
{
	struct cmdudpsend cmd;
	struct socket *s = checksocket(SSOCKET, sid);
	finalizer = finalizer ? finalizer : silly_free;
	if (s == NULL) {
		finalizer(buf);
		return -1;
	}
	assert(s->protocol = PROTOCOL_UDP);
	assert(s->type == STYPE_UDPBIND || s->type == STYPE_SOCKET);
	if (unlikely(s->type == STYPE_UDPBIND && addr == NULL)) {
		finalizer(buf);
		silly_log("[socket] udpsend udpbind must specify dest addr\n");
		return -1;
	}
	cmd.send.type = 'U';
	cmd.send.sid = sid;
	cmd.send.data= buf;
	cmd.send.size = sz;
	cmd.send.finalizer = finalizer;
	if (s->type == STYPE_UDPBIND) {//udp bind socket need sendto address
		assert(addrlen <= sizeof(cmd.addr));
		memcpy(&cmd.addr, addr, addrlen);
	}
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	return 0;
}


static int
trysend(struct silly_socket *ss, struct cmdsend *cmd)
{
	struct socket *s = checksocket(ss, cmd->sid);
	uint8_t *data = cmd->data;
	size_t sz = cmd->size;
	silly_finalizer_t finalizer = cmd->finalizer;
	if (s == NULL) {
		finalizer(data);
		return 0;
	}
	if (wlist_empty(s)) {//try send
		ssize_t n = sendn(s->fd, data, sz);
		if (n < 0) {
			finalizer(data);
			report_close(ss, s, errno);
			freesocket(ss, s);
			return -1;
		} else if ((size_t)n < sz) {
			s->wloffset = n;
			wlist_append(s, data, sz, finalizer);
			ev_io_reset(ss->loop, &s->watcher, s->fd, EV_READ | EV_WRITE);
		} else {
			assert((size_t)n == sz);
			finalizer(data);
		}
	} else {
		wlist_append(s, data, sz, finalizer);
	}
	return 0;
}

static int
tryudpsend(struct silly_socket *ss, struct cmdudpsend *cmd)
{
	uint8_t *data;
	size_t size;
	union sockaddr_full *addr;
	silly_finalizer_t finalizer;
	struct socket *s = checksocket(ss, cmd->send.sid);
	finalizer = cmd->send.finalizer;
	data = cmd->send.data;
	if (s == NULL) {
		finalizer(data);
		return 0;
	}
	size = cmd->send.size;
	assert(s->protocol == PROTOCOL_UDP);
	if (s->type == STYPE_UDPBIND) {
		//only udp server need address
		addr = &cmd->addr;
	} else {
		addr = NULL;
	}
	if (wlist_empty(s)) {//try send
		ssize_t n = sendudp(s->fd, data, size, addr);
		if (n == -1 || n >= 0) {	//occurs error or send ok
			finalizer(data);
			return 0;
		}
		assert(n == -2);	//EAGAIN
		wlist_appendudp(s, data, size, finalizer, addr);
	} else {
		wlist_appendudp(s, data, size, finalizer, addr);
	}
	return 0;
}

void
silly_socket_terminate()
{
	struct cmdterm cmd;
	cmd.type = 'T';
	pipe_blockwrite(SSOCKET->ctrlsendfd, &cmd, sizeof(cmd));
	return ;
}

//values of cmdpacket::type
//'L'	--> listen(tcp)
//'B'	--> bind(udp)
//'C'	--> connect(tcp)
//'O'	--> connect(udp)
//'K'	--> close(kick)
//'S'	--> send data(tcp)
//'U'	--> send data(udp)
//'T'	--> terminate(exit poll)

static void
resize_cmdbuf(struct silly_socket *ss, size_t sz)
{
	ss->cmdcap = sz;
	ss->cmdbuf = (uint8_t *)silly_realloc(ss->cmdbuf, sizeof(uint8_t) * sz);
	return ;
}

static int
cmd_process(struct silly_socket *ss)
{
	int count;
	int close = 0;
	uint8_t *ptr, *end;
	count = ss->ctrlcount;
	if (count <= 0)
		return close;
	if (count > ss->cmdcap)
		resize_cmdbuf(ss, count);
	pipe_blockread(ss->ctrlrecvfd, ss->cmdbuf, count);
	ptr = ss->cmdbuf;
	end = ptr + count;
	while (ptr < end) {
		struct cmdpacket *cmd = (struct cmdpacket *)ptr;
		switch (cmd->u.type) {
			case 'L':
				trylisten(ss, &cmd->u.listen);
				ptr += sizeof(cmd->u.listen);
				break;
			case 'B':
				tryudpbind(ss, &cmd->u.listen);
				ptr += sizeof(cmd->u.listen);
				break;
			case 'C':
				tryconnect(ss, &cmd->u.connect);
				ptr += sizeof(cmd->u.connect);
				break;
			case 'O':
				tryudpconnect(ss, &cmd->u.open);
				ptr += sizeof(cmd->u.open);
				break;
			case 'K':
				if (tryclose(ss, &cmd->u.kick) == 0)
					close = 1;
				ptr += sizeof(cmd->u.kick);
				break;
			case 'S':
				if (trysend(ss, &cmd->u.send) < 0)
					close = 1;
				ptr += sizeof(cmd->u.send);
				break;
			case 'U':
				//udp socket can only be closed active
				tryudpsend(ss, &cmd->u.udpsend);
				ptr += sizeof(cmd->u.udpsend);
				break;
			case 'T':	//just to return from sp_wait
				return -1;
			default:
				silly_log("[socket] cmd_process:"
					"unkonw operation:%d\n",
					cmd->u.type);
				assert(!"oh, no!");
				break;
			}
	}
	
	return close;
}

int
silly_socket_poll()
{
	struct silly_socket *ss = SSOCKET;
	ev_run(ss->loop, 0);
	return 0;
}

int
silly_socket_init()
{
	int err;
	int fd[2] = {-1, -1};
	struct socket *s = NULL;
	struct silly_socket *ss = silly_malloc(sizeof(*ss));
	memset(ss, 0, sizeof(*ss));
	
	//use the pipe and not the socketpair because
	//the pipe will be automatic
	//when the data size small than PIPE_BUF
	err = pipe(fd);
	if (unlikely(err < 0))
		goto end;

	ss->loop = ev_loop_new(0);
	socketpool_init(ss);
	s = allocsocket(ss, fd[0], STYPE_CTRL, PROTOCOL_PIPE);
	assert(s);
	
	ss->w.data = s;
	ev_io_init(&ss->w, ev_io_cb, fd[0], EV_READ);
	ev_io_start(ss->loop, &ss->w);
	
	ss->reservefd = open("/dev/null", O_RDONLY);
	ss->ctrlsendfd = fd[1];
	ss->ctrlrecvfd = fd[0];
	ss->ctrlcount = 0;
	resize_cmdbuf(ss, CMDBUF_SIZE);
	SSOCKET = ss;
	return 0;
end:
	if (fd[0] >= 0)
		close(fd[0]);
	if (fd[1] >= 0)
		close(fd[1]);
	if (ss)
		silly_free(ss);

	return -errno;
}

void silly_socket_exit()
{
	int i;

	assert(SSOCKET);
	close(SSOCKET->reservefd);
	close(SSOCKET->ctrlsendfd);
	close(SSOCKET->ctrlrecvfd);
	
	for (i = 0; i < MAX_SOCKET_COUNT; i++) {
		struct socket *s = &SSOCKET->socketpool[i];
		enum stype type = s->type;
		if (type == STYPE_SOCKET ||
			type == STYPE_LISTEN ||
			type == STYPE_HALFCLOSE) {
			ev_io_stop(SSOCKET->loop, &s->watcher);
			close(s->fd);
		}
	}
	silly_free(SSOCKET->cmdbuf);
	silly_free(SSOCKET->socketpool);
	ev_loop_destroy(SSOCKET->loop);
	silly_free(SSOCKET);
	return ;
}

const char *
silly_socket_pollapi()
{
	//return SOCKET_POLL_API;
	int backend = ev_backend(SSOCKET->loop);
	switch(backend) {
		case EVBACKEND_SELECT:
			return "select";
		case EVBACKEND_POLL:
			return "poll";
  		case EVBACKEND_EPOLL:
		  	return "epoll";
  		case EVBACKEND_KQUEUE:
		  	return "kqueue";
		default:
			return "other poll";
	}
}

//ev_io的回调函数，用于异步接受回复
void 
ev_io_cb(struct ev_loop *loop, ev_io* watcher,int e)
{
	(void)loop;
	int err;
	
	struct socket *s;
	struct silly_socket *ss = SSOCKET;
	s = (struct socket*)watcher->data;
	if (s == NULL)
		return;

	switch (s->type) {
		case STYPE_LISTEN:
			assert(e & EV_READ);
			report_accept(ss, s);
			return;
		case STYPE_CONNECTING:
			s->type = STYPE_SOCKET;
			report_connected(ss, s);
			return;
		case STYPE_RESERVE:
			silly_log("[socket] poll reserve socket\n");
			return;
		case STYPE_HALFCLOSE:
		case STYPE_SOCKET:
		case STYPE_UDPBIND:
			break;
		case STYPE_CTRL:
			{
				int err = cmd_process(ss);
				if (err < 0)
				{
					ev_break(ss->loop,EVBREAK_ALL);
				}
			}
			return;
		default:
			silly_log("[socket] poll: unkonw socket type:%d\n", s->type);
			return;
	}

	if (e & EV_ERROR) {
		report_close(ss, s, 0);
		freesocket(ss, s);
		return;
	}
	
	if (e & EV_READ) {
		switch (s->protocol) {
		case PROTOCOL_TCP:
			err = forward_msg_tcp(ss, s);
			break;
		case PROTOCOL_UDP:
			err = forward_msg_udp(ss, s);
			break;
		default:
			silly_log("[socket] poll:"
				"unsupport protocol:%d\n",
				s->protocol);
			return;
		}
		//this socket have already occurs error, so ignore the write event
		if (err < 0)
			return;
	}
	
	if (e & EV_WRITE) {
		if (s->protocol == PROTOCOL_TCP)
			send_msg_tcp(ss, s);
		else
			send_msg_udp(ss, s);
	}
}
