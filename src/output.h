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

struct output_stream;

struct output
  {
    int out;
    int err;
    struct output_stream *stream_out; /* Colored stdout while a child runs.  */
    struct output_stream *stream_err; /* Colored stderr, if separate.  */
    unsigned int syncout:1;     /* True if we want to synchronize output.  */
 };

/* Nonzero if make colors errors and warnings written to stdout / stderr.  */
extern int output_color_out;
extern int output_color_err;

/* Decide on colors for --color=WHEN: "auto" (also for NULL), "always" or
   "never".  "auto" colors a stream only if it is a terminal and neither
   NO_COLOR nor TERM=dumb is set.  */
void output_color_setup (const char *when);

extern struct output *output_context;
extern unsigned int stdio_traced;

#define FD_STDIN  (fileno (stdin))
#define FD_STDOUT (fileno (stdout))
#define FD_STDERR (fileno (stderr))

#define OUTPUT_SET(_new)    do{ output_context = (_new)->syncout ? (_new) : NULL; }while(0)
#define OUTPUT_UNSET()      do{ output_context = NULL; }while(0)

#define OUTPUT_TRACED()     do{ stdio_traced = 1; }while(0)
#define OUTPUT_IS_TRACED()  (!!stdio_traced)

/* Write a buffer directly to the given file descriptor.
   This handles errors etc.  */
int output_write (int fd, const void *buffer, size_t len);

/* Initialize and close a child output structure: if NULL do this program's
   output (this should only be done once).  */
void output_init (struct output *out);
void output_close (struct output *out);

/* In situations where output may be about to be displayed but we're not
   sure if we've set it up yet, call this.  */
void output_start (void);

/* Show a message on stdout or stderr.  Will start the output if needed.  */
void outputs (int is_err, const char *msg);

#if defined(NO_OUTPUT_SYNC)
# define output_dump(_o) (void)(0)
#else
/* Dump any child output content to stdout, and reset it.  */
void output_dump (struct output *out);

#endif

/* Streaming a child's output through a pipe, colored line by line while the
   child runs, needs a reader thread.  Without it colors only reach child
   output that --output-sync collects.  */
#if MK_OS_W32 \
    || (defined(HAVE_PTHREAD_CREATE) && defined(HAVE_PTHREAD_H) \
        && defined(HAVE_POLL_H) && !MK_OS_OS2 && !MK_OS_VMS && !MK_OS_DOS)
# define HAVE_OUTPUT_STREAMS 1

/* Set up streams for a child that is about to start, if colors are on and
   its output is not synchronized.  */
void output_streams_open (struct output *out);

/* Replace *OUTFD and *ERRFD with the pipe ends the child must write to, if
   it has streams.  */
void output_stream_fds (const struct output *out, int *outfd, int *errfd);

/* Close make's copies of the pipe ends once the child has started.  */
void output_streams_started (struct output *out);

/* Wait until all output of the child has been written, then free its
   streams.  Call after the child has exited and before reporting on it.  */
void output_streams_close (struct output *out);
#else
# define output_streams_open(_o)        (void)(0)
# define output_stream_fds(_o,_of,_ef)  (void)(0)
# define output_streams_started(_o)     (void)(0)
# define output_streams_close(_o)       (void)(0)
#endif
