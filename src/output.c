/* Output to stdout / stderr for GNU Make
Copyright (C) 2013-2024 Free Software Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <https://www.gnu.org/licenses/>.  */

#include "makeint.h"
#include "os.h"
#include "output.h"

/* GNU Make no longer supports pre-ANSI89 environments.  */

#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#else
# include <sys/file.h>
#endif

#if MK_OS_W32
# include <windows.h>
# include <io.h>
# include <process.h>
# include "sub_proc.h"
#endif

struct output *output_context = NULL;
unsigned int stdio_traced = 0;

#define OUTPUT_NONE (-1)

/* ANSI color sequences for make's own messages.  */
#define COLOR_ERROR   "\33[91m"
#define COLOR_WARNING "\33[93m"
#define COLOR_RESET   "\33[0m"

#define OUTPUT_ISSET(_out) ((_out)->out >= 0 || (_out)->err >= 0)

/* Write a string to the current STDOUT or STDERR.  */
static void
_outputs (struct output *out, int is_err, const char *msg)
{
  FILE *f;

  if (out && out->syncout)
    {
      int fd = is_err ? out->err : out->out;
      if (fd != OUTPUT_NONE)
        {
          size_t len = strlen (msg);
          int r;
          EINTRLOOP (r, lseek (fd, 0, SEEK_END));
          writebuf (fd, msg, len);
          return;
        }
    }

  f = is_err ? stderr : stdout;
  fputs (msg, f);
  fflush (f);
}

/* Write a message indicating that we've just entered or
   left (according to ENTERING) the current directory.  */

static int
log_working_directory (int entering)
{
  static char *buf = NULL;
  static size_t len = 0;
  size_t need;
  const char *fmt;
  char *p;

  /* Get enough space for the longest possible output.  */
  need = strlen (program) + INTSTR_LENGTH + 2 + 1;
  if (starting_directory)
    need += strlen (starting_directory);

  /* Use entire sentences to give the translators a fighting chance.  */
  if (makelevel == 0)
    if (starting_directory == 0)
      if (entering)
        fmt = _("%s: Entering an unknown directory\n");
      else
        fmt = _("%s: Leaving an unknown directory\n");
    else
      if (entering)
        fmt = _("%s: Entering directory '%s'\n");
      else
        fmt = _("%s: Leaving directory '%s'\n");
  else
    if (starting_directory == 0)
      if (entering)
        fmt = _("%s[%u]: Entering an unknown directory\n");
      else
        fmt = _("%s[%u]: Leaving an unknown directory\n");
    else
      if (entering)
        fmt = _("%s[%u]: Entering directory '%s'\n");
      else
        fmt = _("%s[%u]: Leaving directory '%s'\n");

  need += strlen (fmt);

  if (need > len)
    {
      buf = xrealloc (buf, need);
      len = need;
    }

  p = buf;
  if (print_data_base_flag)
    {
      *(p++) = '#';
      *(p++) = ' ';
    }

  if (makelevel == 0)
    if (starting_directory == 0)
      sprintf (p, fmt , program);
    else
      sprintf (p, fmt, program, starting_directory);
  else if (starting_directory == 0)
    sprintf (p, fmt, program, makelevel);
  else
    sprintf (p, fmt, program, makelevel, starting_directory);

  _outputs (NULL, 0, buf);

  return 1;
}


/* Return a new buffer with the child output BUF[0..*LEN) in which " error "
   and " warning " are colored, and set *LEN to its length.  The trailing
   space of a match is not consumed, so "x error error y" colors both words.
   The worst case is a 6 byte step growing to 15 bytes, hence 3 * LEN.  */
static char *
colorize (const char *buf, size_t *len)
{
  static const char err_find[] = " error ";
  static const char err_repl[] = " \33[91merror\33[0m";
  static const char warn_find[] = " warning ";
  static const char warn_repl[] = " \33[95mwarning\33[0m";
  size_t in_len = *len;
  char *result = xmalloc (in_len * 3 + 1);
  char *p = result;
  size_t i = 0;

  while (i < in_len)
    {
      size_t rest = in_len - i;
      if (buf[i] == ' ' && rest >= sizeof (err_find) - 1
          && memcmp (buf + i, err_find, sizeof (err_find) - 1) == 0)
        {
          memcpy (p, err_repl, sizeof (err_repl) - 1);
          p += sizeof (err_repl) - 1;
          i += sizeof (err_find) - 2;
        }
      else if (buf[i] == ' ' && rest >= sizeof (warn_find) - 1
               && memcmp (buf + i, warn_find, sizeof (warn_find) - 1) == 0)
        {
          memcpy (p, warn_repl, sizeof (warn_repl) - 1);
          p += sizeof (warn_repl) - 1;
          i += sizeof (warn_find) - 2;
        }
      else
        *p++ = buf[i++];
    }

  *len = p - result;
  return result;
}

