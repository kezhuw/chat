#ifndef __PIPE_H__
#define __PIPE_H__

#include "define.h"

#include <stddef.h>
#include <stdbool.h>
#include <sys/uio.h>

struct spipe;

// Memory for store a spipe struct.
size_t spipe_size(void);

struct spipe *spipe_create(size_t ntiem, size_t isize);
void spipe_delete(struct spipe *);

void spipe_init(struct spipe *p, size_t nitem, size_t isize);
void spipe_fini(struct spipe *p);

// Fetch v[2] for reading.
//
// 0 means there is no data available, and we need go to sleep.
// otherwise, retval is the sum of v[0].iov_len + v[1].iov_len.
//
// Return value is not the total bytes available in pipe.
//
// If you try anther spipe_readv after previous one return 0,
// without writer writes some data, the behavior is undefined.
size_t spipe_readv(struct spipe *p, struct iovec v[2]);

// Called after readv() to indicate that n bytes of data has been read.
// n must less than or equal to previous retval from readv().
void spipe_readn(struct spipe *p, size_t n);

// Client can call this function to know whether there is data in pipe.
//
// 0, writer will know that there is no data in pipe.
// size, reader will know that there is at least size bytes data in pipe.
size_t spipe_space(struct spipe *p);

// Fetch v[2] for writing.
//
// Return number of bytes located in v. The value returned equals to
// v[0].iov_len + v[1].iov_len.
size_t spipe_writev(struct spipe *p, struct iovec v[2]);

// Tell the pipe that we have write n bytes in iovec. n must less than or
// equal to retval from writev().
bool spipe_writen(struct spipe *p, size_t n);

// Write len bytes of buf to pipe. Return whether need to wake up reader.
bool spipe_writeb(struct spipe *dst, const char *buf, size_t len);

// Similar to spipe, expect bpipe provides a utility that writer will
// wake up reader when the reader is in sleeping.
struct bpipe;

// Memory for store a bpipe struct.
size_t bpipe_size(void);

struct bpipe *bpipe_create(size_t nitem, size_t isize);
void bpipe_delete(struct bpipe *p);

void bpipe_init(struct bpipe *p, size_t nitem, size_t isize);
void bpipe_fini(struct bpipe *p);

// Get reader fd, you can set it to nonblocking mode.
int bpipe_getfd(struct bpipe *p);

// Return zero only when there is no available data and reader fd is set to
// non-blocking by client.
size_t bpipe_readv(struct bpipe *p, struct iovec v[2]);

void bpipe_readn(struct bpipe *p, size_t n);

// Get space for writing.
size_t bpipe_writev(struct bpipe *p, struct iovec v[2]);

// When reader is in sleeping, send a message to wake it up.
void bpipe_writen(struct bpipe *p, size_t n);

#endif
