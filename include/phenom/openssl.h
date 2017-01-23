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


#ifndef PHENOM_OPENSSL_H
#define PHENOM_OPENSSL_H

#include "phenom/buffer.h"

// Avoid fatal compilation error due to an #if TARGET_OS_MAC line in
// a kerberos related include
#pragma GCC diagnostic ignored "-Wundef"
#ifdef __APPLE__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <openssl/err.h>
#include <openssl/engine.h>
#include <openssl/ssl.h>
#pragma GCC diagnostic error "-Wundef"

#ifdef __cplusplus
extern "C" {
#endif

/* OpenSSL 1.0.2 compatibility */
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define BIO_get_init(x)                  (x->init)
#define BIO_set_init(x, v)               (x->init = v)
#define BIO_set_flags(x, v)              (x->flags = v)
#define BIO_get_flags(x)                 (x->flags)
#define BIO_get_data(x)                  (x->ptr)
#define BIO_set_data(x, v)               (x->ptr = v)
#define BIO_set_shutdown(x, v)           (x->shutdown = v)
#endif /* OPENSSL_VERSION_NUMBER < 0x10100000L */

/** Initialize multi-threaded OpenSSL for the process
 *
 * If you are building an application that owns the process, as opposed
 * to a library that is loaded into an existing process, you will need
 * to correctly configure OpenSSL for multithreaded use.
 *
 * libPhenom provides this function as a convenience; you should only
 * call it if you don't already have code to configure the locking
 * callbacks required by OpenSSL.
 */
void ph_library_init_openssl(void);

/** Wrap a phenom stream in an OpenSSL BIO
 *
 * The BIO is intended to be used for SSL.  The BIO holds a weak
 * reference on the stream and will never close the underlying
 * stream.
 */
BIO *ph_openssl_bio_wrap_stream(ph_stream_t *stm);

/** Wrap an OpenSSL SSL object in a phenom stream
 *
 * The stream is unbuffered since this is intended to be used with the
 * ph_sock_t implementation, which implements buffering using `ph_bufq_t`.
 *
 * Closing the stream will cause SSL_free() to be invoked on the underlying
 * ssl object.
 */
ph_stream_t *ph_stm_ssl_open(SSL *ssl);

/** Wrap a phenom bufq in an OpenSSL BIO
 *
 * The BIO is intended to be used for SSL.  The BIO holds a weak
 * reference to the bufq and will never free the underlying bufq.
 */
BIO *ph_openssl_bio_wrap_bufq(ph_bufq_t *bufq);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

