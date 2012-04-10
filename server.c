#include "util.h"
#include "pipe.h"
#include "define.h"

#include <sys/socket.h>
#include <sys/event.h>
#include <sys/uio.h>	// for readv, writev
#include <netdb.h>
#include <unistd.h>
#include <signal.h>

#include <pthread.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

struct session;
struct verifier;
static void verifier_mulcast(struct verifier *v, const char *buf, size_t len, struct session *owner);
static void verifier_unicast(struct verifier *v, const char *buf, size_t len, const struct string *peer);

#define ARRAY_NELEM(arr)	(sizeof(arr)/sizeof(arr[0]))

typedef void (*Handler)(void *context);

struct udata {
	void *context;
	Handler read;
	Handler write;
};

#include <limits.h>
#define FD_UNIT_NBITS	(sizeof(fdunit_t)*CHAR_BIT)
typedef unsigned long fdunit_t;
struct verifier {
	int eventfd;
	size_t nclient;
	struct session **clients;
	int maxfd;
	struct session *free_sessions;
	// map name ==> client
};

enum {
	SessionStateNone,
	SessionStateAuth,
	SessionStateNorm,
};

struct session {
	struct udata udata;
	struct verifier *verifier;
	int fd;
	int eventfd;
	int state;
	char *msgbuf;
	size_t buflen;
	struct string name;
	struct spipe *rpipe;
	struct spipe *wpipe;
	struct session *nextfree;
};

//static size_t
//veccpy(struct iovec dst[1], struct iovec src[2], size_t size) {
//	size_t len = dst->iov_len;
//	if (size <= len) {
//		len = size;
//	}
//	dst->iov_len = len;
//	if (len == 0) {
//		return;
//	}
//	size_t l0 = src[0].iov_len;
//	if (l0 >= len) {
//		dst->iov_base = src[0].iov_base;
//	} else {
//		memcpy(dst->iov_base, src[0].iov_base, l0);
//		size_t l1 = len - l0;
//		memcpy((char*)dst->iov_base + l0, src[1].iov_base, l1);
//	}
//	return len;
//}
//
static size_t
iovec_read_vec(struct iovec src[2], size_t *szp, char *buf, size_t len, struct iovec dst[1]) {
	size_t size = *szp;
	if (size <= len) {
		len = size;
	}
	dst->iov_len = len;
	if (len == 0) {
		return 0;
	}
	*szp = size - len;
	size_t l0 = src[0].iov_len;
	if (l0 >= len) {
		dst->iov_base = src[0].iov_base;
		if (l0 == len) {
			src[0] = src[1];
			src[1].iov_len = 0;
		} else {
			src[0].iov_base = (char*)src[0].iov_base + len;
			src[0].iov_len = l0 - len;
		}
	} else {
		dst->iov_base = buf;
		memcpy(buf, src[0].iov_base, l0);
		size_t l1 = len - l0;
		memcpy(buf+l0, src[1].iov_base, l1);
	}
	return len;
}

static size_t
iovec_read_buf(struct iovec src[2], size_t *szp, char *buf, size_t len) {
	struct iovec tmp[1];
	size_t n = iovec_read_vec(src, szp, buf, len, tmp);
	if (tmp->iov_base != buf) {
		memcpy(buf, tmp->iov_base, n);
	}
	return n;
}

enum {
	SessionMessageNone,
	SessionMessageType,
	SessionMessageSize,
	SessionMessageDone,
};

static unsigned
iovec_read_uint16(struct iovec v[2]) {
	union {
		char bytes[2];
		uint16_t integer;
	} u16;
	if (v[0].iov_len >= 2) {
		u16.bytes[0] = ((char*)v[0].iov_base)[0];
		u16.bytes[1] = ((char*)v[0].iov_base)[1];
		if (v[0].iov_len == 2) {
			v[0] = v[1];
			v[1].iov_len = 0;
		} else {
			v[0].iov_base = (char*)v[0].iov_base + 2;
			v[0].iov_len -= 2;
		}
	} else {
		assert(v[0].iov_len != 0);
		u16.bytes[0] = ((char*)v[0].iov_base)[0];
		u16.bytes[1] = ((char*)v[1].iov_base)[0];
		v[0].iov_base = (char*)v[1].iov_base + 1;
		v[0].iov_len = v[1].iov_len - 1;
		v[1].iov_len = 0;
	}
	return u16.integer;
}

