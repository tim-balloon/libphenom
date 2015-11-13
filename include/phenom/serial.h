/*
 * Copyright 2015-present Seth N. Hillbrand
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


#ifndef PHENOM_SERIAL_H_
#define PHENOM_SERIAL_H_

#include "phenom/defs.h"
#include "phenom/job.h"

#include <sys/types.h>
#include <limits.h>
#include <termios.h>

#include "phenom/string.h"
#include "phenom/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif


struct ph_serial;
typedef struct ph_serial ph_serial_t;

/** Indicates the results of an async sock connect
 *
 * If successful, `sock` will be non-NULL and `errcode` will be set
 * to `PH_SERIAL_CONNECT_SUCCESS`.
 *
 * `addr` may be NULL.  If it is not NULL, it holds the address that we attempted
 * to connect to.  It may be set if we didn't successfully connect.
 *
 * `elapsed` holds the amount of time that has elapsed since we attempted to initiate
 * the connection.
 *
 * `arg` passes through the arg parameter from ph_sock_resolve_and_connect().
 */
typedef void (*ph_serial_open_func)(
    ph_serial_t *serial, int errcode, void *arg);

/** Set or disable non-blocking mode for a file descriptor */
void ph_serial_set_nonblock(int fd, bool enable);

#define PH_SERIAL_CLOEXEC  1
#define PH_SERIAL_NONBLOCK 2


/** Open a serial port
 *
 * The results will be delivered to your connect func.
 * In some immediate failure cases, this will be called before
 * ph_serial_open has returned, but in the common case, this
 * will happen asynchronously from an NBIO thread.
 *
 */
ph_serial_t *ph_serial_open(const char *m_name,
        const struct termios *m_termios, void *arg);

/** The socket object callback function */
typedef void (*ph_serial_func)(ph_serial_t *serial, ph_iomask_t why,
        void *data);

/** Serial Object
 *
 * A serial object is a higher level representation of an underlying
 * file descriptor.
 *
 * A serial object is either enabled or disabled; when enabled, the
 * underlying descriptor is managed by the NBIO pool and any pending
 * write data will be sent as and when it is ready to go.  Any pending
 * reads will trigger a wakeup and you can use the serial functions to
 * read chunks or delimited records (such as lines).
 *
 * If your client/server needs to perform some blocking work, you may
 * simply disable the serial port until that work is complete.
 */
struct ph_serial {
  // Embedded job so we can participate in NBIO
  ph_job_t job;

  // Buffers for output, input
  ph_bufq_t *wbuf, *rbuf;

  // The per IO operation timeout duration
  struct timeval timeout_duration;

  // A stream for writing to the underlying connection
  ph_stream_t *conn;
  // A stream representation of myself.  Writing bytes into the
  // stream causes the data to be buffered in wbuf
  ph_stream_t *stream;

  // Dispatcher
  ph_serial_func callback;
  bool enabled;

  // The defines needed for this serial object
  char              path[PATH_MAX];
  struct termios    term;
  void              *priv;
};


/** Enable or disable IO dispatching for a serial object
 *
 * While enabled, the serial object will trigger callbacks when it is
 * readable/writable or timed out.  While disabled, none of these conditions
 * will trigger.
 */
void ph_serial_enable(ph_serial_t *serial, bool enable);

/** Wakeup the serial object
 *
 * Queues an PH_IOMASK_WAKEUP to the sock.  This is primarily useful in cases
 * where some asynchronous processing has completed and you wish to ping the
 * serial job so that it can consume the results.
 *
 * Delegates to ph_job_wakeup() and can fail for the same reasons as that
 * function.
 */
ph_result_t ph_serial_wakeup(ph_serial_t *serial);

/** Release all resources associated with a serial object
 *
 * Implicitly disables the serial object.
 */
void ph_serial_free(ph_serial_t *serial);

/**
 * Peeks at the exact number of bytes requested.  The number of bytes
 * will be returned to the calling function without removing them from
 * the buffer.
 *
 * @param m_serial  Pointer to the serial structure
 * @param len Length of data requested in bytes
 * @return Buffer containing the requested bytes or NULL if the number is
 *      not available.
 */
ph_buf_t *ph_serial_peek_bytes_exact(ph_serial_t *m_serial, uint64_t len);


/** Read exactly the specified number of bytes
 *
 * Returns a buffer containing the requested number of bytes, or NULL if they
 * are not available.
 *
 * Never returns a partial read.
 */
ph_buf_t *ph_serial_read_bytes_exact(ph_serial_t *serial, uint64_t len);


/** Read a delimited record
 *
 * Search for the delimiter in the buffer; if found, returns a buffer containing
 * the record and its delimiter text.
 */
ph_buf_t *ph_serial_read_record(ph_serial_t *serial, const char *delim,
    uint32_t delim_len);

/** Read a CRLF delimited line
 *
 * Search for the canonical CRLF in the buffer.  If found, returns a buffer
 * containing the line and its CRLF delimiter.
 */
ph_buf_t *ph_serial_read_line(ph_serial_t *serial);

int ph_serial_set_baud_base(ph_serial_t *m_serial, int m_base);
int ph_serial_set_baud_divisor(ph_serial_t *m_serial, int m_speed);
int ph_serial_setspeed(ph_serial_t *m_serial, speed_t m_speed);

// Succeeded
#define PH_SERIAL_CONNECT_SUCCESS  0
// Failed; errcode per strerror
#define PH_SERIAL_CONNECT_ERRNO    1



#ifdef __cplusplus
}
#endif

#endif /* PHENOM_SERIAL_H_ */

/* vim:ts=2:sw=2:et:
 */
