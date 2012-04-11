#include "util.h"
#include "pipe.h"

#include <sys/socket.h>
#include <unistd.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

struct data {
	struct data *next;
	char bytes[];
};

struct spipe {
	char *guard;

	// reader
	char *rpos;

	// writer
	char *wpos;
	char *wend;
	char *wbeg;

	struct data * volatile frees;
	struct data *write;
	struct data *first;

	volatile size_t writesize;

#define FINI_SIZE	(offsetof(struct spipe, totalsize))
	size_t totalsize;
	size_t blocksize;
	size_t chunksize;
};

#define GUARD_INVAL	((char*)0x03)
void
spipe_init(struct spipe *s, size_t nitem, size_t isize) {
	memzero(s, sizeof(*s));
	s->totalsize = nitem*isize;
	s->blocksize = isize;
	s->chunksize = sizeof(struct data) + s->totalsize;
	s->guard = s->wbeg = s->rpos = GUARD_INVAL;
}

void
spipe_fini(struct spipe *s) {
	struct data *d = s->frees;
	while (d != NULL) {
		struct data *n = d->next;
		free(d);
		d = n;
	}
	if (s->write != NULL) {
		s->write->next = NULL;
		d = s->first;
		do {
			struct data *n = d->next;
			free(d);
			d = n;
		} while (d != NULL);
	}
	memzero(s, FINI_SIZE);
}

struct spipe *
spipe_create(size_t nitem, size_t isize) {
	struct spipe *s = malloc(sizeof(*s));
	spipe_init(s, nitem, isize);
	return s;
}

void
spipe_delete(struct spipe *s) {
	spipe_fini(s);
	free(s);
}

static struct data *
spipe_malloc_data(struct spipe *s) {
	struct data *d;
	if ((d = s->frees) != NULL) {
retry:
		// Only one thread can call this, if frees != NULL,
		// it will always true in this calling.
		d = s->frees;
		struct data *n = d->next;
		if (!__sync_bool_compare_and_swap(&s->frees, d, n)) {
			goto retry;
		}
		return d;
	}
	return malloc(s->chunksize);
}

static void
spipe_free_data(struct spipe *s, struct data *d) {
	assert(d != NULL);
	struct data *n;
retry:
	n = s->frees;
	// Setup next field before compare_and_swap().
	d->next = n;
	if (!__sync_bool_compare_and_swap(&s->frees, n, d)) {
		goto retry;
	}
}

size_t
spipe_space(struct spipe *s) {
	return s->writesize;
}

static bool
spipe_cwake(struct spipe *s) {
	// assert we had written some data.
	assert(s->wbeg != s->wpos);
	if (!__sync_bool_compare_and_swap(&s->guard, s->wbeg, s->wpos)) {
		s->guard = s->wbeg = s->wpos;
		return true;
	}
	s->wbeg = s->wpos;
	return false;
}

// Caller need to ensure than n is smaller than value returned from readv().
void
spipe_readn(struct spipe *s, size_t n) {
	assert(n%s->blocksize == 0);
	s->rpos += n;
	char *cend = (char*)s->first + s->chunksize;
	if (s->rpos >= cend) {
		struct data *d0 = s->first;
		s->first = d0->next;
		spipe_free_data(s, d0);
		n = (size_t)(s->rpos - cend);
		cend = (char*)s->first + s->chunksize;
		s->rpos = s->first->bytes + n;
		assert(s->rpos <= cend);
		if (s->rpos == cend) {
			struct data *d1 = s->first;
			s->first = d1->next;
			s->rpos = s->first->bytes;
			spipe_free_data(s, d1);
		}
		spipe_free_data(s, d0);
	}
	__sync_sub_and_fetch(&s->writesize, n);
}

#define __sync_acquire_lock(ptr)	\
do {					\
	bool b;				\
	while (!(b = __sync_bool_compare_and_swap((ptr), 0, 1))) {	\
	}				\
	assert(b == true);		\
} while(0)

#define __sync_release_lock(ptr)	\
do {					\
	bool b = __sync_bool_compare_and_swap((ptr), 1, 0);	\
	assert(b == true);		\
} while(0)


static inline char *
spipe_guard(struct spipe *s) {
	return __sync_val_compare_and_swap(&s->guard, s->rpos, NULL);
}

size_t
spipe_readv(struct spipe * restrict s, struct iovec v[2]) {
	char *guard = spipe_guard(s);
	if (guard == s->rpos || guard == GUARD_INVAL) {
		return 0;
	}
	char *cend = (char*)s->first + s->chunksize;
	if (guard > s->rpos && guard < cend) {
		// reader/writer in same chunk
		size_t len = (size_t)(guard - s->rpos);
		v[0].iov_base = s->rpos;
		v[0].iov_len = len;
		v[1].iov_base = NULL;
		v[1].iov_len = 0;
		return len;
	}
	assert(guard != cend);
	size_t l0 = (size_t)(cend - s->rpos);
	v[0].iov_base = s->rpos;
	v[0].iov_len = l0;
	struct data *d1 = s->first->next;
	if (guard >= (char*)d1 && guard <= (char*)d1+s->chunksize) {
		// writer is one step ahead of reader
		size_t l1 = (size_t)(guard - d1->bytes);
		v[1].iov_base = d1->bytes;
		v[1].iov_len = l1;
		return l0+l1;
	}
	v[1].iov_base = d1->bytes;
	v[1].iov_len = s->totalsize;
	return l0 + s->totalsize;
}