#if MK_OS_W32

/* Child output streamed through a pipe.  A reader thread colors each
   complete line and writes it to make's stdout or stderr at once, so the
   output shows while the command runs instead of after it has finished.  */
struct output_stream
  {
    HANDLE read;              /* Read end of the pipe.  */
    HANDLE target;            /* Make's stdout or stderr.  */
    HANDLE thread;
    volatile LONG stop;       /* Set once the child has exited.  */
  };

/* Writes from parallel reader threads must not mix within a line.  */
static CRITICAL_SECTION stream_lock;

/* After the child has exited, a background process it started can keep the
   pipe open.  Stop reading once it has been quiet this long.  */
#define STREAM_GRACE_MS   200

/* A partial line (a prompt, a progress dot) is written after this long
   without new data.  */
#define STREAM_PARTIAL_MS 100

#define STREAM_POLL_MS    10

static void
stream_write (struct output_stream *s, const char *buf, size_t len)
{
  char *colored = colorize (buf, &len);
  const char *p = colored;

  EnterCriticalSection (&stream_lock);
  while (len > 0)
    {
      DWORD written;
      if (!WriteFile (s->target, p, (DWORD) len, &written, NULL)
          || written == 0)
        break;
      p += written;
      len -= written;
    }
  LeaveCriticalSection (&stream_lock);

  free (colored);
}

/* Reading polls with PeekNamedPipe instead of blocking in ReadFile, so the
   thread can stop when a background process holds the pipe open.  */
static unsigned __stdcall
stream_reader (void *arg)
{
  struct output_stream *s = arg;
  char *buf = NULL;
  size_t size = 0;
  size_t used = 0;
  DWORD last_data = GetTickCount ();
  DWORD stop_seen = 0;
  int stopping = 0;

  while (1)
    {
      DWORD avail = 0;
      DWORD got = 0;
      DWORD now;
      size_t end;

      /* This fails with ERROR_BROKEN_PIPE once the pipe is drained and
         every writer has closed it.  */
      if (!PeekNamedPipe (s->read, NULL, 0, NULL, &avail, NULL))
        break;

      now = GetTickCount ();
      if (!stopping && s->stop)
        {
          stopping = 1;
          stop_seen = now;
        }

      if (avail == 0)
        {
          if (used > 0 && now - last_data >= STREAM_PARTIAL_MS)
            {
              stream_write (s, buf, used);
              used = 0;
            }
          if (stopping && now - stop_seen >= STREAM_GRACE_MS
              && now - last_data >= STREAM_GRACE_MS)
            break;
          Sleep (STREAM_POLL_MS);
          continue;
        }

      if (used + avail > size)
        {
          size = used + avail;
          buf = xrealloc (buf, size);
        }
      if (!ReadFile (s->read, buf + used, avail, &got, NULL))
        break;
      used += got;
      last_data = GetTickCount ();

      /* Write every complete line, keep the rest for the next read.  */
      end = used;
      while (end > 0 && buf[end - 1] != '\n')
        --end;
      if (end > 0)
        {
          stream_write (s, buf, end);
          memmove (buf, buf + end, used - end);
          used -= end;
        }
    }

  if (used > 0)
    stream_write (s, buf, used);
  free (buf);
  return 0;
}

/* Start streaming a child's stdout (IS_ERR == 0) or stderr.  On success
   *FD is the write end of the pipe to hand to the child; the caller closes
   it after starting the child.  Returns NULL if the pipe cannot be set up,
   the child then writes to make's own handle.  */
struct output_stream *
output_stream_open (int is_err, int *fd)
{
  static int lock_ready = 0;
  struct output_stream *s;
  HANDLE r, w;

  *fd = -1;

  if (!lock_ready)
    {
      InitializeCriticalSection (&stream_lock);
      lock_ready = 1;
    }

  /* Not inheritable: process_easy() makes an inheritable copy for this
     child only, so other children started in parallel do not hold the pipe
     open.  */
  if (!CreatePipe (&r, &w, NULL, 64 * 1024))
    return NULL;

  *fd = _open_osfhandle ((intptr_t) w, _O_BINARY);
  if (*fd < 0)
    {
      CloseHandle (r);
      CloseHandle (w);
      return NULL;
    }

  s = xcalloc (sizeof (struct output_stream));
  s->read = r;
  s->target = get_handle_for_fd (fileno (is_err ? stderr : stdout));
  s->thread = (HANDLE) _beginthreadex (NULL, 0, stream_reader, s, 0, NULL);
  if (!s->thread)
    {
      close (*fd);
      *fd = -1;
      CloseHandle (r);
      free (s);
      return NULL;
    }

  return s;
}

