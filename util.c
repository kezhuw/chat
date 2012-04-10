#include "util.h"

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>

#include <stdio.h>

int
socket_set_blocking(int fd) {
//	int flags = fcntl(fd, F_GETFL);
//	flags |= O_NONBLOCK;
//	return fcntl(fd, F_SETFL, flags);
	int nb = 0;
	return ioctl(fd, FIONBIO, &nb);
}

int
socket_clr_blocking(int fd) {
//	int flags = fcntl(fd, F_GETFL);
//	flags &= ~O_NONBLOCK;
//	return fcntl(fd, F_SETFL, flags);
	int nb = 1;
	return ioctl(fd, FIONBIO, &nb);
}

char *
split_addr(char *addr) {
	char *deli = strrchr(addr, ':');
	if (deli == NULL) {
		return NULL;
	}
	*deli = '\0';
	return (deli+1);
}

int
connectTCP(const char *address, SetSocketOptionFunc_t setopt, void *ud) {
	char temp[strlen(address)+1];
	strcpy(temp, address);
	char *addr = temp;
	char *serv = split_addr(temp);
	struct addrinfo hint;
	memzero(&hint, sizeof(hint));
	hint.ai_family = PF_UNSPEC;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_protocol= IPPROTO_TCP;
	struct addrinfo *res;
	int err = getaddrinfo(addr, serv, &hint, &res);
	if (err != 0) {
		return err;
	}
	struct addrinfo *i;
	int sockfd = -1;
	for (i=res; i!=NULL; i = i->ai_next) {
		sockfd = socket(i->ai_family, i->ai_socktype, i->ai_protocol);
		if (sockfd < 0) {
			continue;
		}
		if (setopt) {
			setopt(sockfd, ud);
		}
		if (connect(sockfd, i->ai_addr, i->ai_addrlen) < 0) {
			close(sockfd);
			sockfd = -1;
			continue;
		}
		break;
	}
	return sockfd;
}

int
listenTCP(const char *address, int backlog) {
	struct addrinfo hint;
	memzero(&hint, sizeof(hint));
	hint.ai_family = AF_UNSPEC;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_protocol = IPPROTO_TCP;
	hint.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	struct addrinfo *res;
	char temp[strlen(address)+1];
	strcpy(temp, address);
	char *addr = temp;
	char *serv = split_addr(temp);
	if (*addr == '*') {
		addr = NULL;
	}
	if (*serv == '*') {
		serv = "0";
	}
	int err = getaddrinfo(addr, serv, &hint, &res);
	if (err != 0) {
		fprintf(stderr, "getaddrinfo() fail: %s.\n", gai_strerror(err));
		return err;
	}
	// Loop through?
	int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	freeaddrinfo(res);
	if (sockfd < 0) {
		perror("socket()");
		return -1;
	}
	int opt = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int));
	err = bind(sockfd, res->ai_addr, res->ai_addrlen);
	if (err == -1) {
		perror("bind()");
		close(sockfd);
		return -1;
	}
	listen(sockfd, backlog);
	return sockfd;
}
