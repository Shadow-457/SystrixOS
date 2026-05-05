/* ============================================================
 *  Systrix OS — kernel/pipe.c
 *
 *  Anonymous pipe — a one-directional, in-kernel byte stream.
 *
 *  A pipe connects a writer to a reader using a fixed-size
 *  circular buffer.  Bytes written at the tail are read from
 *  the head in FIFO order.
 *
 *  Buffer layout:
 *
 *    index: 0    head          tail        PIPE_BUF_SIZE-1
 *           |     |  <readable> |  <empty>  |
 *
 *  Both head and tail wrap around modulo PIPE_BUF_SIZE.
 *  `count` tracks how many bytes are currently in the buffer
 *  so we never confuse a full buffer from an empty one.
 *
 *  Blocking behaviour:
 *    This implementation is non-blocking.  If the buffer is
 *    empty on read (or full on write), EAGAIN is returned
 *    instead of sleeping.  The caller is expected to retry.
 *
 *  End-of-file semantics:
 *    When the write end is closed and the buffer is drained,
 *    pipe_read() returns 0 (EOF) instead of EAGAIN.
 * ============================================================ */

#include "../include/kernel.h"


/* ============================================================
 *  Constants
 * ============================================================ */

/* Capacity of the circular data buffer in bytes.
 * 4096 is the traditional POSIX minimum pipe buffer size. */
#define PIPE_BUF_SIZE  4096


/* ============================================================
 *  Pipe State
 * ============================================================ */

typedef struct {
    u8    buf[PIPE_BUF_SIZE]; /* the circular data buffer */
    usize head;               /* index of the next byte to read */
    usize tail;               /* index where the next byte will be written */
    usize count;              /* number of bytes currently in the buffer */
    int   read_open;          /* 1 while the read end is open */
    int   write_open;         /* 1 while the write end is open */
} Pipe;


/* ============================================================
 *  pipe_create
 *
 *  Allocates and initialises a new pipe.
 *  Both ends start open.
 *
 *  Returns a pointer to the pipe on success, NULL on allocation failure.
 * ============================================================ */
void *pipe_create(void)
{
    Pipe *pipe = (Pipe *)heap_malloc(sizeof(Pipe));

    if (pipe == NULL) {
        return NULL;
    }

    memset(pipe, 0, sizeof(Pipe));

    pipe->read_open  = 1;
    pipe->write_open = 1;

    return pipe;
}


/* ============================================================
 *  pipe_read
 *
 *  Reads up to `count` bytes from the pipe into `buf`.
 *
 *  Return values:
 *    > 0        — number of bytes actually read (may be less than `count`
 *                 if the buffer empties before the request is satisfied)
 *    0          — EOF: the write end has been closed and the buffer is empty
 *    EAGAIN     — no data available yet and the write end is still open
 *    EBADF      — the pipe pointer is NULL or the read end has been closed
 * ============================================================ */
i64 pipe_read(void *pipe_ptr, void *buf, usize count)
{
    Pipe *pipe = (Pipe *)pipe_ptr;

    if (pipe == NULL || !pipe->read_open) {
        return (i64)EBADF;
    }

    if (count == 0) {
        return 0;
    }

    u8    *out_bytes   = (u8 *)buf;
    usize  bytes_read  = 0;

    while (bytes_read < count) {
        if (pipe->count == 0) {
            /* Buffer is empty — decide whether this is EOF or just not-yet */
            if (!pipe->write_open) {
                /* Writer closed: return what we have, or 0 to signal EOF */
                return (i64)bytes_read;
            }
            /* Writer still open: return what we have, or EAGAIN if nothing yet */
            return bytes_read > 0 ? (i64)bytes_read : (i64)EAGAIN;
        }

        /* Copy one byte from the head of the circular buffer */
        out_bytes[bytes_read] = pipe->buf[pipe->head];
        pipe->head  = (pipe->head + 1) % PIPE_BUF_SIZE;
        pipe->count--;
        bytes_read++;
    }

    return (i64)bytes_read;
}


/* ============================================================
 *  pipe_write
 *
 *  Writes up to `count` bytes from `buf` into the pipe.
 *
 *  Return values:
 *    > 0        — number of bytes actually written (may be less than `count`
 *                 if the buffer fills before all bytes are consumed)
 *    EAGAIN     — the buffer was already full; nothing was written
 *    EBADF      — the pipe pointer is NULL or the write end has been closed
 * ============================================================ */
i64 pipe_write(void *pipe_ptr, const void *buf, usize count)
{
    Pipe *pipe = (Pipe *)pipe_ptr;

    if (pipe == NULL || !pipe->write_open) {
        return (i64)EBADF;
    }

    if (count == 0) {
        return 0;
    }

    const u8 *in_bytes    = (const u8 *)buf;
    usize     bytes_written = 0;

    while (bytes_written < count) {
        if (pipe->count == PIPE_BUF_SIZE) {
            /* Buffer is full — return what we managed to write, or EAGAIN */
            return bytes_written > 0 ? (i64)bytes_written : (i64)EAGAIN;
        }

        /* Append one byte at the tail of the circular buffer */
        pipe->buf[pipe->tail] = in_bytes[bytes_written];
        pipe->tail  = (pipe->tail + 1) % PIPE_BUF_SIZE;
        pipe->count++;
        bytes_written++;
    }

    return (i64)bytes_written;
}


/* ============================================================
 *  pipe_destroy
 *
 *  Frees the memory backing the pipe.
 *  The caller is responsible for closing both ends before
 *  destroying the pipe to avoid use-after-free bugs.
 * ============================================================ */
void pipe_destroy(void *pipe_ptr)
{
    if (pipe_ptr != NULL) {
        heap_free(pipe_ptr);
    }
}