bool
spipe_writen(struct spipe *s, size_t n) {
	assert(n != 0);
	assert(n <= ((size_t)(s->wend - s->wpos) + s->totalsize));
	s->wpos += n;
	if (s->wpos >= s->wend) {
		n = (size_t)(s->wpos - s->wend);
		if (n < s->totalsize) {
			s->wpos = s->write->bytes + n;
			s->wend = (char*)s->write + s->chunksize;
			s->write->next = spipe_malloc_data(s);
			s->write = s->write->next;
		} else {
			assert(n == s->totalsize);
			struct data *d0 = spipe_malloc_data(s);
			struct data *d1 = spipe_malloc_data(s);
			s->wpos = d0->bytes;
			s->wend = (char*)d0 + s->chunksize;
			s->write->next = d0;
			d0->next = d1;
			s->write = d1;
		}
	}
	// XXX These two steps are non-atomic.
	bool wake = spipe_cwake(s);
	__sync_add_and_fetch(&s->writesize, n);
	return wake;
}

size_t
spipe_writev(struct spipe *s, struct iovec v[2]) {
	if (s->first == NULL) {
		s->write = malloc(s->chunksize);
		s->first = malloc(s->chunksize);
		s->first->next = s->write;
		s->wbeg = s->wpos = s->rpos = s->first->bytes;
		s->wend = (char*)s->first + s->chunksize;
		s->rpos = s->wbeg;
		// If reader call spipe_guard(s) here, spipe_readv will return
		// 0 as expected.
		__sync_bool_compare_and_swap(&s->guard, GUARD_INVAL, s->rpos);
	} else if (s->guard == NULL) {
		// There is no reader, we can adjust pointers.
		assert(s->first->next == s->write);
		s->wbeg = s->wpos = s->rpos = s->first->bytes;
		s->wend = (char*)s->first + s->chunksize;
	}
	v[0].iov_base = s->wpos;
	v[0].iov_len = s->wend - s->wpos;
	v[1].iov_base = s->write->bytes;
	v[1].iov_len = s->totalsize;
	return v[0].iov_len + v[1].iov_len;
}

struct bpipe {
	struct spipe pipe;
	int sleep;
	int pair[2];
};

void
bpipe_init(struct bpipe *b, size_t nitem, size_t isize) {
	spipe_init(&b->pipe, nitem, isize);
	socketpair(AF_UNIX, SOCK_STREAM, 0, b->pair);
	b->sleep = 0;
}

struct bpipe *
bpipe_create(size_t nitem, size_t isize) {
	struct bpipe *b = malloc(sizeof(*b));
	bpipe_init(b, nitem, isize);
	return b;
}

void
bpipe_delete(struct bpipe *b) {
	close(b->pair[0]);
	close(b->pair[1]);
	spipe_fini(&b->pipe);
}

void
bpipe_readn(struct bpipe *b, size_t n) {
	spipe_readn(&b->pipe, n);
}

int
bpipe_getfd(struct bpipe *b) {
	return b->pair[0];
}

#define DUMMY_VALUE	0x33
size_t
bpipe_readv(struct bpipe *b, struct iovec v[2]) {
	size_t n;
	if (b->sleep || ((n = spipe_readv(&b->pipe, v) == 0))) {
		char dummy;
		ssize_t nbyte;
tryagain:
		nbyte = recv(b->pair[0], &dummy, sizeof(dummy), 0);
		if (nbyte == -1) {
			switch (errno) {
			case EWOULDBLOCK:
				b->sleep = 1;
				return 0;
			case EINTR:
				goto tryagain;
			}
		}
		b->sleep = 0;
		assert(nbyte == sizeof(dummy));
		assert(dummy == DUMMY_VALUE);
		n = spipe_readv(&b->pipe, v);
	}
	assert(n != 0);
	return n;
}

void
bpipe_writen(struct bpipe *b, size_t n) {
	if (spipe_writen(&b->pipe, n)) {
		char dummy = DUMMY_VALUE;
		ssize_t nbyte;
tryagain:
		nbyte = send(b->pair[1], &dummy, sizeof(dummy), 0);
		if (nbyte == -1 && errno == EINTR) {
			goto tryagain;
		}
		assert(nbyte == sizeof(dummy));
	}
}

size_t
bpipe_writev(struct bpipe *b, struct iovec v[2]) {
	return spipe_writev(&b->pipe, v);
}
//
//struct sring {
//	char *wpos;
//	char *wend;
//	char *rpos;
//	char *rend;
//
//
//	bool lock;
//	char *wpos;
//	char *wend;
//	char *rpos;
//	size_t wlen;
//	size_t rlen;
//	size_t bsize;
//	char *guard;
//	char bytes[];
//};
//
//static void
//sring_readn(struct sring *s, size_t n) {
//}
//
//static bool
//sring_writen(struct sring *s, size_t n) {
//	size_t wlen = s->wend - s->wpos;
//	if (n < wlen) {
//		s->wpos += n;
//	}
//	__sync_acquire_lock(&s->lock);
//	__sync_release_lock(&s->lock);
//	if (s->wpos >= rpos) {
//	}
//}
//
//static void
//sring_writev(struct sring *s, struct iovec v[2]) {
//	__sync_acquire_lock(&s->lock);
//	char *rpos = s->rpos;
//	char *rend = s->rend;
//	__sync_release_lock(&s->lock);
//	if (s->wpos >= rpos) {
//		s->wend = s->bytes + s->bsize;
//		v[1].iov_base = s->bytes;
//		v[1].iov_len = rpos - s->bytes;
//	} else {
//		s->wend = s->rpos;
//		v[1] = {NULL, NULL};
//	}
//	v[0].iov_base = s->wpos;
//	v[0].iov_len = s->wend - s->wpos;
//	return (v[0].iov_base + v[1].iov_base);
//}