static void verifier_detach_session(struct verifier *v, int fd, struct session *s);

static size_t
spipe_readb(struct spipe *src, char *dst, size_t len) {
	struct iovec v[2];
	size_t pos = 0;
	while (pos != len && spipe_readv(src, v)) {
		size_t cpy = iovec_copyout(v, dst+pos, len-pos);
		spipe_readn(src, cpy);
		pos += cpy;
	}
	return pos;
}

static void
session_write_buffer(struct session *s, const char *buf, size_t len) {
	printf("%s:%s:%d\n", __FILE__, __func__, __LINE__);
	assert(len != 0);
	if (s->fd == -1) {
		return;
	}
	struct iovec v[2];
	int fd = s->fd;
	if (!spipe_readv(s->wpipe, v)) {
		// No data in pipe, try write directly.
		ssize_t n;
retry:
		n = send(fd, buf, len, MSG_DONTWAIT);
		if (n == -1) {
			int error = errno;
			switch (error) {
			case EWOULDBLOCK:
				n = 0;
				break;
			case EINTR:
				goto retry;
			case ECONNRESET:
				close(s->fd);
				verifier_detach_session(s->verifier, s->fd, s);
				s->fd = -1;
				break;
			default:
				perror("send() failure");
				abort();
			}
		}
		assert(n != -1);
		size_t wrt = (size_t)n;
		buf += wrt;
		len -= wrt;
	}
	if (len != 0) {
		spipe_writeb(s->wpipe, buf, len);
		// TODO Ensure this !
		struct kevent ev;
		EV_SET(&ev, fd, EVFILT_WRITE, EV_ENABLE, 0, 0, &s->udata);
		kevent(s->eventfd, &ev, 1, NULL, 0, NULL);
	}
}

static const char authsucc_string[] = "Authorization successful.";
static void
session_do_auth(struct session *s, const char *msg, size_t len) {
	int typ = *((uint16_t*)(msg+4));
	if (typ != MessageTypeAuth) {
		return;
	}
	s->name.str = malloc(len-6);
	s->name.len = len-7;
	strcpy(s->name.str, msg+6);
	printf("new name[%s].\n", s->name.str);
	char *sndbuf = s->msgbuf;
	size_t sndlen = 6 + sizeof(authsucc_string);
	*((uint32_t*)sndbuf) = sndlen;
	*((uint16_t*)(sndbuf+4)) = MessageTypeAuth;
	strcpy(sndbuf+6, authsucc_string);
	session_write_buffer(s, sndbuf, sndlen);
	s->state = SessionStateNorm;
}

static void
session_do_chat(struct session *s, const char *msg, size_t len) {
	printf("%s:%s:%d\n", __FILE__, __func__, __LINE__);
	verifier_mulcast(s->verifier, msg, len, s);
}

static const char *
memrchr(const char *str, int c, size_t len) {
	const char *end = str+len;
	while (end-- > str) {
		if (*end == c) {
			return end;
		}
	}
	return NULL;
}

static void
session_do_mail(struct session *s, const char *msg, size_t len) {
	printf("%s:%s:%d\n", __FILE__, __func__, __LINE__);
	const char *term = memrchr(msg+6, '\0', len-7) + 1;
	*((uint32_t*)msg) = (uint32_t)(term-msg);
	struct string peer;
	peer.str = (char*)term;
	peer.len = (size_t)((msg+len-1) - peer.str);
	verifier_unicast(s->verifier, msg, *((uint32_t*)msg), &peer);
}

static void
session_do_work(struct session *s, size_t len) {
	printf("%s:%s:%d\n", __FILE__, __func__, __LINE__);
	if (len > s->buflen) {
		s->buflen = len*2;
		s->msgbuf = realloc(s->msgbuf, s->buflen);
	}
	char *msg = s->msgbuf;
	spipe_readb(s->rpipe, msg, len);

	if (SessionStateNone == s->state) {
		session_do_auth(s, msg, len);
		return;
	}
	int typ = *((uint16_t*)(msg+4));
	switch (typ) {
	case MessageTypeChat:
		session_do_chat(s, msg, len);
		break;
	case MessageTypeMail:
		session_do_mail(s, msg, len);
		break;
	default:
		break;
	}
}

