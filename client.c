#include "util.h"
#include "pipe.h"
#include "define.h"

#include <sys/types.h>	// for kevent struct
#include <sys/event.h>
#include <unistd.h>

#include <regex.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>

int g_tobe_terminate;

static int
about_term(void) {
	return g_tobe_terminate;
}

static void
catch_term(int signo) {
	(void)signo;
	g_tobe_terminate = 1;
}

static void
install_signals(void) {
	struct sigaction sa;
	sa.sa_handler = catch_term;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

typedef void (*Handler)(void *);

struct udata {
	void *context;
	Handler read;
	Handler write;
};

enum {
	ClientNone,
	ClientAuth,
	ClientLive,
};

struct client {
	struct udata udata;
	int fd;
	int state;
	int eventfd;
	struct string name;
	struct spipe *rpipe;
	struct spipe *wpipe;
	char *buffer;
	size_t buflen;
	char errstr[1024];
	regex_t login_regexp;
	regex_t speak_regexp;
	regex_t talk2_regexp;
	regex_t member_regexp;
	regex_t logout_regexp;
};

static void
client_do_work(struct client *c, const char *buf, size_t len) {
	(void)c;
	assert(buf[len-1] == '\0');
	printf("%s\n", buf+6);
}

static void
client_recvmsg(struct client *c, size_t len) {
	if (len > c->buflen) {
		c->buflen = len*2;
		c->buffer = realloc(c->buffer, c->buflen);
	}
	struct iovec v[2];
	char *dst = c->buffer;
	size_t pos = 0;
	do {
		spipe_readv(c->rpipe, v);
		size_t cpy = iovec_copyout(v, dst+pos, len-pos);
		spipe_readn(c->rpipe, cpy);
		pos += cpy;
	} while (len != pos);
	client_do_work(c, dst, len);
	if (c->state == ClientAuth) {
		c->state = ClientLive;
	}
}

static void
client_do_recv(struct client *c) {
	struct iovec v[2];
	struct msghdr msgdes;
	msghdr_init_tcp(&msgdes, v, 2);
	size_t len;
	ssize_t rcv;
	do {
		len = spipe_writev(c->rpipe, v);
retry:
		rcv = recvmsg(c->fd, &msgdes, 0);
		if (rcv == -1) {
			switch (errno) {
			default:
			case ECONNRESET:
				close(c->fd);
				exit(0);
			case EWOULDBLOCK:
				break;
			case EINTR:
				goto retry;
			}
		}
		if (rcv == 0) {
			close(c->fd);
			exit(0);
			break;
		}
		spipe_writen(c->rpipe, (size_t)rcv);
	} while ((size_t)rcv == len);
	for (;;) {
		if (spipe_readv(c->rpipe, v) <= 4) break;
		size_t rcvlen = iovec_read_uint32(v);
		if (rcvlen > spipe_space(c->rpipe)) break;
		client_recvmsg(c, rcvlen);
	}
}

static void
client_do_send(struct client *c) {
	struct iovec v[2];
	struct msghdr mhdr;
	size_t len;
	msghdr_init_tcp(&mhdr, v, 2);
	while ((len = spipe_readv(c->wpipe, v))) {
		ssize_t n = sendmsg(c->fd, &mhdr, 0);
		if (n == -1) {
			int error = errno;
			switch (error) {
			case ECONNRESET:
				close(c->fd);
				exit(0);
			case EWOULDBLOCK:
				return;
			case EINTR:
				continue;
			default:
				perror("sendmsg fail");
				abort();
			}
		}
		spipe_readn(c->wpipe, (size_t)n);
		if ((size_t)n < len) {
			break;
		}
	}
}

static void
client_write_buffer(struct client *c, const char *buf, size_t len) {
	if (spipe_space(c->wpipe) == 0) {
		ssize_t n;
retry:
		n = send(c->fd, buf, len, 0);
		if (n == -1) {
			int error = errno;
			switch (error) {
			case EWOULDBLOCK:
				break;
			case EINTR:
				goto retry;
			default:
				perror("send() fail");
				abort();
			}
		}
		len -= (size_t)n;
		buf += (size_t)n;
	}
	if (len != 0) {
		spipe_writeb(c->wpipe, buf, len);
		struct kevent e;
		EV_SET(&e, c->fd, EVFILT_WRITE, EV_ADD|EV_CLEAR|EV_ONESHOT, 0, 0, &c->udata);
		kevent(c->eventfd, &e, 1, NULL, 0, NULL);
	}
}

static void
client_chat(struct client *c, char *str) {
	regmatch_t matchs[3];
	int err = regexec(&c->speak_regexp, str, 2, matchs, 0);
	if (err == 0) {
		const char midstr[] = " say ";
		size_t midlen = sizeof(midstr)-1;
		size_t msglen = matchs[1].rm_eo - matchs[1].rm_so;
		size_t sndlen = 6 + c->name.len + midlen + msglen + 1;
		if (sndlen > c->buflen) {
			c->buflen = 2*sndlen;
			c->buffer = realloc(c->buffer, c->buflen);
		}
		char *sndbuf = c->buffer;
		*((uint32_t*)sndbuf) = sndlen;
		*((uint16_t*)(sndbuf+4)) = MessageTypeChat;
		char *body = sndbuf+6;
		memcpy(body, c->name.str, c->name.len);
		memcpy(body+c->name.len, midstr, midlen);
		memcpy(body+c->name.len+midlen, str+matchs[1].rm_so, msglen);
		*(body+c->name.len+midlen+msglen) = '\0';
		client_write_buffer(c, sndbuf, sndlen);
		return;
	} else if (err == REG_NOMATCH) {
		err = regexec(&c->talk2_regexp, str, 3, matchs, 0);
		if (err == 0) {
			const char midstr[] = " send to you: ";
			size_t midlen = sizeof(midstr)-1;
			size_t usrlen = matchs[1].rm_eo - matchs[1].rm_so;
			size_t msglen = matchs[2].rm_eo - matchs[2].rm_so;
			size_t chunklen = 6 + c->name.len + midlen + msglen + 1 + usrlen + 1;
			if (chunklen > c->buflen) {
				c->buflen = 2*chunklen;
				c->buffer = realloc(c->buffer, c->buflen);
			}
			char *chunkptr = c->buffer;
			*((uint32_t*)chunkptr) = (uint32_t)chunklen; 
			*((uint16_t*)(chunkptr+4)) = MessageTypeMail;
			memcpy(chunkptr+6, c->name.str, c->name.len);
			memcpy(chunkptr+6+c->name.len, midstr, midlen);
			memcpy(chunkptr+6+c->name.len+midlen, str+matchs[2].rm_so, msglen);
			*(chunkptr+6+c->name.len+midlen+msglen) = '\0';
			memcpy(chunkptr+6+c->name.len+midlen+msglen+1, str+matchs[1].rm_so, usrlen);
			*(chunkptr+6+c->name.len+midlen+msglen+1+usrlen) = '\0';
			client_write_buffer(c, chunkptr, chunklen);
			return;
		}
	}
	if (err != REG_NOMATCH) {
	}
}

static void
client_auth(struct client *c, char *str) {
	if (c->state != ClientNone) {
		printf("Already login ...\n");
		return;
	}
	regmatch_t matchs[3];
	int err = regexec(&c->login_regexp, str, 3, matchs, 0);
	if (err) {
		regerror(err, &c->login_regexp, c->errstr, sizeof(c->errstr));
		printf("invalid login syntax, see below:\n"
			":login user:yourname password:yourpass\n");
		printf("origin string{{%s}}\n", str);
		printf("%s:%s:%d %s\n", __FILE__, __func__, __LINE__, c->errstr);
		return;
	}
	size_t msglen = matchs[1].rm_eo - matchs[1].rm_so;
	c->name.str = malloc(msglen+1);
	memcpy(c->name.str, str+matchs[1].rm_so, msglen);
	c->name.str[msglen] = '\0';
	c->name.len = msglen;
	size_t sndlen = 6 + msglen + 1;
	if (sndlen > c->buflen) {
		c->buflen = 2*sndlen;
		c->buffer = realloc(c->buffer, c->buflen);
	}
	char *sndbuf = c->buffer;
	*((uint32_t*)sndbuf) = (uint32_t)sndlen;
	*((uint16_t*)(sndbuf+4)) = MessageTypeAuth;
	char *msg = sndbuf+6;
	memcpy(msg, str+matchs[1].rm_so, msglen);
	msg[msglen] = '\0';
	client_write_buffer(c, sndbuf, sndlen);
	c->state = ClientAuth;
}

static void
client_work(struct client *c, char *str) {
	int err = regexec(&c->logout_regexp, str, 0, NULL, 0);
	if (err == 0) {
		c->state = ClientNone;
		return;
	}
	if (err != REG_NOMATCH) {
		regerror(err, &c->logout_regexp, c->errstr, sizeof(c->errstr));
		fprintf(stderr, "%s:%s:%d %s\n", __FILE__, __func__, __LINE__, c->errstr);
		return;
	}
	switch (c->state) {
	case ClientNone:
		client_auth(c, str);
		break;
	case ClientAuth:
	case ClientLive:
		client_chat(c, str);
		break;
	}
}

const char login_pattern[] = "^:login[ \t]+user:([a-z]+)[ \t]+password:([a-z]+)$";
const char speak_pattern[] = "^:speak[ \t]+(.*)$";
const char talk2_pattern[] = "^:talk2[ \t]+user:([a-z]*)[ \t]+message:(.*)$";
const char member_pattern[] = "^:member[ \t]*$";
const char logout_pattern[] = "^:logout[ \t]*$";

static void
regex_comp(regex_t *re, const char *pattern) {
	char errstr[1024];
	int err = regcomp(re, pattern, REG_EXTENDED|REG_NEWLINE);
	if (err) {
		regerror(err, re, errstr, sizeof(errstr));
		fprintf(stderr, "%s\n", errstr);
		regfree(re);
	}
}

static void
client_init(struct client *c, int conn, int eventfd) {
	c->fd = conn;
	c->state = ClientNone;
	c->eventfd = eventfd;
	c->udata.context = c;
	c->udata.read = (Handler)client_do_recv;
	c->udata.write = (Handler)client_do_send;
	c->buflen = 2048;
	c->buffer = malloc(c->buflen);
	c->wpipe = spipe_create(2048, 1);
	c->rpipe = spipe_create(2048, 1);
	c->name.str = NULL;
	c->name.len = 0;
	regex_comp(&c->login_regexp, login_pattern);
	regex_comp(&c->speak_regexp, speak_pattern);
	regex_comp(&c->talk2_regexp, talk2_pattern);
	regex_comp(&c->member_regexp, member_pattern);
	regex_comp(&c->logout_regexp, logout_pattern);
	struct kevent e;
	EV_SET(&e, conn, EVFILT_READ, EV_ADD|EV_CLEAR, 0, 0, &c->udata);
	kevent(eventfd, &e, 1, NULL, 0, NULL);
}

struct input {
	struct udata udata;
	struct client *out;
	int fd;
	int state;
};

static void
input_read(struct input *in) {
	char buf[2048];
	for (;;) {
		ssize_t n = read(in->fd, buf, sizeof(buf)-1);
		if (n == -1) {
			int error = errno;
			switch (error) {
			case EWOULDBLOCK:
				return;
			case EINTR:
				continue;
			default:
				perror("read() stdin error");
				abort();
			}
		}
		assert(n != -1);
		buf[n] = 0;
		client_work(in->out, buf);
	}
}

static void
input_init(struct input *in, struct client *out) {
	in->fd = STDIN_FILENO;
	in->out = out;
	in->udata.context = in;
	in->udata.read = (Handler)input_read;
	in->udata.write = NULL;
}

// server address
int
main(int argc, char *argv[]) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 2) {
		printf("usage: client server_address.\n");
		exit(0);
	}
	char *address = argv[1];
	int conn = connectTCP(address, NULL, NULL);
	if (conn == -1) {
		perror("connectTCP() fail");
		exit(0);
	}
	socket_clr_blocking(conn);
	socket_clr_blocking(STDIN_FILENO);

	install_signals();

	int eventfd = kqueue();

	struct client out;
	client_init(&out, conn, eventfd);

	struct input in;
	input_init(&in, &out);

	struct kevent ev[4];
	EV_SET(&ev[0], STDIN_FILENO, EVFILT_READ, EV_ADD, 0, 0, &in.udata);
	kevent(eventfd, ev, 1, NULL, 0, NULL);
	for (;;) {
		int n = kevent(eventfd, NULL, 0, ev, 4, NULL);
		if (n != -1) {
			int i;
			for (i=0; i<n; i++) {
				struct kevent *e = ev+i;
				struct udata *ud = e->udata;
				if ((e->flags & EV_EOF) || (e->filter == EVFILT_READ)) {
					ud->read(ud->context);
				} else if (e->filter == EVFILT_WRITE) {
					ud->write(ud->context);
				}
			}
		}
		if (about_term()) {
			break;
		}
	}
	close(conn);
	return 0;
}
