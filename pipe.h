#ifndef __PIPE_H__
#define __PIPE_H__

#include "define.h"

#include <stddef.h>
#include <stdbool.h>
#include <sys/uio.h>

struct spipe;

struct spipe *spipe_create(size_t ntiem, size_t isize);
void spipe_delete(struct spipe *);
size_t spipe_memory(struct spipe *p);

size_t spipe_size(void);
void spipe_init(struct spipe *p, size_t nitem, size_t isize);
void spipe_fini(struct spipe *p);

// 0 means there is no data available, and we need go to sleep.
size_t spipe_space(struct spipe *p);
size_t spipe_readv(struct spipe *p, struct iovec v[2]);
void spipe_readn(struct spipe *p, size_t n);

size_t spipe_writev(struct spipe *p, struct iovec v[2]);

// True means reader is in sleeping.
bool spipe_writen(struct spipe *p, size_t n);

static void
spipe_writeb(struct spipe *dst, const char *buf, size_t len) {
	if (len != 0) {
		struct iovec v[2];
		size_t pos = 0;
		do {
			spipe_writev(dst, v);
			size_t n = iovec_copyin(v, buf+pos, len-pos);
			spipe_writen(dst, n);
			pos += n;
		} while (len != pos);
	}
}

struct bpipe;

size_t bpipe_size(void);
void bpipe_init(struct bpipe *p);
void bpipe_ctor(struct bpipe *p);
void bpipe_dtor(struct bpipe *p);
void bpipe_fini(struct bpipe *p);

int bpipe_getfd(struct bpipe *p);

size_t bpipe_readv(struct bpipe *p, struct iovec v[2]);
void bpipe_readn(struct bpipe *p, size_t n);

size_t bpipe_writev(struct bpipe *p, struct iovec v[2]);
void bpipe_writen(struct bpipe *p, size_t n);

#endif