static void
session_do_send(struct session *s) {
	int fd = s->fd;
	if (fd == -1) {
		return;
	}
	struct iovec v[2];
	size_t size = spipe_readv(s->wpipe, v);
	if (size != 0) {
		struct msghdr msgdes;
		msghdr_init_tcp(&msgdes, v, 2);
		do {
			ssize_t n = sendmsg(fd, &msgdes, 0);
			if (n == -1) {
				int error = errno;
				switch (error) {
				case EWOULDBLOCK:
					return;
				case ECONNRESET:
					close(fd);
					verifier_detach_session(s->verifier, s->fd, s);
					s->fd = -1;
					return;
				case EINTR:
					continue;
				}
			}
			assert(n != -1);
			spipe_readn(s->wpipe, (size_t)n);
			size = spipe_readv(s->wpipe, v);
		} while (size != 0);
	}
}

static void
session_do_recv(struct session *s) {
	fprintf(stderr, "%s:%s:%d\n", __FILE__, __func__, __LINE__);
	if (s->fd == -1) {
		return;
	}
	struct iovec v[2];
	struct msghdr msgdes;
	msghdr_init_tcp(&msgdes, v, 2);
	struct spipe *r = s->rpipe;
	int fd = s->fd;
	fprintf(stderr, "%s:%s:%d\n", __FILE__, __func__, __LINE__);
	for (;;) {
		size_t len = spipe_writev(r, v);
		ssize_t rcv = recvmsg(fd, &msgdes, 0);
		if (rcv == -1) {
			switch (errno) {
			default:
			case ECONNRESET:
				close(fd);
				verifier_detach_session(s->verifier, s->fd, s);
				s->fd = -1;
			case EWOULDBLOCK:
				break;
			case EINTR:
				continue;
			}
		}
		if (rcv == 0) {
			close(fd);
			verifier_detach_session(s->verifier, s->fd, s);
			s->fd = -1;
			break;
		}
		spipe_writen(r, (size_t)rcv);
		if ((size_t)rcv < len) {
			break;
		}
	}
	size_t len = spipe_readv(s->rpipe, v);
	if (len > 4) {
		size_t msglen = iovec_read_uint32(v);
		if (msglen >= spipe_space(s->rpipe)) {
			session_do_work(s, msglen);
		}
	}
}

static void
session_init(struct session *s, struct verifier *v, struct spipe *pipe[2]) {
	s->fd = -1;
	s->verifier = v;
	s->rpipe = pipe[0];
	s->wpipe = pipe[1];
	s->buflen = 1024;
	s->msgbuf = malloc(s->buflen);
	s->udata.context = s;
	s->udata.write = (Handler)session_do_send;
}

static void
session_ctor(struct session *s, int fd) {
	s->fd = fd;
	s->state = SessionStateNone;
	s->name.str = NULL;
	s->name.len = 0;
	s->udata.read = (Handler)session_do_recv;
}

static void
session_dtor(struct session *d) {
	free(d->name.str);
	spipe_fini(d->rpipe);
	spipe_fini(d->wpipe);
	d->fd = -1;
}

static void
session_fini(struct session *d, struct spipe *pipe[2]) {
	pipe[0] = d->rpipe;
	pipe[1] = d->wpipe;
}

//static void
//verifier_addfd(struct verifier *v, int fd) {
//	size_t i = (size_t)(fd/FD_UNIT_NBITS);
//	if (i >= v->cnt) {
//		size_t n = 2*i+10;
//		v->fds = realloc(v->fds, n);
//		memzero(&v->fds[v->cnt], n-v->cnt);
//		v->cnt = n;
//	}
//	size_t f = (size_t)(fd%FD_UNIT_NBITS);
//	size_t m = 1<<f;
//	fdunit_t old = __sync_fetch_and_or(v->fds+i, m);
//	assert((old & m) == 0);
//}
//
//static void
//verifier_clrfd(struct verifier *v, int fd) {
//	size_t i = (size_t)(fd/FD_UNIT_NBITS);
//	size_t f = (size_t)(fd%FD_UNIT_NBITS);
//	size_t m = 1<<f;
//	fdunit_t old = __sync_fetch_and_and(v->fds+i, ~m);
//	assert((old & m) == 1);
//}
//
//struct verifier *
//verifier_create(int eventfd) {
//	struct verifier *v = malloc(sizeof(*v));
//	v->eventfd = eventfd;
//	v->cnt = 256;
//	v->fds = calloc(v->cnt, sizeof(fdunit_t));
//	return v;
//}
//
static void
verifier_unicast(struct verifier *v, const char *buf, size_t len, const struct string *peer) {
	struct session **clients = v->clients;
	for (size_t i=0, n=v->maxfd; i<=n; i++) {
		struct session *s = clients[i];
		if (s != NULL
		&& s->fd != -1
		&& s->name.len == peer->len
		&& memcmp(s->name.str, peer->str, peer->len) == 0) {
			session_write_buffer(s, buf, len);
		}
	}
}

