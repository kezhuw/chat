#include "util.h"
#include "pipe.h"

#include <sys/socket.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

struct data {
	struct data *next;
	char bytes[];
};

struct spipe {
	bool sleep;
	bool wlock;
	char *guard;
	char *wbeg;
	char *wpos;
	size_t wlen;
	size_t wseq;
	char *rpos;
	char *rend;
	volatile size_t rlen;
	size_t rseq;
	struct data * volatile frees;
	struct data *write;
	struct data *first;

	volatile size_t writesize;

#define FINI_SIZE	(offsetof(struct spipe, totalsize))
	size_t totalsize;
	size_t blocksize;
	size_t chunksize;
};

void
spipe_init(struct spipe *s, size_t nitem, size_t isize) {
	memzero(s, sizeof(*s));
	s->totalsize = nitem*isize;
	s->blocksize = isize;
	s->chunksize = sizeof(struct data) + s->totalsize;
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
spipe_cwait(struct spipe *s) {
//	if (s->rpos != s->rend) {
//		return false;
//	}
	s->rend = __sync_val_compare_and_swap(&s->guard, s->rpos, NULL);
	if (s->rpos == s->rend) {
		return true;
	}
	return false;
}

static bool
spipe_cwake(struct spipe *s) {
	// Ensure we had written some data.
	assert(s->wbeg != s->wpos);
	if (!__sync_bool_compare_and_swap(&s->guard, s->wbeg, s->wpos)) {
		s->guard = s->wbeg = s->wpos;
		s->sleep = false;
		return true;
	}
	s->wbeg = s->wpos;
	return false;
}

// Caller need to ensure than n is smaller than value returned from readv().
// Called after readv, so if (s->rseq == s->wseq), n must lte s->rlen.
void
spipe_readn(struct spipe *s, size_t n) {
	assert(n%s->blocksize == 0);
	__sync_sub_and_fetch(&s->writesize, n);
	if (n < s->rlen) {
		s->rpos += n;
		__sync_sub_and_fetch(&s->rlen, n);
		return;
	}
	if (s->rseq == s->wseq) {
		assert(n <= s->rlen);
		s->rpos += n;
		__sync_sub_and_fetch(&s->rlen, n);
		return;
	}
	// __sync_acquire_lock(&s->wlock);
	// size_t wseq = s->wseq;
	// size_t wlen = s->wlen;
	// __sync_release_lock(&s->wlock);
	// ??? rlen
	n -= s->rlen;
	struct data *d0 = s->first;
	s->first = d0->next;
	if (n < s->totalsize) {
		s->rpos = s->first->bytes + n;
		s->rlen = s->totalsize - n;
	} else {
		assert(n == s->totalsize);
		struct data *d1 = s->first;
		s->first = d1->next;
		s->rpos = s->first->bytes;
		s->rlen = s->totalsize;
		spipe_free_data(s, d1);
	}
	spipe_free_data(s, d0);
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


size_t
spipe_readv(struct spipe *s, struct iovec v[2]) {
	if (s->sleep) {
		return 0;
	}
	if (spipe_cwait(s)) {
		s->sleep = true;
		return 0;
	}
	__sync_acquire_lock(&s->wlock);
	size_t wseq = s->wseq;
	struct data *d = s->first->next;
	__sync_release_lock(&s->wlock);
	v[0].iov_base = s->rpos;
	v[0].iov_len = s->rlen;
	if (s->rseq == wseq) {
		v[1].iov_base = NULL;
		v[1].iov_len = 0;
	} else if (s->rseq+1 == wseq) {
		v[1].iov_base = d->bytes;
		v[1].iov_len = s->totalsize - s->wlen;
	} else {
		// Can't use gt/lt, because wseq/rseq may overflow.
		v[1].iov_base = d->bytes;
		v[1].iov_len = s->totalsize;
	}
	return v[0].iov_len + v[1].iov_len;
}

bool
spipe_writen(struct spipe *s, size_t n) {
	assert(n != 0);
	assert(n <= (s->wlen+s->totalsize));
	__sync_add_and_fetch(&s->writesize, n);
	if (n < s->wlen) {
		s->wpos += n;
		s->wlen -= n;
		// Need not lock. rseq would not change, when (wseq == rseq).
		if (s->wseq == s->rseq) {
			__sync_add_and_fetch(&s->rlen, n);
		}
		return spipe_cwake(s);
	}
	if (s->wseq == s->rseq) {
		__sync_add_and_fetch(&s->rlen, s->wlen);
	}
	if (n == (s->wlen+s->totalsize)) {
		struct data *d0 = spipe_malloc_data(s);
		struct data *d1 = spipe_malloc_data(s);
		d0->next = d1;
		s->write->next = d0;
		__sync_acquire_lock(&s->wlock);
		s->wseq += 2;
		s->wpos = d0->bytes;
		s->wlen = s->totalsize;
		__sync_release_lock(&s->wlock);
		s->write = d1;
		return spipe_cwake(s);
	}
	n -= s->wlen;
	__sync_acquire_lock(&s->wlock);
	s->wseq++;
	s->wpos = s->write->bytes + n;
	s->wlen = s->totalsize - n;
	__sync_release_lock(&s->wlock);
	struct data *d = malloc(sizeof(*d)+s->totalsize);
	s->write->next = d;
	s->write = d;
	return spipe_cwake(s);
}

size_t
spipe_writev(struct spipe *s, struct iovec v[2]) {
	if (s->first == NULL) {
		s->first = malloc(s->chunksize);
		s->write = malloc(s->chunksize);
		s->first->next = s->write;
		s->rend = s->wbeg = s->wpos = s->rpos = s->first->bytes;
		s->wlen = s->totalsize;
	}
	if (s->guard == NULL) {
		// No available data, no reader.
		//assert(s->rpos == s->rend);
		//assert(s->rpos == s->wpos);
		//assert(s->wbeg == s->wpos);
		// Adjust pointers.
		// TODO
	}
	v[0].iov_base = s->wpos;
	v[0].iov_len = s->wlen;
	v[1].iov_base = s->write;
	v[1].iov_len = s->totalsize;
	size_t size = s->wlen + s->totalsize;
	assert(size%s->blocksize == 0);
	return size;
}

struct bpipe {
	struct spipe pipe;
	int pair[2];
};

void
bpipe_init(struct bpipe *b, size_t nitem, size_t isize) {
	spipe_init(&b->pipe, nitem, isize);
	socketpair(AF_UNIX, SOCK_STREAM, 0, b->pair);
}

struct bpipe *
bpipe_create(size_t nitem, size_t isize) {
	struct bpipe *b = malloc(sizeof(*b));
	bpipe_init(b, nitem, isize);
	return b;
}

void
bpipe_readn(struct bpipe *b, size_t n) {
	spipe_readn(&b->pipe, n);
}

int
bpipe_getfd(struct bpipe *b) {
	return b->pair[0];
}

size_t
bpipe_readv(struct bpipe *b, struct iovec v[2]) {
	size_t n;
tryagain:
	n = spipe_readv(&b->pipe, v);
	if (n == 0) {
		char dummy;
		ssize_t nbyte = recv(b->pair[0], &dummy, sizeof(dummy), 0);
		if (nbyte == -1) {
			int error = errno;
			if (error == EWOULDBLOCK) {
				return 0;
			}
			if (error == EINTR) {
				goto tryagain;
			}
		}
		assert(nbyte == sizeof(dummy));
		assert(dummy == 0x33);
		n = spipe_readv(&b->pipe, v);
	}
	assert(n != 0);
	return n;
}

void
bpipe_writen(struct bpipe *b, size_t n) {
	if (spipe_writen(&b->pipe, n)) {
		char dummy = 0x33;
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