/* Wait until all output of the stream has been written, then free it.
   Call after the child has exited and before make reports on it.  */
void
output_stream_close (struct output_stream **ps)
{
  struct output_stream *s = *ps;

  if (!s)
    return;
  *ps = NULL;

  /* Make's own messages printed before this point must not end up after
     the child's output in the CRT buffers.  */
  fflush (stdout);
  fflush (stderr);

  InterlockedExchange (&s->stop, 1);
  WaitForSingleObject (s->thread, INFINITE);
  CloseHandle (s->thread);
  CloseHandle (s->read);
  free (s);
}

#endif /* MK_OS_W32 */


#ifndef NO_OUTPUT_SYNC

/* Support routine for output_sync() */
static void
pump_from_tmp (int from, FILE *to)
{
  long pos;
  size_t len = 0;
  char *buffer;
  char *colored;

#if MK_OS_W32
  int prev_mode;

  /* "from" is opened by open_tmpfd, which does it in binary mode, so
     we need the mode of "to" to match that.  */
  prev_mode = _setmode (fileno (to), _O_BINARY);
#endif

  pos = lseek (from, 0, SEEK_END);
  if (pos < 0 || lseek (from, 0, SEEK_SET) == -1)
    {
      perror ("lseek()");
      pos = 0;
    }

  buffer = xmalloc (pos + 1);
  while (len < (size_t) pos)
    {
      int r;
      EINTRLOOP (r, read (from, buffer + len, (unsigned int) (pos - len)));
      if (r < 0)
        perror ("read()");
      if (r <= 0)
        break;
      len += r;
    }

  if (len > 0)
    {
      colored = colorize (buffer, &len);
      if (fwrite (colored, len, 1, to) < 1)
        perror ("fwrite()");
      fflush (to);
      free (colored);
    }

  free (buffer);

#if MK_OS_W32
  /* Switch "to" back to its original mode, so that log messages by
     Make have the same EOL format as without --output-sync.  */
  _setmode (fileno (to), prev_mode);
#endif
}

/* Returns a file descriptor to a temporary file, that will be automatically
   deleted on exit.  */
int
output_tmpfd (void)
{
  int fd = get_tmpfd (NULL);
  fd_set_append (fd);
  return fd;
}

/* Adds file descriptors to the child structure to support output_sync; one
   for stdout and one for stderr as long as they are open.  If stdout and
   stderr share a device they can share a temp file too.
   Will reset output_sync on error.  */
static void
setup_tmpfile (struct output *out)
{
  static unsigned int in_setup = 0;
  unsigned int io_state;

  /* If something fails during setup we might recurse back into this function
     while writing errors.  Make sure we don't do so infinitely.  */
  if (in_setup)
    return;
  in_setup = 1;

  io_state = check_io_state ();

  if (NONE_SET (io_state, IO_STDOUT_OK|IO_STDERR_OK))
    {
      /* This is probably useless since stdout/stderr aren't working. */
      perror_with_name ("output-sync suppressed: ", "stderr");
      goto error;
    }

  if (ANY_SET (io_state, IO_STDOUT_OK))
    {
      int fd = output_tmpfd ();
      if (fd < 0)
        goto error;
      fd_noinherit (fd);
      out->out = fd;
    }

  if (ANY_SET (io_state, IO_STDERR_OK))
    {
      if (out->out != OUTPUT_NONE && ANY_SET (io_state, IO_COMBINED_OUTERR))
        out->err = out->out;
      else
        {
          int fd = output_tmpfd ();
          if (fd < 0)
            goto error;
          fd_noinherit (fd);
          out->err = fd;
        }
    }

  in_setup = 0;
  return;

  /* If we failed to create a temp file, disable output sync going forward.  */
 error:
  O (error, NILF,
     _("cannot open output-sync lock file: suppressing output-sync"));

  output_close (out);
  output_sync = OUTPUT_SYNC_NONE;
  osync_clear ();
  in_setup = 0;
}

/* Synchronize the output of jobs in -j mode to keep the results of
   each job together. This is done by holding the results in temp files,
   one for stdout and potentially another for stderr, and only releasing
   them to "real" stdout/stderr when a semaphore can be obtained. */

