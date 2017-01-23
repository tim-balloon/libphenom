/*
 * Copyright 2013-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "phenom/stream.h"
#include "phenom/openssl.h"
#include <openssl/bio.h>

/* Implements an OpenSSL BIO that writes to a phenom bufq */

static int bio_bufq_write(BIO *h, const char *buf, int size)
{
  uint64_t n;
  ph_bufq_t *q = BIO_get_data(h);

  BIO_clear_retry_flags(h);
  if (ph_bufq_append(q, buf, size, &n) != PH_OK) {
    BIO_set_retry_write(h);
    errno = EAGAIN;
    return -1;
  }

  return (int)n;
}

static int bio_bufq_puts(BIO *h, const char *str)
{
  return bio_bufq_write(h, str, strlen(str));
}

static int bio_bufq_read(BIO *h, char *buf, int size)
{
  ph_unused_parameter(h);
  ph_unused_parameter(buf);
  ph_unused_parameter(size);
  errno = ENOSYS;
  return -1;
}

static long bio_bufq_ctrl(BIO *h, int cmd, // NOLINT(runtime/int)
    long arg1, void *arg2)                 // NOLINT(runtime/int)
{
  ph_unused_parameter(h);
  ph_unused_parameter(cmd);
  ph_unused_parameter(arg1);
  ph_unused_parameter(arg2);
  return 1;
}

static int bio_bufq_new(BIO *h)
{
  BIO_set_init(h, 0);
  BIO_set_data(h, NULL);
  BIO_set_flags(h, 0);
  return 1;
}

static int bio_bufq_free(BIO *h)
{
  if (!h) {
    return 0;
  }

  BIO_set_init(h, 0);
  BIO_set_data(h, NULL);
  BIO_set_flags(h, 0);

  return 1;
}

#if OPENSSL_VERSION_NUMBER < 0x10100001L
static BIO_METHOD method_bufq = {
  // See bio_stream.c
  81 | BIO_TYPE_SOURCE_SINK,
  "phenom-bufq",
  bio_bufq_write,
  bio_bufq_read,
  bio_bufq_puts,
  NULL, /* no gets */
  bio_bufq_ctrl,
  bio_bufq_new,
  bio_bufq_free,
  NULL, /* no callback ctrl */
};
#else
static BIO_METHOD *method_bufq;
static int bio_method_init(void) {
  if (!method_bufq) {
    return 0;
  }
  method_bufq = BIO_meth_new(80 /* 'P' */
			     | BIO_TYPE_SOURCE_SINK, "phenom-stream");
  if (method_bufq == 0
      || !BIO_meth_set_write(method_bufq, bio_bufq_write)
      || !BIO_meth_set_read(method_bufq, bio_bufq_read)
      || !BIO_meth_set_puts(method_bufq, bio_bufq_puts)
      || !BIO_meth_set_ctrl(method_bufq, bio_bufq_ctrl)
      || !BIO_meth_set_create(method_bufq, bio_bufq_new)
      || !BIO_meth_set_destroy(method_bufq, bio_bufq_free)) {
      BIO_meth_free(method_bufq);
      return 0;
    }
  return 1;
}
#endif

BIO *ph_openssl_bio_wrap_bufq(ph_bufq_t *bufq)
{
  BIO *h;

#if OPENSSL_VERSION_NUMBER < 0x10100001L
  h = BIO_new(&method_bufq);
#else
  bio_method_init();
  h = BIO_new(method_bufq);
#endif
  if (!h) {
    return NULL;
  }

  BIO_set_data(h, bufq);
  BIO_set_init(h, 1);
  return h;
}


/* vim:ts=2:sw=2:et:
 */