static void
verifier_mulcast(struct verifier *v, const char *buf, size_t len, struct session *owner) {
	struct session **clients = v->clients;
	size_t i=0, n=v->maxfd;
	while (i<=n) {
		struct session *s = clients[i];
		if (s != NULL && s != owner && s->fd != -1 && s->state == SessionStateNorm) {
			session_write_buffer(s, buf, len);
		}
		i++;
	}
}

static struct session *
verifier_create_session(struct verifier *v) {
	struct session *s = malloc(sizeof(*s));
	struct spipe *pipe[2];
	pipe[0] = spipe_create(1024, 1);
	pipe[1] = spipe_create(1024, 1);
	session_init(s, v, pipe);
	return s;
}

static void
verifier_delete_session(struct verifier *v, struct session *s) {
	(void)v;
	struct spipe *pipe[2];
	session_fini(s, pipe);
	free(s);
	spipe_delete(pipe[0]);
	spipe_delete(pipe[1]);
}

static struct session *
verifier_accept_session(struct verifier *v, int fd) {
	struct session *s = v->clients[fd];
	if (s == NULL) {
		s = verifier_create_session(v);
		v->clients[fd] = s;
	}
	assert(s->fd == -1);
	session_ctor(s, fd);
	struct kevent ev[2];
	EV_SET(&ev[0], fd, EVFILT_READ, EV_ADD, 0, 0, &s->udata);
	EV_SET(&ev[1], fd, EVFILT_WRITE, EV_ADD, 0, 0, &s->udata);
	kevent(v->eventfd, ev, 2, NULL, 0, NULL);
	fprintf(stderr, "oops\n");
	return s;
}