void
output_dump (struct output *out)
{
#define FD_NOT_EMPTY(_f) ((_f) != OUTPUT_NONE && lseek ((_f), 0, SEEK_END) > 0)

  int outfd_not_empty = FD_NOT_EMPTY (out->out);
  int errfd_not_empty = FD_NOT_EMPTY (out->err);

  if (outfd_not_empty || errfd_not_empty)
    {
      int traced = 0;

      /* Try to acquire the semaphore.  If it fails, dump the output
         unsynchronized; still better than silently discarding it.
         We want to keep this lock for as little time as possible.  */
      if (!osync_acquire ())
        {
          O (error, NILF,
             _("warning: cannot acquire output lock: disabling output sync"));
          osync_clear ();
        }

      /* Log the working directory for this dump.  */

      if (output_sync != OUTPUT_SYNC_RECURSE && should_print_dir ())
        traced = log_working_directory (1);

      if (outfd_not_empty)
        pump_from_tmp (out->out, stdout);
      if (errfd_not_empty && out->err != out->out)
        pump_from_tmp (out->err, stderr);

      if (traced)
        log_working_directory (0);

      /* Exit the critical section.  */
      osync_release ();

      /* Truncate and reset the output, in case we use it again.  */
      if (out->out != OUTPUT_NONE)
        {
          int e;
          lseek (out->out, 0, SEEK_SET);
          EINTRLOOP (e, ftruncate (out->out, 0));
        }
      if (out->err != OUTPUT_NONE && out->err != out->out)
        {
          int e;
          lseek (out->err, 0, SEEK_SET);
          EINTRLOOP (e, ftruncate (out->err, 0));
        }
    }
}
#endif /* NO_OUTPUT_SYNC */


static int stdout_flags = -1;
static int stderr_flags = -1;

void
output_init (struct output *out)
{
  if (out)
    {
      out->out = out->err = OUTPUT_NONE;
      out->syncout = !!output_sync;
      return;
    }

  /* Force stdout/stderr into append mode (if they are files) to ensure
     parallel jobs won't lose output due to overlapping writes.  */
  stdout_flags = fd_set_append (fileno (stdout));
  stderr_flags = fd_set_append (fileno (stderr));
}

void
output_close (struct output *out)
{
  if (! out)
    {
      if (stdio_traced)
        log_working_directory (0);
      fd_reset_append(fileno (stdout), stdout_flags);
      fd_reset_append(fileno (stderr), stderr_flags);
      return;
    }

#ifndef NO_OUTPUT_SYNC
  output_dump (out);
#endif

  if (out->out >= 0)
    close (out->out);
  if (out->err >= 0 && out->err != out->out)
    close (out->err);

  output_init (out);
}

/* We're about to generate output: be sure it's set up.  */
void
output_start (void)
{
#ifndef NO_OUTPUT_SYNC
  /* If we're syncing output make sure the temporary file is set up.  */
  if (output_context && output_context->syncout)
    if (! OUTPUT_ISSET(output_context))
      setup_tmpfile (output_context);
#endif

  /* If we're not syncing this output per-line or per-target, make sure we emit
     the "Entering..." message where appropriate.  */
  if (output_sync == OUTPUT_SYNC_NONE || output_sync == OUTPUT_SYNC_RECURSE)
    if (! stdio_traced && should_print_dir ())
      stdio_traced = log_working_directory (1);
}

void
outputs (int is_err, const char *msg)
{
  if (! msg || *msg == '\0')
    return;

  output_start ();

  _outputs (output_context, is_err, msg);
}


static struct fmtstring
  {
    char *buffer;
    size_t size;
  } fmtbuf = { NULL, 0 };

static char *
get_buffer (size_t need)
{
  /* Make sure we have room.  NEED includes space for \0.  */
  if (need > fmtbuf.size)
    {
      fmtbuf.size += need * 2;
      fmtbuf.buffer = xrealloc (fmtbuf.buffer, fmtbuf.size);
    }

  fmtbuf.buffer[need-1] = '\0';

  return fmtbuf.buffer;
}

/* Print a message on stdout.  */

void
message (int prefix, size_t len, const char *fmt, ...)
{
  va_list args;
  char *start;
  char *p;

  len += strlen (fmt) + strlen (program) + INTSTR_LENGTH + 4 + 1 + 1;
  start = p = get_buffer (len);

  if (prefix)
    {
      if (makelevel == 0)
        sprintf (p, "%s: ", program);
      else
        sprintf (p, "%s[%u]: ", program, makelevel);
      p += strlen (p);
    }

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  strcat (p, "\n");

  assert (start[len-1] == '\0');
  outputs (0, start);
}

