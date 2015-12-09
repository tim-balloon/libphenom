/*
 * Copyright 2015-present Seth Hillbrand
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

#include "phenom/serial.h"
#include "phenom/sysutil.h"
#include "phenom/memory.h"
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/dns.h"
#include "phenom/printf.h"
#include "phenom/configuration.h"

#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <unistd.h>

#define MAX_SERIAL_BUFFER_SIZE 128*1024

static ph_memtype_def_t defs[] = {
  { "serial", "serial", sizeof(ph_serial_t), PH_MEM_FLAGS_ZERO },
};
static struct {
  ph_memtype_t serial;
} mt;

static void serial_job_cleanup(ph_job_t *job)
{
  ph_serial_t *m_serial = (ph_serial_t*)job;


  if (m_serial->wbuf) {
    ph_bufq_free(m_serial->wbuf);
    m_serial->wbuf = NULL;
  }
  if (m_serial->rbuf) {
    ph_bufq_free(m_serial->rbuf);
    m_serial->rbuf = NULL;
  }
  if (m_serial->conn) {
    ph_stm_close(m_serial->conn);
    m_serial->conn = NULL;
  }
  if (m_serial->stream) {
    ph_stm_close(m_serial->stream);
    m_serial->stream = NULL;
  }
}

static bool try_write(ph_serial_t *m_serial)
{
  while (ph_bufq_len(m_serial->wbuf)) {
    if (!ph_bufq_stm_write(m_serial->wbuf, m_serial->conn, NULL)) {
      if (ph_stm_errno(m_serial->conn) != EAGAIN) {
        return false;
      }
      // No room right now
      return true;
    }
  }
  return true;
}

static bool try_read(ph_serial_t *m_serial)
{
  ph_stream_t *stm = m_serial->conn;

  if (!ph_bufq_stm_read(m_serial->rbuf, stm, NULL) &&
      ph_stm_errno(stm) != EAGAIN) {
    return false;
  }
  return true;
}

static void serial_dispatch(ph_job_t *j, ph_iomask_t why, void *data)
{
  ph_serial_t *serial = (ph_serial_t*)j;
  bool had_err = why & PH_IOMASK_ERR;

  if (j->def && j->epoch_entry.function) {
    return;
  }

  if (serial->enabled) {
    serial->conn->need_mask = 0;

    // If we have data pending write, try to get that sent, and flag
    // errors
    if (!try_write(serial)) {
      why |= PH_IOMASK_ERR;
    }

    if ((why & PH_IOMASK_ERR) == 0) {
      if (!try_read(serial)) {
        why |= PH_IOMASK_ERR;
      }
    }
  }

dispatch_again:
  if (why & PH_IOMASK_ERR) {
    had_err = true;
  }
  if (serial->enabled || (why & PH_IOMASK_WAKEUP) || (why & PH_IOMASK_ERR)) {
    serial->callback(serial, why, data);
  }

  if (!serial->enabled) {
    return;
  }

  if (!had_err) {
    if (!try_write(serial)) {
      why = PH_IOMASK_ERR;
      goto dispatch_again;
    }
  }

  ph_iomask_t mask = serial->conn->need_mask | PH_IOMASK_READ;

  if (ph_bufq_len(serial->wbuf)) {
    mask |= PH_IOMASK_WRITE;
  }

  ph_log(PH_LOG_DEBUG, "fd=%d setting mask=%x timeout={%d,%d}",
      serial->job.fd, mask, (int)serial->timeout_duration.tv_sec,
      (int)serial->timeout_duration.tv_usec);
  ph_job_set_nbio_timeout_in(&serial->job, mask, serial->timeout_duration);
}

static struct ph_job_def serial_job_template = {
  serial_dispatch,
  PH_MEMTYPE_INVALID,
  serial_job_cleanup
};

static void do_serial_init(void)
{
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs,
      &mt.serial);
  serial_job_template.memtype = mt.serial;
}
PH_LIBRARY_INIT(do_serial_init, 0)


static bool serial_stm_close(ph_stream_t *stm)
{
  ph_unused_parameter(stm);
  return true;
}

static bool serial_stm_readv(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nread)
{
  ph_serial_t *m_serial = stm->cookie;
  int i;
  uint64_t tot = 0;
  uint64_t avail;
  ph_buf_t *b;
  bool res = true;

  for (i = 0; i < iovcnt; i++) {
    avail = MIN(ph_bufq_len(m_serial->rbuf), iov[i].iov_len);
    if (avail == 0) {
      continue;
    }

    // This allocates a buf slice; in theory, we can eliminate this
    // allocation, but in practice it's probably fine until profiling
    // tells us otherwise
    b = ph_bufq_consume_bytes(m_serial->rbuf, avail);
    if (!b) {
      stm->last_err = ENOMEM;
      res = false;
      break;
    }
    memcpy(iov[i].iov_base, ph_buf_mem(b), avail);
    ph_buf_delref(b);
    tot += avail;
  }

  if (nread) {
    *nread = tot;
  }

  if (tot > 0) {
    return true;
  }

  return res;
}

static bool serial_stm_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrotep)
{
  int i;
  uint64_t n, total = 0;
  ph_serial_t *m_serial = stm->cookie;
  ph_bufq_t *bufq = m_serial->wbuf;

  for (i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) {
      continue;
    }
    if (ph_bufq_append(bufq, iov[i].iov_base,
          iov[i].iov_len, &n) != PH_OK) {
      stm->last_err = EAGAIN;
      break;
    }
    total += n;
  }

  if (total) {
    if (nwrotep) {
      *nwrotep = total;
    }
    return true;
  }
  return false;
}

static bool serial_stm_seek(ph_stream_t *stm, int64_t delta, int whence,
    uint64_t *newpos)
{
  ph_unused_parameter(delta);
  ph_unused_parameter(whence);
  ph_unused_parameter(newpos);
  stm->last_err = ESPIPE;
  return false;
}

static struct ph_stream_funcs serial_stm_funcs = {
  serial_stm_close,
  serial_stm_readv,
  serial_stm_writev,
  serial_stm_seek
};

ph_serial_t *ph_serial_open(const char *m_name,
        const struct termios *m_termios, void *arg)
{
  int fd;
  ph_serial_t *serial;
  int64_t max_buf;

  fd = open(m_name, O_NOCTTY | O_RDWR | O_NONBLOCK);

  if (fd < 0) {
      return NULL;
  }

  serial = (ph_serial_t*)ph_job_alloc(&serial_job_template);
  if (!serial) {
    return NULL;
  }

  if (m_termios)
      memcpy(&serial->term, m_termios, sizeof(struct termios));
  else
      cfmakeraw(&serial->term);

  max_buf = ph_config_query_int("$.serial.max_buffer_size",
          MAX_SERIAL_BUFFER_SIZE);

  serial->wbuf = ph_bufq_new(max_buf);
  if (!serial->wbuf) {
    goto fail;
  }

  serial->rbuf = ph_bufq_new(max_buf);
  if (!serial->rbuf) {
    goto fail;
  }

  serial->conn = ph_stm_fd_open(fd, 0, 0);
  if (!serial->conn) {
    goto fail;
  }

  serial->stream = ph_stm_make(&serial_stm_funcs, serial, 0, 0);
  if (!serial->stream) {
    goto fail;
  }

  strncpy(serial->path, m_name, sizeof(serial->path));
  serial->job.fd = fd;
  serial->timeout_duration.tv_sec = 60;
  serial->job.data = arg;

  tcsetattr(serial->job.fd, TCSANOW, &serial->term);

  return serial;

fail:
  ph_job_free(&serial->job);
  return NULL;
}


void ph_serial_free(ph_serial_t *m_serial)
{
  m_serial->enabled = false;
  ph_job_free(&m_serial->job);
}

ph_result_t ph_serial_wakeup(ph_serial_t *m_serial)
{
  return ph_job_wakeup(&m_serial->job);
}

void ph_serial_enable(ph_serial_t *m_serial, bool enable)
{
  if (m_serial->enabled == enable) {
    return;
  }

  m_serial->enabled = enable;
  if (enable) {
    // Enable for read AND write here, in case we're in a different context.
    // This may result in a spurious wakeup at the point we enable the sock,
    // but the sock_func will set this according to the buffer status once
    // we get there
    ph_job_set_nbio_timeout_in(&m_serial->job,
        PH_IOMASK_READ|PH_IOMASK_WRITE,
        m_serial->timeout_duration);
  } else {
    ph_job_set_nbio(&m_serial->job, 0, NULL);
  }
}

int ph_serial_set_baud_base(ph_serial_t *m_serial, int m_base) {
    struct serial_struct ss;
    if (ioctl(m_serial->job.fd, TIOCGSERIAL, &ss) != 0) {
        m_serial->stream->last_err = errno;
        return -1;
    }

    ss.baud_base = m_base;

    if (ioctl(m_serial->job.fd, TIOCSSERIAL, &ss)) {
        ph_log(PH_LOG_DEBUG, "fd=%d setting baud base=%d error=%s",
                m_serial->job.fd, m_base, strerror(errno));

        m_serial->stream->last_err = errno;
        return -1;
    }
    return 0;
}

int ph_serial_set_baud_divisor(ph_serial_t *m_serial, int m_speed) {
    // default baud was not found, so try to set a custom divisor
    struct serial_struct ss;
    if (ioctl(m_serial->job.fd, TIOCGSERIAL, &ss) != 0) {
        m_serial->conn->last_err = errno;
        return -1;
    }

    ss.flags = (ss.flags & ~ASYNC_SPD_MASK) | ASYNC_SPD_CUST;
    ss.custom_divisor = (ss.baud_base + (m_speed / 2)) / m_speed;

    // Set the High-speed bit
    if (m_speed > 115200) ss.custom_divisor |= (1 << 15);
    /**
     * It is debatable whether this next line is required.  Generally, we
     * need frob config bits to get 921600 (despite the SMSC manual's
     * directions).  But it doesn't seem to hurt so we keep it here for
     * completeness.
     */
    // Set the Enhanced Freq low bit
    if (m_speed > 460800) ss.custom_divisor |= (1 << 14);

    if (ioctl(m_serial->job.fd, TIOCSSERIAL, &ss)) {
        ph_log(PH_LOG_DEBUG, "fd=%d setting speed=%d error=%s",
            m_serial->job.fd, m_speed, strerror(errno));
        m_serial->stream->last_err = errno;
        return -1;
    }
    return 0;
}

int ph_serial_setspeed(ph_serial_t *m_serial, speed_t m_speed)
{
    if (cfsetospeed(&m_serial->term, m_speed) ||
            cfsetispeed(&m_serial->term, m_speed)) {
        return -1;
    }

    if (m_serial->job.fd != -1) {
        tcsetattr(m_serial->job.fd, TCSADRAIN, &m_serial->term);
    }
    return true;
}

ph_buf_t *ph_serial_read_bytes_exact(ph_serial_t *m_serial, uint64_t len)
{
  return ph_bufq_consume_bytes(m_serial->rbuf, len);
}

ph_buf_t *ph_serial_read_record(ph_serial_t *m_serial, const char *delim,
    uint32_t delim_len)
{
  return ph_bufq_consume_record(m_serial->rbuf, delim, delim_len);
}

ph_buf_t *ph_serial_read_line(ph_serial_t *m_serial)
{
  return ph_bufq_consume_record(m_serial->rbuf, "\r\n", 2);
}

ph_buf_t *ph_serial_peek_bytes_exact(ph_serial_t *m_serial, uint64_t len)
{
  return ph_bufq_peek_bytes(m_serial->rbuf, len);
}



/* vim:ts=2:sw=2:et:
 */

