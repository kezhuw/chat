#ifndef __DEFINE__H_
#define __DEFINE__H_

enum {
	MessageTypeHandshake,
	MessageTypeBroadmsg,
	MessageTypeUnitymsg,
};

// Occupy 2-bytes
enum message_type {
	MessageCstring,
	MessageSbinary,		// 2-bytes size
	MessageIbinary,		// 4-bytes size

	MessageTypeAuth,
	MessageTypeChat,
	MessageTypeMail,
};

#include <stddef.h>
#include <sys/uio.h>

#include <string.h>
static size_t
iovec_copyin(struct iovec v[2], const char *src, size_t len) {
	size_t cp0 = v[0].iov_len;
	if (cp0 >= len) {
		memcpy(v[0].iov_base, src, len);
		return len;
	} else {
		memcpy(v[0].iov_base, src, cp0);
		len -= cp0;
		size_t cp1 = len<=v[1].iov_len ? len : v[1].iov_len;
		memcpy(v[1].iov_base, src+cp0, cp1);
		return cp0+cp1;
	}
}

static size_t
iovec_copyout(struct iovec v[2], char *dst, size_t len) {
	size_t cp0 = v[0].iov_len;
	if (cp0 >= len) {
		memcpy(dst, v[0].iov_base, len);
		return len;
	} else {
		memcpy(dst, v[0].iov_base, cp0);
		len -= cp0;
		size_t cp1 = len<=v[1].iov_len ? len : v[1].iov_len;
		memcpy(dst+cp0, v[1].iov_base, cp1);
		return cp0+cp1;
	}
}

static void
copy1(char dst[1], char src[1]) {
	dst[0] = src[0];
}

static inline void
copy2(char dst[2], char src[2]) {
	dst[0] = src[0];
	dst[1] = src[1];
}

static inline void
copy3(char dst[3], char src[3]) {
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
}

static void
copy4(char dst[4], char src[4]) {
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
	dst[3] = src[3];
}

#include <stdint.h>
static unsigned
iovec_read_uint32(struct iovec v[2]) {
	union {
		char bytes[4];
		uint32_t integer;
	} u32;
	switch (v[0].iov_len) {
	case 0:
		copy4(u32.bytes, v[1].iov_base);
		v[0].iov_base = (char*)v[1].iov_base + 4;
		v[0].iov_len = v[1].iov_len - 4;
		break;
	case 1:
		copy1(u32.bytes, v[0].iov_base);
		copy3(u32.bytes+1, v[1].iov_base);
		v[0].iov_base = (char*)v[1].iov_base + 3;
		v[0].iov_len = v[1].iov_len - 3;
		break;
	case 2:
		copy2(u32.bytes, v[0].iov_base);
		copy2(u32.bytes+2, v[1].iov_base);
		v[0].iov_base = (char*)v[1].iov_base + 2;
		v[0].iov_len = v[1].iov_len - 2;
		break;
	case 3:
		copy3(u32.bytes, v[0].iov_base);
		copy1(u32.bytes+3, v[1].iov_base);
		v[0].iov_base = (char*)v[1].iov_base + 1;
		v[0].iov_len = v[1].iov_len - 1;
		break;
	default:
		copy4(u32.bytes, v[0].iov_base);
		v[0].iov_base = (char*)v[0].iov_base + 4;
		v[0].iov_len -= 4;
		break;
	}
	return u32.integer;
}

size_t iovec_copyout(struct iovec v[2], char *buf, size_t len);
#endif