/* Print a warning message.  */

void
warning_msg (const floc *flocp, size_t len, const char *fmt, ...)
{
  va_list args;
  char *p;

  len += (strlen (fmt) + strlen (program)
          + (flocp && flocp->filenm ? strlen (flocp->filenm) : 0)
          + INTSTR_LENGTH + 4 + 1 + 1
          + CSTRLEN (COLOR_WARNING) + CSTRLEN (COLOR_RESET));
  p = get_buffer (len);

  if (flocp && flocp->filenm)
    sprintf (p, COLOR_WARNING "%s:%lu: ", flocp->filenm, flocp->lineno + flocp->offset);
  else if (makelevel == 0)
    sprintf (p, COLOR_WARNING "%s: ", program);
  else
    sprintf (p, COLOR_WARNING "%s[%u]: ", program, makelevel);
  p += strlen (p);

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  strcat (p, COLOR_RESET "\n");

  assert (fmtbuf.buffer[len-1] == '\0');
  outputs (1, fmtbuf.buffer);
}

/* Print an error message.  */

void
error (const floc *flocp, size_t len, const char *fmt, ...)
{
  va_list args;
  char *start;
  char *p;

  len += (strlen (fmt) + strlen (program)
          + (flocp && flocp->filenm ? strlen (flocp->filenm) : 0)
          + INTSTR_LENGTH + 4 + 1 + 1
          + CSTRLEN (COLOR_ERROR) + CSTRLEN (COLOR_RESET));
  start = p = get_buffer (len);

  if (flocp && flocp->filenm)
    sprintf (p, COLOR_ERROR "%s:%lu: ", flocp->filenm, flocp->lineno + flocp->offset);
  else if (makelevel == 0)
    sprintf (p, COLOR_ERROR "%s: ", program);
  else
    sprintf (p, COLOR_ERROR "%s[%u]: ", program, makelevel);
  p += strlen (p);

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  strcat (p, COLOR_RESET "\n");

  assert (start[len-1] == '\0');
  outputs (1, start);
}

/* Print an error message and exit.  */

void
fatal (const floc *flocp, size_t len, const char *fmt, ...)
{
  const char *stop = _(".  Stop.\33[0m\n");
  va_list args;
  char *start;
  char *p;

  len += (strlen (fmt) + strlen (program)
          + (flocp && flocp->filenm ? strlen (flocp->filenm) : 0)
          + INTSTR_LENGTH + 8 + strlen (stop) + 1 + CSTRLEN (COLOR_ERROR));
  start = p = get_buffer (len);

  if (flocp && flocp->filenm)
    sprintf (p, COLOR_ERROR "%s:%lu: *** ", flocp->filenm, flocp->lineno + flocp->offset);
  else if (makelevel == 0)
    sprintf (p, COLOR_ERROR "%s: *** ", program);
  else
    sprintf (p, COLOR_ERROR "%s[%u]: *** ", program, makelevel);
  p += strlen (p);

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  strcat (p, stop);

  assert (start[len-1] == '\0');
  outputs (1, start);

  die (MAKE_FAILURE);
}

/* Format a message and return a pointer to an internal buffer.  */

char *
format (const char *prefix, size_t len, const char *fmt, ...)
{
  va_list args;
  size_t plen = prefix ? strlen (prefix) : 0;
  char *start;
  char *p;

  len += strlen (fmt) + plen + 1;
  start = p = get_buffer (len);

  if (plen)
    p = mempcpy (p, prefix, plen);

  va_start (args, fmt);
  vsprintf (p, fmt, args);
  va_end (args);

  return start;
}

/* Print an error message from errno.  */

void
perror_with_name (const char *str, const char *name)
{
  const char *err = strerror (errno);
  OSSS (error, NILF, _("%s%s: %s"), str, name, err);
}

/* Print an error message from errno and exit.  */

void
pfatal_with_name (const char *name)
{
  const char *err = strerror (errno);
  OSS (fatal, NILF, _("%s: %s"), name, err);

  /* NOTREACHED */
}

/* Print a message about out of memory (not using more heap) and exit.
   Our goal here is to be sure we don't try to allocate more memory, which
   means we don't want to use string translations or normal cleanup.  */

void
out_of_memory ()
{
  writebuf (FD_STDOUT, program, strlen (program));
  writebuf (FD_STDOUT, STRING_SIZE_TUPLE (": *** virtual memory exhausted\n"));
  exit (MAKE_FAILURE);
}
    