static void
verifier_detach_session(struct verifier *v, int fd, struct session *s) {
	session_dtor(s);
	assert(s->fd == -1);
	struct kevent ev[2];
	EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
	EV_SET(&ev[0], fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
	kevent(v->eventfd, ev, 2, NULL, 0, NULL);
}

static void
verifier_new_client(struct verifier *v, int fd) {
	fprintf(stderr, "%s:%s:%d\n", __FILE__, __func__, __LINE__);
	fprintf(stderr, "new client[%d].\n", fd);
	if (fd > v->maxfd) {
		v->maxfd = fd;
	}
	fprintf(stderr, "%s:%s:%d\n", __FILE__, __func__, __LINE__);
	if ((size_t)fd >= v->nclient) {
		fprintf(stderr, "%s:%s:%d\n", __FILE__, __func__, __LINE__);
		size_t nclient = fd + fd/2;
		v->clients = realloc(v->clients, nclient);
		memzero(&v->clients[v->nclient], (nclient - v->nclient)*sizeof(void*));
		v->nclient = nclient;
	}
	fprintf(stderr, "%s:%s:%d\n", __FILE__, __func__, __LINE__);
	verifier_accept_session(v, fd);
}

struct listener {
	struct udata udata;
	int fd;
	struct verifier *verifier;
};

static void
listener_accept(struct listener *l) {
	int fd = l->fd;
	char addr[NI_MAXHOST], port[NI_MAXSERV];
	for (;;) {
		struct sockaddr_storage saddr;
		socklen_t salen = sizeof(saddr);
		int client_fd = accept(fd, (struct sockaddr *)&saddr, &salen);
		if (client_fd == -1) {
			switch (errno) {
			case EWOULDBLOCK:
				return;
			case EINTR:
				continue;
			default:
				perror("accept() failure");
				abort();
			}
		}
		int err = getnameinfo((struct sockaddr *)&saddr, salen, addr, sizeof(addr), port, sizeof(port), NI_NUMERICHOST|NI_NUMERICSERV);
		if (err != 0) {
			fprintf(stderr, "getnameinfo(client) failure: %s.\n", gai_strerror(err));
		} else {
			fprintf(stderr, "new client address %s:%s\n", addr, port);
		}
		assert(client_fd != -1);
		verifier_new_client(l->verifier, client_fd);
	}
}

static void
listener_write(struct listener *l) {
	(void)l;
	assert(!"non reach");
}

static void
makepair(struct listener *l, struct verifier *v, int servefd, int eventfd) {
	l->fd = servefd;
	l->udata.context = l;
	l->udata.read = (Handler)listener_accept;
	l->udata.write = (Handler)listener_write;
	l->verifier = v;

	v->eventfd = eventfd;
	v->nclient = 1024;
	v->clients = calloc(v->nclient, sizeof(void*));
	v->maxfd = 0;
}

static bool g_tobe_terminate;

static void
set_terminate(int signo) {
	(void)signo;
	g_tobe_terminate = true;
}

static bool
tobe_terminate(void) {
	return g_tobe_terminate;
}

static void
install_signals(void) {
	struct sigaction sa;
	sa.sa_handler = set_terminate;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void *
kevent_loop(int eventfd) {
	struct kevent ev[256];
	for (;;) {
		int n = kevent(eventfd, NULL, 0, ev, ARRAY_NELEM(ev), NULL);
		if (n > 0) {
			while (n--) {
				struct udata *ud = ev[n].udata;
				if ((ev[n].flags & EV_EOF)) {
					ud->read(ud->context);
					printf("kevent EV_EOF.\n");
					continue;
				}
				if (ev[n].filter == EVFILT_READ) {
					ud->read(ud->context);
				} else if (ev[n].filter == EVFILT_WRITE) {
					ud->write(ud->context);
				}
			}
		}
		if (tobe_terminate()) {
			break;
		}
	}
//	for (;;) {
//		if (term()) {
//			break;
//		}
//		int n = kevent(eventfd, NULL, 0, ebuf, ARRAY_NELEM(ebuf), NULL);
//		// struct event {
//		// 	int fd;
//		// 	struct udata *udata;
//		// 	struct kevent *kevent;
//		// };
//		// TODO Callback functions may delete udata.
//		for (int i=0; i<n; i++) {
//			dispatch(d, &ebuf[i]);
//		}
//	}
	return NULL;
}

int
kevent_add(int eventfd, int fd, int filter, void *udata) {
	struct kevent ev;
	EV_SET(&ev, fd, filter, EV_ADD, 0, 0, udata);
	return kevent(eventfd, &ev, 1, NULL, 0, NULL);
}

int
kevent_del(int eventfd, int fd, int filter) {
	struct kevent ev;
	EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, NULL);
	return kevent(eventfd, &ev, 1, NULL, 0, NULL);
}

int
main(int argc, char *argv[]) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 2) {
		printf("usage: server server_addr.\n");
		exit(0);
	}
// TODO
// Input:
//   argv[1]	server_name
//   argv[2]	server_addr	need to be a WAN address
//   argv[3]	auth_addr
//	char *server_name = argv[1];
	char *server_addr = argv[1];

//	int regfd = -1;
//	for (int i=0; i<100; i++) {
//		printf("Try connecting to registry %d ...\n", i);
//		regfd = connectTCP(registry_addr, NULL, NULL);
//		if (regfd < 0) {
//			printf("Try %d failure :{{%s}}.\n", i, strerror(errno));
//			sleep(30);
//			continue;
//		}
//		break;
//	}
//	if (regfd < 0) {
//		fprintf(stderr, "Can't connect to registry {{%s}}.\n", registry_addr);
//		return 0;
//	}
//	 TODO
//	up(regfd, server_name, server_addr);

	install_signals();

	int servefd = listenTCP(server_addr, 100);
	socket_clr_blocking(servefd);
	int eventfd = kqueue();
	struct listener l;
	struct verifier v;
	makepair(&l, &v, servefd, eventfd);
	kevent_add(eventfd, servefd, EVFILT_READ, &l.udata);

	kevent_loop(eventfd);

//	down(regfd, server_name);
//	close(regfd);
	close(servefd);
	return 0;
}
