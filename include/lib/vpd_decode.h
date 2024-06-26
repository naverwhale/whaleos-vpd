/*
 * Copyright 2019 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __VPD_DECODE_H
#define __VPD_DECODE_H

/**
 * Enable kernel types for user land utility.
 * Remove these for kernel or firmware targets.
 */
#include <linux/types.h>
typedef __u8 u8;
typedef __u32 u32;
typedef __u64 u64;
typedef __s8 s8;
typedef __s32 s32;
typedef __s64 s64;

enum {
	VPD_DECODE_OK = 0,
	VPD_DECODE_FAIL = 1,
};

enum {
	VPD_TYPE_TERMINATOR = 0,
	VPD_TYPE_STRING,
	VPD_TYPE_INFO = 0xfe,
	VPD_TYPE_IMPLICIT_TERMINATOR = 0xff,
};

/* Callback for vpd_decode_string to invoke. */
typedef int vpd_decode_callback(
		const u8 *key, u32 key_len, const u8 *value, u32 value_len,
		void *arg);

/*
 * vpd_decode_string
 *
 * Given the encoded string, this function invokes callback with extracted
 * (key, value). The *consumed will be incremented by the number of bytes
 * consumed in this function.
 *
 * The input_buf points to the first byte of the input buffer.
 *
 * The *consumed starts from 0, which is actually the next byte to be decoded.
 * It can be non-zero to be used in multiple calls.
 *
 * If one entry is successfully decoded, sends it to callback and returns the
 * result.
 */
int vpd_decode_string(
		const u32 max_len, const u8 *input_buf, u32 *consumed,
		vpd_decode_callback callback, void *callback_arg);

#endif  /* __VPD_DECODE_H */
