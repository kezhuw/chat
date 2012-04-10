#ifndef __UTIL_H__
#define __UTIL_H__

#include <sys/socket.h>
#include <string.h>
#include <stddef.h>

#define memzero(addr, size)	memset((addr), 0, (size))

struct string {
	char *str;
	size_t len;
};

#include <sys/uio.h>
static void
msghdr_init_tcp(struct msghdr *hdr, struct iovec *iov, size_t iovcnt) {
	hdr->msg_name = NULL;
	hdr->msg_namelen = 0;
	hdr->msg_iov = iov;
	hdr->msg_iovlen = iovcnt;
	hdr->msg_control = NULL;
	hdr->msg_controllen = 0;
	hdr->msg_flags = 0;
}

static inline int
getsockaddr(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict len) {
	return getsockname(sockfd, addr, len);
}

static inline int
getpeeraddr(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict len) {
	return getpeername(sockfd, addr, len);
}

typedef int (*SetSocketOptionFunc_t)(int fd, void *ud);

int connectTCP(const char *address, SetSocketOptionFunc_t setopt, void *ud);
int listenTCP(const char *address, int backlog);

int socket_set_blocking(int fd);
int socket_clr_blocking(int fd);

#endif
