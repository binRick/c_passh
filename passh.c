/* passh - automate ssh password authentication
 * Copyright (C) 2017-2020 Clark Wang <dearvoid@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#if !defined (__APPLE__) && !defined (__FreeBSD__) && !defined (_AIX)
#define _XOPEN_SOURCE          600 /* for posix_openpt() */
#endif
#define LOGLEVEL               3
#define DO_VERBOSE_LOG         false
#define DO_TIME_LOG            false
#define THREAD_BUFFER_BYTES    1024 * 256 * 1
#define THREAD_BUFFER_LINES    1000
#define DEBUG_MEMORY
///////////////////////////////////////////////////////////////////////////
#include "submodules/debug-memory/debug_memory.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fcntl.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
///////////////////////////////////////////////////////////////////////////
#include "submodules/ansi-codes/ansi-codes.h"
#include "submodules/c_string_buffer/include/stringbuffer.h"
#include "submodules/c_stringfn/include/stringfn.h"
#include "submodules/c_timer/include/c_timer.h"
#include "submodules/chan/src/chan.h"
#include "submodules/log.h/log.h"
#include "submodules/timestamp/timestamp.h"
///////////////////////////////////////////////////////////////////////////
#define BUFFSIZE           (8 * 1024)
#define DEFAULT_COUNT      0
#define DEFAULT_TIMEOUT    0
#define DEFAULT_PASSWD     "password"
#define DEFAULT_PROMPT     "[Pp]assword: \\{0,1\\}$"
#define DEFAULT_YESNO      "(yes/no)? \\{0,1\\}$"
#define ERROR_GENERAL      (200 + 1)
#define ERROR_USAGE        (200 + 2)
#define ERROR_TIMEOUT      (200 + 3)
#define ERROR_SYS          (200 + 4)
#define ERROR_MAX_TRIES    (200 + 5)
///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////
static void __debug_memory();
ssize_t writen(int fd, const void *ptr, size_t n);
void fatal_sys(const char *fmt, ...);

///////////////////////////////////////////////////////////////////////////


char * const MY_NAME  = "passh";
char * const VERSION_ = "1.0.2";
struct stdout_content_t {
  char   *content;
  char   *sender;
  size_t length;
  int    fd1;
  int    fd2;
};
struct times_t {
  size_t write2;
  size_t flush_buffer;
};
struct times_t times = {
  .write2       = 0,
  .flush_buffer = 0,
};
struct thread_t {
  char                *name;
  pthread_t           *thread;
  pthread_mutex_t     *mutex;
  bool                enabled;
  void                *(*handler)(void *);
  unsigned long       started_ts;
  unsigned long       dur_ms;
  size_t              processed_qty;
  size_t              processed_bytes;
  size_t              processed_lines;
  size_t              qty;
  size_t              buffer_flushes_qty;
  struct StringBuffer *sb;
  size_t              buffer_bytes;
  size_t              buffer_lines;
};
static void *stdout_processor_manager(void *);
static void *stdout_processor(void *);

static void __attribute__((destructor)) __passh_destructor();

static void __attribute__((constructor)) __passh_constructor();

enum mutex_types_t {
  MUTEX_TYPE_WRITE2,
  MUTEX_TYPES_QTY,
};
enum thread_types_t {
  THREAD_TYPE_STDOUT_PROCESSOR0,
  THREAD_TYPE_STDOUT_PROCESSOR1,
  THREAD_TYPE_STDOUT_PROCESSOR2,
  THREAD_TYPE_STDOUT_PROCESSOR_MANAGER,
  THREAD_TYPES_QTY,
};
enum chan_types_t {
  CHAN_TYPE_STDOUT,
  CHAN_TYPE_DONE,
  CHAN_TYPE_STDOUT_PROCESSOR_DONE,
  CHAN_TYPE_STDOUT_PROCESSORS_DONE,
  CHAN_TYPE_PROCESS_DONE,
  CHAN_TYPES_QTY,
};

struct mutex_t {
  char            *name;
  pthread_mutex_t *mutex;
};
struct mutex_t  *mutexes[] = {
  [MUTEX_TYPE_WRITE2] = &(struct mutex_t){
    .name  = "write2",
    .mutex = NULL,
  },
  [MUTEX_TYPES_QTY] = NULL,
};

static size_t   chan_buffer_sizes[] = {
  [CHAN_TYPE_STDOUT]                 = 1000,
  [CHAN_TYPE_STDOUT_PROCESSOR_DONE]  = 1,
  [CHAN_TYPE_STDOUT_PROCESSORS_DONE] = 1,
  [CHAN_TYPE_PROCESS_DONE]           = 1,
  [CHAN_TYPE_DONE]                   = 1,
  [CHAN_TYPES_QTY]                   = 0,
};

struct thread_t *threads[] = {
  [THREAD_TYPE_STDOUT_PROCESSOR0] = &(struct thread_t)        {
    .thread          = NULL,
    .mutex           = NULL,
    .handler         = stdout_processor,
    .processed_qty   = 0,
    .processed_bytes = 0,
    .enabled         = true,
    .sb              = NULL,
    .buffer_bytes    = THREAD_BUFFER_BYTES * 1,
    .buffer_lines    = THREAD_BUFFER_LINES,
  },
  [THREAD_TYPE_STDOUT_PROCESSOR1] = &(struct thread_t)        {
    .thread          = NULL,
    .mutex           = NULL,
    .handler         = stdout_processor,
    .processed_qty   = 0,
    .processed_bytes = 0,
    .enabled         = true,
    .sb              = NULL,
    .buffer_bytes    = THREAD_BUFFER_BYTES * 2,
    .buffer_lines    = THREAD_BUFFER_LINES,
  },
  [THREAD_TYPE_STDOUT_PROCESSOR2] = &(struct thread_t)        {
    .thread          = NULL,
    .mutex           = NULL,
    .handler         = stdout_processor,
    .processed_qty   = 0,
    .processed_bytes = 0,
    .enabled         = true,
    .sb              = NULL,
    .buffer_bytes    = THREAD_BUFFER_BYTES * 4,
    .buffer_lines    = THREAD_BUFFER_LINES,
  },
  [THREAD_TYPE_STDOUT_PROCESSOR_MANAGER] = &(struct thread_t) {
    .thread        = NULL,
    .mutex         = NULL,
    .handler       = stdout_processor_manager,
    .processed_qty = 0,
    .enabled       = true,
    .sb            = NULL,
    .buffer_bytes  = 0,
    .buffer_lines  = 0,
  },
  [THREAD_TYPES_QTY] = NULL,
};
static chan_t   *chans[] = {
  [CHAN_TYPE_STDOUT]                = NULL,
  [CHAN_TYPE_STDOUT_PROCESSOR_DONE] = NULL,
  [CHAN_TYPE_PROCESS_DONE]          = NULL,
  [CHAN_TYPE_DONE]                  = NULL,
};


//////////////////////////////////////////////////////////////////
static void *stdout_processor_manager(void *_ctx){
  ct_start(NULL);
  struct thread_t *ctx = (struct thread_t *)_ctx;
  assert(chan_recv(chans[CHAN_TYPE_PROCESS_DONE], NULL) == 0);
  assert(chan_close(chans[CHAN_TYPE_STDOUT]) == 0);
  assert(chan_recv(chans[CHAN_TYPE_STDOUT_PROCESSOR_DONE], NULL) == 0);
  assert(chan_recv(chans[CHAN_TYPE_STDOUT_PROCESSOR_DONE], NULL) == 0);
  assert(chan_recv(chans[CHAN_TYPE_STDOUT_PROCESSOR_DONE], NULL) == 0);
  assert(chan_close(chans[CHAN_TYPE_PROCESS_DONE]) == 0);
  assert(chan_close(chans[CHAN_TYPE_STDOUT_PROCESSOR_DONE]) == 0);
  assert(chan_send(chans[CHAN_TYPE_STDOUT_PROCESSORS_DONE], "1") == 0);
  char *dur = ct_stop("");
  if (DO_VERBOSE_LOG) {
    log_debug("closed channels in %s", dur);
  }
  return((void *)NULL);
}

#define write2(sender, fd1, fd2, buf, len)                                                      \
  do {                                                                                          \
    ct_start(NULL);                                                                             \
    char *msg;                                                                                  \
    int  fds[2] = { fd1, fd2 };                                                                 \
    int  i;                                                                                     \
    for (i = 0; i < 2; ++i) {                                                                   \
      if (fds[i] < 0) {                                                                         \
        continue;                                                                               \
      }                                                                                         \
      int wrote = writen(fds[i], buf, len);                                                     \
      if (wrote != len) {                                                                       \
        fatal_sys("<%s> write: fd %d | wrote %d but expected  %d", sender, fds[i], wrote, len); \
      }                                                                                         \
    }                                                                                           \
    if (true == DO_TIME_LOG) {                                                                  \
      asprintf(&msg, "%s-%s", "write2", sender);                                                \
      char   *dur = ct_stop("");                                                                \
      size_t dur_ = atoi(dur) * 10;                                                             \
      times.write2 += dur_;                                                                     \
      log_debug("dur_: %lu", dur_);                                                             \
      log_debug("%s- %s", msg, dur);                                                            \
    }                                                                                           \
  } while (0)

#define FLUSH_THREAD_BUFFER(sender)                                                                                             \
  do {                                                                                                                          \
    long unsigned start_ts;                                                                                                     \
    long unsigned dur_ms;                                                                                                       \
    size_t        len;                                                                                                          \
    char          *tmp, *msg;                                                                                                   \
    ct_start(NULL);                                                                                                             \
    if (true == DO_VERBOSE_LOG) {                                                                                               \
      log_debug("buffer: %lu/%lu",                                                                                              \
                stringbuffer_get_content_size(ctx->sb),                                                                         \
                ctx->buffer_bytes                                                                                               \
                );                                                                                                              \
      start_ts = timestamp();                                                                                                   \
    }                                                                                                                           \
    {                                                                                                                           \
      pthread_mutex_lock(ctx->mutex);                                                                                           \
      len = stringbuffer_get_content_size(ctx->sb);                                                                             \
      tmp = stringbuffer_to_string(ctx->sb);                                                                                    \
      assert(stringbuffer_clear(ctx->sb) == true);                                                                              \
      pthread_mutex_unlock(ctx->mutex);                                                                                         \
      pthread_mutex_lock(mutexes[MUTEX_TYPE_WRITE2]->mutex);                                                                    \
      write2(stdout_content->sender, stdout_content->fd1, stdout_content->fd2, tmp, len);                                       \
      pthread_mutex_unlock(mutexes[MUTEX_TYPE_WRITE2]->mutex);                                                                  \
      ctx->buffer_flushes_qty++;                                                                                                \
    }                                                                                                                           \
    if (true == DO_VERBOSE_LOG) {                                                                                               \
      dur_ms = timestamp() - start_ts;                                                                                          \
      log_debug("flushed %lub buffer in %ldms",                                                                                 \
                len,                                                                                                            \
                dur_ms                                                                                                          \
                );                                                                                                              \
    }                                                                                                                           \
    if (true == DO_TIME_LOG) {                                                                                                  \
      asprintf(&msg, "%s-%s-fd1-%d-fd2-%d-%lub", "flush_thread_buffer", sender, stdout_content->fd1, stdout_content->fd2, len); \
      char   *dur = ct_stop("");                                                                                                \
      size_t dur_ = atoi(dur) * 10;                                                                                             \
      times.flush_buffer += dur_;                                                                                               \
      log_debug("dur_- %lu", dur_);                                                                                             \
      log_debug("%s- %s", msg, dur);                                                                                            \
    }                                                                                                                           \
  }while (0)
#define SHOULD_FLUSH_BUFFER() \
  ((stringbuffer_get_content_size(ctx->sb) > ctx->buffer_bytes) || (stdout_content_lines.count > ctx->buffer_lines))


static void * stdout_processor(void *_ctx){
  struct thread_t *ctx = (struct thread_t *)_ctx;

  if (ctx->sb == NULL) {
    ctx->sb = stringbuffer_new_with_options(ctx->buffer_bytes * 2, true);
  }

  ctx->started_ts = timestamp();
  void                    *_stdout_content;
  struct stdout_content_t *stdout_content;
  struct StringFNStrings  stdout_content_lines;
  bool                    should_flush_buffer = false;

  while (chan_recv(chans[CHAN_TYPE_STDOUT], &_stdout_content) == 0) {
    stdout_content = (struct stdout_content_t *)(_stdout_content);
    if (stdout_content && stdout_content->length > 0) {
      {
        pthread_mutex_lock(ctx->mutex);
        stdout_content_lines = stringfn_split_lines(stdout_content->content);
        ctx->processed_qty++;
        ctx->processed_bytes += stdout_content->length;
        ctx->processed_lines += stdout_content_lines.count;
        stringfn_release_strings_struct(stdout_content_lines);
        stringbuffer_append_string(ctx->sb, stdout_content->content);
        pthread_mutex_unlock(ctx->mutex);
      }
      {
        pthread_mutex_lock(ctx->mutex);
        should_flush_buffer = SHOULD_FLUSH_BUFFER();
        pthread_mutex_unlock(ctx->mutex);
        if (should_flush_buffer == true) {
          char *sender = stdout_content->sender;
          FLUSH_THREAD_BUFFER(sender);
        }
      }
    }
  }
  if (stringbuffer_get_content_size(ctx->sb) > 0) {
    char *sender = "post-stdout_processor";
    FLUSH_THREAD_BUFFER(sender);
  }

  ctx->dur_ms = timestamp() - ctx->started_ts;
  if (DO_VERBOSE_LOG) {
    log_debug(
      AC_RESETALL "received all stdouts | processed "
      AC_RESETALL AC_RED "%lu buffer flushes,"
      " "
      AC_RESETALL AC_BLUE "%lu stdouts,"
      " "
      AC_RESETALL AC_MAGENTA "%lu lines,"
      " "
      AC_RESETALL AC_GREEN "%lu bytes in"
      " "
      AC_RESETALL AC_YELLOW "%lums"
      ,
      ctx->buffer_flushes_qty,
      ctx->processed_qty,
      ctx->processed_lines,
      ctx->processed_bytes,
      ctx->dur_ms
      );
  }
  assert(chan_send(chans[CHAN_TYPE_STDOUT_PROCESSOR_DONE], "1") == 0);
  return((void *)NULL);
} /* stdout_processor */


//////////////////////////////////////////////////////////////////
static void __debug_memory(){
  print_allocated_memory();
}
static void __attribute__((destructor)) __passh_destructor(){
  for (size_t i = 0; i < THREAD_TYPES_QTY; i++) {
    if (threads[i]->enabled == false) {
      continue;
    }
    if (DO_VERBOSE_LOG) {
      log_debug("joining thread #%lu", i);
    }
    pthread_join(*threads[i]->thread, NULL);
    if (DO_VERBOSE_LOG) {
      log_debug("joined thread #%lu", i);
    }
  }
  for (size_t i = 0; i < CHAN_TYPES_QTY; i++) {
    chan_dispose(chans[i]);
  }
  for (size_t i = 0; i < MUTEX_TYPES_QTY; i++) {
    free(mutexes[i]->mutex);
  }
  for (size_t i = 0; i < THREAD_TYPES_QTY; i++) {
    free(threads[i]->mutex);
    free(threads[i]->thread);
  }
  if (DO_TIME_LOG) {
    log_debug("Write2 time: %lums|Flush Buffer time:%lums|",
              times.write2,
              times.flush_buffer
              );
  }
}
static void __attribute__((constructor)) __passh_constructor(){
  if (atexit(__debug_memory) < 0) {
    fatal_sys("atexit error");
  }
  long unsigned started_ts;
  long unsigned dur_ms;

  if (DO_VERBOSE_LOG) {
    started_ts = timestamp();
  }

  ct_set_unit(ct_MICROSECONDS);

  for (size_t i = 0; i < MUTEX_TYPES_QTY; i++) {
    assert((mutexes[i]->mutex = calloc(1, sizeof(pthread_mutex_t))) != NULL);
    assert((pthread_mutex_init(mutexes[i]->mutex, NULL) == 0));
  }
  for (size_t i = 0; i < CHAN_TYPES_QTY; i++) {
    assert((chans[i] = chan_init(chan_buffer_sizes[i])) != NULL);
  }
  for (size_t i = 0; i < THREAD_TYPES_QTY; i++) {
    if (threads[i]->enabled == false) {
      continue;
    }
    assert((threads[i]->mutex = calloc(1, sizeof(pthread_mutex_t))) != NULL);
    assert(pthread_mutex_init(threads[i]->mutex, NULL) == 0);
    assert((threads[i]->thread = calloc(1, sizeof(pthread_t))) != NULL);
    assert(pthread_create(threads[i]->thread, NULL, threads[i]->handler, (void *)threads[i]) == 0);
  }
  if (DO_VERBOSE_LOG) {
    dur_ms = timestamp() - started_ts;
    log_debug("constructor finished in %ldms", dur_ms);
  }
}
//////////////////////////////////////////////////////////////////
static struct {
  char           *progname;
  bool           reset_on_exit;
  struct termios save_termios;
  bool           SIGCHLDed;
  bool           received_winch;
  bool           stdin_is_tty;
  bool           now_interactive;

  int            fd_ptym;

  struct {
    bool    ignore_case;
    bool    nohup_child;
    bool    fatal_no_prompt;
    bool    auto_yesno;
    char    *password;
    char    *passwd_prompt;
    char    *yesno_prompt;
    regex_t re_prompt;
    regex_t re_yesno;
    int     timeout;
    int     tries;
    bool    fatal_more_tries;
    char    **command;

    char    *log_to_pty;
    char    *log_from_pty;
  } opt;
} g;


void show_version(void) {
  printf("%s %s\n", MY_NAME, VERSION_);

  exit(0);
}


void usage(int exitcode) {
  printf("Usage: %s [OPTION]... COMMAND...\n"
         "\n"
         "  -c <N>          Send at most <N> passwords (0 means infinite. Default: %d)\n"
         "  -C              Exit if prompted for the <N+1>th password\n"
         "  -h              Help\n"
         "  -i              Case insensitive for password prompt matching\n"
         "  -n              Nohup the child (e.g. used for `ssh -f')\n"
         "  -p <password>   The password (Default: `" DEFAULT_PASSWD "')\n"
         "  -p env:<var>    Read password from env var\n"
         "  -p file:<file>  Read password from file\n"
         "  -P <prompt>     Regexp (BRE) for the password prompt\n"
         "                  (Default: `" DEFAULT_PROMPT "')\n"
         "  -l <file>       Save data written to the pty\n"
         "  -L <file>       Save data read from the pty\n"
         "  -t <timeout>    Timeout waiting for next password prompt\n"
         "                  (0 means no timeout. Default: %d)\n"
         "  -T              Exit if timed out waiting for password prompt\n"
         "  -V              Show version\n"
         "  -y              Auto answer `(yes/no)?' questions\n"
#if 0
         "  -Y <pattern>    Regexp (BRE) for the `yes/no' prompt\n"
         "                  (Default: `" DEFAULT_YESNO "')\n"
#endif
         "\n"
         "Report bugs to Clark Wang <dearvoid@gmail.com>\n"
         "", g.progname, DEFAULT_COUNT, DEFAULT_TIMEOUT);

  exit(exitcode);
}


void fatal(int rcode, const char *fmt, ...) {
  va_list ap;
  char    buf[1024];

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  /* in case stdout and stderr are the same */
  fflush(stdout);

  fprintf(stderr, "!! %s\r\n", buf);

  /* flush all open files */
  fflush(NULL);

  exit(rcode);
}


void fatal_sys(const char *fmt, ...){
  va_list ap;
  char    buf[1024];
  int     error = errno;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  fatal(ERROR_SYS, "%s: %s (%d)", buf, strerror(error), error);
}


void startup() {
  g.opt.passwd_prompt = DEFAULT_PROMPT;
  g.opt.yesno_prompt  = DEFAULT_YESNO;
  g.opt.password      = DEFAULT_PASSWD;
  g.opt.tries         = DEFAULT_COUNT;
  g.opt.timeout       = DEFAULT_TIMEOUT;
}


char *arg2pass(char *optarg) {
  char *pass = NULL;

  if (strncmp(optarg, "file:", 5) == 0) {
    FILE *fp       = fopen(optarg + 5, "r");
    char buf[1024] = "";

    if (fp == NULL) {
      fatal_sys("failed to open file %s", optarg + 5);
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
      fatal(ERROR_GENERAL, "failed to read the file");
    }
    fclose(fp);

    pass = strtok(buf, "\r\n");
    if (pass) {
      pass = strdup(pass);
    } else {
      pass = strdup("");
    }
  } else if (strncmp(optarg, "env:", 4) == 0) {
    pass = getenv(optarg + 4);
    if (pass) {
      pass = strdup(pass);
    } else {
      fatal(ERROR_GENERAL, "env var not found: %s", optarg + 4);
    }
  } else {
    pass = strdup(optarg);
  }

  return(pass);
}


void getargs(int argc, char **argv) {
  int ch, i, r, reflag;

  if ((g.progname = strrchr(argv[0], '/')) != NULL) {
    ++g.progname;
  } else {
    g.progname = argv[0];
  }

  if (argc == 1 || (argc == 2 && strcmp("--help", argv[1]) == 0)) {
    usage(0);
  }

  if (argc == 2 && strcmp("--version", argv[1]) == 0) {
    show_version();
  }

  /*
   * If the first character of optstring is '+' or the environment variable
   * POSIXLY_CORRECT is set, then option processing stops as soon as a
   * nonoption argument is encountered.
   */
  while ((ch = getopt(argc, argv, "+:c:Chil:L:np:P:t:TVy")) != -1) {
    switch (ch) {
    case 'c':
      g.opt.tries = atoi(optarg);
      break;
    case 'C':
      g.opt.fatal_more_tries = true;
      break;
    case 'h':
      usage(0);

    case 'i':
      g.opt.ignore_case = true;
      break;

    case 'l':
      g.opt.log_to_pty = optarg;
      break;

    case 'L':
      g.opt.log_from_pty = optarg;
      break;

    case 'n':
      g.opt.nohup_child = true;
      break;

    case 'p':
      g.opt.password = arg2pass(optarg);
      for (i = 0; i < strlen(optarg); ++i) {
        optarg[i] = '*';
      }
      if (g.opt.password == NULL) {
        fatal(ERROR_USAGE, "Error: failed to get password");
      }
      break;

    case 'P':
      g.opt.passwd_prompt = optarg;
      break;

    case 't':
      g.opt.timeout = atoi(optarg);
      break;

    case 'T':
      g.opt.fatal_no_prompt = true;
      break;

    case 'V':
      show_version();
      break;

    case 'y':
      g.opt.auto_yesno = true;
      break;
#if 0
    case 'Y':
      g.opt.yesno_prompt = optarg;
      break;
#endif
    case ':':
      fatal(ERROR_USAGE, "Error: option '-%c' requires an argument", optopt);
      break;

    case '?':
    default:
      fatal(ERROR_USAGE, "Error: unknown option '-%c'", optopt);
    } /* switch */
  }
  argc -= optind;
  argv += optind;

  if (0 == argc) {
    fatal(ERROR_USAGE, "Error: no command specified");
  }
  g.opt.command = argv;

  if (0 == strlen(g.opt.passwd_prompt)) {
    fatal(ERROR_USAGE, "Error: empty prompt");
  }

  /* Password: */
  reflag  = 0;
  reflag |= g.opt.ignore_case ? REG_ICASE : 0;
  r       = regcomp(&g.opt.re_prompt, g.opt.passwd_prompt, reflag);
  if (r != 0) {
    fatal(ERROR_USAGE, "Error: invalid RE for password prompt");
  }
  /* (yes/no)? */
  r = regcomp(&g.opt.re_yesno, g.opt.yesno_prompt, reflag);
  if (r != 0) {
    fatal(ERROR_USAGE, "Error: invalid RE for yes/no prompt");
  }
} /* getargs */


int ptym_open(char *pts_name, int pts_namesz) {
  char *ptr;
  int  fdm;

  snprintf(pts_name, pts_namesz, "/dev/ptmx");

  fdm = posix_openpt(O_RDWR);
  if (fdm < 0) {
    return(-1);
  }

  if (grantpt(fdm) < 0) {
    close(fdm);
    return(-2);
  }

  if (unlockpt(fdm) < 0) {
    close(fdm);
    return(-3);
  }

  if ((ptr = ptsname(fdm)) == NULL) {
    close(fdm);
    return(-4);
  }

  snprintf(pts_name, pts_namesz, "%s", ptr);
  return(fdm);
}


int ptys_open(char *pts_name) {
  int fds;

  if ((fds = open(pts_name, O_RDWR)) < 0) {
    return(-5);
  }
  return(fds);
}


pid_t pty_fork(int *ptrfdm, char *slave_name, int slave_namesz,
               const struct termios *slave_termios,
               const struct winsize *slave_winsize){
  int   fdm, fds;
  pid_t pid;
  char  pts_name[32];

  if ((fdm = ptym_open(pts_name, sizeof(pts_name))) < 0) {
    fatal_sys("can't open master pty: %s, error %d", pts_name, fdm);
  }

  if (slave_name != NULL) {
    /*
     * Return name of slave.  Null terminate to handle case
     * where strlen(pts_name) > slave_namesz.
     */
    snprintf(slave_name, slave_namesz, "%s", pts_name);
  }

  if ((pid = fork()) < 0) {
    return(-1);
  } else if (pid == 0) {
    /*
     * child
     */
    if (setsid() < 0) {
      fatal_sys("setsid error");
    }

    /*
     * System V acquires controlling terminal on open().
     */
    if ((fds = ptys_open(pts_name)) < 0) {
      fatal_sys("can't open slave pty");
    }

    /* all done with master in child */
    close(fdm);

#if defined (TIOCSCTTY)
    /*
     * TIOCSCTTY is the BSD way to acquire a controlling terminal.
     *
     * Don't check the return code. It would fail in Cygwin.
     */
    ioctl(fds, TIOCSCTTY, (char *)0);
#endif
    /*
     * Set slave's termios and window size.
     */
    if (slave_termios != NULL) {
      if (tcsetattr(fds, TCSANOW, slave_termios) < 0) {
        fatal_sys("tcsetattr error on slave pty");
      }
    }
    if (slave_winsize != NULL) {
      if (ioctl(fds, TIOCSWINSZ, slave_winsize) < 0) {
        fatal_sys("TIOCSWINSZ error on slave pty");
      }
    }

    /*
     * Slave becomes stdin/stdout/stderr of child.
     */
    if (dup2(fds, STDIN_FILENO) != STDIN_FILENO) {
      fatal_sys("dup2 error to stdin");
    }
    if (dup2(fds, STDOUT_FILENO) != STDOUT_FILENO) {
      fatal_sys("dup2 error to stdout");
    }
    if (dup2(fds, STDERR_FILENO) != STDERR_FILENO) {
      fatal_sys("dup2 error to stderr");
    }
    if (fds != STDIN_FILENO && fds != STDOUT_FILENO
        && fds != STDERR_FILENO) {
      close(fds);
    }

    return(0);
  } else {
    /*
     * parent
     */
    *ptrfdm = fdm;
    return(pid);
  }
} /* pty_fork */


int tty_raw(int fd, struct termios *save_termios) {
  int            err;
  struct termios buf;

  if (tcgetattr(fd, &buf) < 0) {
    return(-1);
  }
  *save_termios = buf;

  /*
   * Echo off, canonical mode off, extended input
   * processing off, signal chars off.
   */
  buf.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  /*
   * No SIGINT on BREAK, CR-to-NL off, input parity
   * check off, don't strip 8th bit on input, output
   * flow control off.
   */
  buf.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  /*
   * Clear size bits, parity checking off.
   */
  buf.c_cflag &= ~(CSIZE | PARENB);

  /*
   * Set 8 bits/char.
   */
  buf.c_cflag |= CS8;

  /*
   * Output processing off.
   */
  buf.c_oflag &= ~(OPOST);

  /*
   * Case B: 1 byte at a time, no timer.
   */
  buf.c_cc[VMIN]  = 1;
  buf.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSAFLUSH, &buf) < 0) {
    return(-1);
  }

  /*
   * Verify that the changes stuck.  tcsetattr can return 0 on
   * partial success.
   */
  if (tcgetattr(fd, &buf) < 0) {
    err = errno;
    tcsetattr(fd, TCSAFLUSH, save_termios);
    errno = err;
    return(-1);
  }
  if ((buf.c_lflag & (ECHO | ICANON | IEXTEN | ISIG))
      || (buf.c_iflag & (BRKINT | ICRNL | INPCK | ISTRIP | IXON))
      || (buf.c_cflag & (CSIZE | PARENB | CS8)) != CS8
      || (buf.c_oflag & OPOST) || buf.c_cc[VMIN] != 1
      || buf.c_cc[VTIME] != 0) {
    /*
     * Only some of the changes were made.  Restore the
     * original settings.
     */
    tcsetattr(fd, TCSAFLUSH, save_termios);
    errno = EINVAL;
    return(-1);
  }

  return(0);
} /* tty_raw */


int tty_reset(int fd, struct termios *termio) {
  if (tcsetattr(fd, TCSAFLUSH, termio) < 0) {
    return(-1);
  }
  return(0);
}


void tty_atexit(void) {
  if (g.reset_on_exit) {
    tty_reset(STDIN_FILENO, &g.save_termios);
  }
}


ssize_t read_if_ready(int fd, char *buf, size_t n) {
  struct timeval timeout;
  fd_set         fds;
  int            nread;

  timeout.tv_sec  = 0;
  timeout.tv_usec = 0;

  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  if (select(fd + 1, &fds, NULL, NULL, &timeout) < 0) {
    return(-1);
  }
  if (!FD_ISSET(fd, &fds)) {
    return(0);
  }
  if ((nread = read(fd, buf, n)) < 0) {
    return(-1);
  }
  return(nread);
}


ssize_t writen(int fd, const void *ptr, size_t n){
  size_t  nleft;
  ssize_t nwritten;

  nleft = n;
  while (nleft > 0) {
    if ((nwritten = write(fd, ptr, nleft)) < 0) {
      if (nleft == n) {
        return(-1);
      } else {
        /* error, return amount written so far */
        break;
      }
    } else if (nwritten == 0) {
      break;
    }
    nleft -= nwritten;
    ptr   += nwritten;
  }
  return(n - nleft);
}


/*
 * The only portable use of signal() is to set a signal's disposition
 * to SIG_DFL or SIG_IGN.  The semantics when using signal() to
 * establish a signal handler vary across systems (and POSIX.1
 * explicitly  permits  this variation); do not use it for this purpose.
 *
 * POSIX.1 solved the portability mess by specifying sigaction(2),
 * which provides explicit control of the semantics when a signal
 * handler is invoked; use that interface instead of signal().
 *
 * In the original UNIX systems, when a handler that was established
 * using signal() was invoked by the delivery of a signal, the
 * disposition of the signal would be reset to SIG_DFL, and the system
 * did not block delivery of further instances of the signal.  This is
 * equivalent to calling sigaction(2) with the following flags:
 *
 *   sa.sa_flags = SA_RESETHAND | SA_NODEFER;
 */
void sig_handle(int signo, void (*handler)(int)) {
  struct sigaction act;

  memset(&act, 0, sizeof(act));
  act.sa_handler = handler;
  sigaction(signo, &act, NULL);
}


void sig_child(int signo) {
  g.SIGCHLDed = true;
}


void sig_winch(int signum) {
  g.received_winch = true;
  return;
}


void big_loop() {
  char           buf1[BUFFSIZE];         /* for read() from stdin */
  char           buf2[2 * BUFFSIZE + 1]; /* for read() from ptym, `+1' for adding the '\000' */
  char           *cache = buf2;
  int            nread, ncache = 0;
  struct timeval select_timeout;
  fd_set         readfds;
  int            i, r, status;
  regmatch_t     re_match[1];
  time_t         last_time = time(NULL);
  bool           given_up = false;
  int            passwords_seen = 0;
  int            fd_to_pty = -1, fd_from_pty = -1;
  bool           stdin_eof = false;
  int            exit_code = -1;
  pid_t          wait_return;

  if (g.opt.log_to_pty != NULL) {
    fd_to_pty = open(g.opt.log_to_pty, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd_to_pty < 0) {
      fatal_sys("open: %s", g.opt.log_to_pty);
    }
  }
  if (g.opt.log_from_pty != NULL) {
    fd_from_pty = open(g.opt.log_from_pty, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd_from_pty < 0) {
      fatal_sys("open: %s", g.opt.log_from_pty);
    }
  }

  /*
   * wait for the child to open the pty
   */
  do {
    /*
     * On Mac, fcntl(O_NONBLOCK) may fail before the child opens the pty
     * slave side. So wait a while for the child to open the pty slave.
     */
    fd_set writefds;

    select_timeout.tv_sec  = 1;
    select_timeout.tv_usec = 0;

    FD_ZERO(&writefds);
    FD_SET(g.fd_ptym, &writefds);

    select(g.fd_ptym + 1, NULL, &writefds, NULL, &select_timeout);
    if (!FD_ISSET(g.fd_ptym, &writefds)) {
      fatal(ERROR_GENERAL, "failed to wait for ptym to be writable");
    }
  } while (0);

  while (true) {
L_chk_sigchld:
    if (g.SIGCHLDed) {
      /*
       * NOTE:
       *  - WCONTINUED does not work on macOS (10.12.5)
       *  - On macOS, SIGCHLD can be generated when
       *     1. child process has terminated/exited
       *     2. the currently *running* child process is stopped (e.g. by `kill -STOP')
       *  - On Linux, SIGCHLD can be generated when
       *     1. child process has terminated/exited
       *     2. the currently *running* child process is stopped (e.g. by `kill -STOP')
       *     3. the currently *stopped* child process is continued (e.g. by `kill -CONT')
       *  - waitpid(WCONTINUED) works on Linux but not on macOS.
       */
      wait_return = waitpid(-1, &status, WUNTRACED | WCONTINUED);
      if (wait_return < 0) {
        fatal_sys("received SIGCHLD but waitpid() failed");
      }
      g.SIGCHLDed = false;

      if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
        goto L_done;
      } else if (WIFSIGNALED(status)) {
        exit_code = status + 128;
        goto L_done;
      } else if (WIFSTOPPED(status)) {
        /* Do nothing. Just wait for the child to be continued and wait
         * for the next SIGCHLD. */
      } else if (WIFCONTINUED(status)) {
        /* */
      } else {
        /* This should not happen. */
        goto L_done;
      }
    }

    if (g.opt.timeout != 0 && g.opt.fatal_no_prompt && passwords_seen == 0
        && labs(time(NULL) - last_time) > g.opt.timeout) {
      fatal(ERROR_TIMEOUT, "timeout waiting for password prompt");
    }

    if (g.received_winch && g.stdin_is_tty) {
      struct winsize ttysize;
      static int     ourtty = -1;

      g.received_winch = false;

      if (ourtty < 0) {
#if 0
        ourtty = open("/dev/tty", 0);
#else
        ourtty = STDIN_FILENO;
#endif
      }
      if (ioctl(ourtty, TIOCGWINSZ, &ttysize) == 0) {
        ioctl(g.fd_ptym, TIOCSWINSZ, &ttysize);
      }
    }

    /* Keep sending EOF until the child exits
     *  - See http://lists.gnu.org/archive/html/help-bash/2016-11/msg00002.html
     *    (EOF ('\004') was lost if it's sent to bash too quickly)
     *  - We cannot simply close(fd_ptym) or the child will get SIGHUP. */
    while (stdin_eof) {
      struct termios        term;
      char                  eof_char;
      static struct timeval last;
      struct timeval        now;
      double                diff;

      if (last.tv_sec == 0) {
        gettimeofday(&last, NULL);
        break;
      }

      gettimeofday(&now, NULL);
      diff = now.tv_sec + now.tv_usec / 1e6 - (last.tv_sec + last.tv_usec / 1e6);
      if (diff > -0.05 && diff < 0.05) {
        break;
      }
      last = now;

      if (tcgetattr(g.fd_ptym, &term) < 0) {
        goto L_done;
      }
      eof_char = term.c_cc[VEOF];
      if (write(g.fd_ptym, &eof_char, 1) < 0) {
        goto L_done;
      }
      write(fd_to_pty, &eof_char, 1);

      break;
    }

    FD_ZERO(&readfds);
    if (g.stdin_is_tty && !stdin_eof) {
      FD_SET(STDIN_FILENO, &readfds);
    }
    FD_SET(g.fd_ptym, &readfds);

    select_timeout.tv_sec  = 1;
    select_timeout.tv_usec = 100 * 1000;

    r = select(g.fd_ptym + 1, &readfds, NULL, NULL, &select_timeout);
    if (r == 0) {
      /* timeout */
      continue;
    } else if (r < 0) {
      if (errno == EINTR) {
        continue;
      } else {
        fatal_sys("select error");
      }
    }

    /*
     * copy data from ptym to stdout
     */
    if (FD_ISSET(g.fd_ptym, &readfds)) {
      while (true) {
        nread = read_if_ready(g.fd_ptym, cache + ncache,
                              2 * BUFFSIZE - (cache - buf2));
        if (nread <= 0) {
          /* child exited? */
          goto L_chk_sigchld;
        }

        //  write2(STDOUT_FILENO, fd_from_pty, cache + ncache, nread);
        char *sender = strdup(__FUNCTION__);
        assert(chan_send(chans[CHAN_TYPE_STDOUT], (void *)&(struct stdout_content_t){
          .content = (cache + ncache),
          .length  = nread,
          .fd1     = STDOUT_FILENO,
          .fd2     = fd_from_pty,
          .sender  = sender,
        }
                         ) == 0);
        free(sender);

        if (!given_up && g.opt.timeout != 0
            && labs(time(NULL) - last_time) >= g.opt.timeout) {
          given_up = true;
        }

        /* regexec() does not like NULLs */
        if (!given_up) {
          for (i = 0; i < nread; ++i) {
            if (cache[ncache + i] == 0) {
              cache[ncache + i] = 0xff;
            }
          }
        }
        ncache += nread;
        /* make it NULL-terminated so regexec() would be happy */
        cache[ncache] = 0;

        /* match password prompt and send the password */
        if (!g.now_interactive && !given_up) {
          if (g.opt.auto_yesno && passwords_seen == 0
              && regexec(&g.opt.re_yesno, cache, 1, re_match, 0) == 0) {
            /*
             * (yes/no)?
             */
            char *yes = "yes\r";

            write2("yes_no", g.fd_ptym, fd_to_pty, yes, strlen(yes));
            ncache -= re_match[0].rm_eo;
            cache  += re_match[0].rm_eo;
          } else if (regexec(&g.opt.re_prompt, cache, 1, re_match, 0) == 0) {
            /*
             * Password:
             */

            ++passwords_seen;

            last_time = time(NULL);

            if (g.opt.fatal_more_tries) {
              if (g.opt.tries != 0 && passwords_seen > g.opt.tries) {
                fatal(ERROR_MAX_TRIES, "still prompted for passwords after %d tries", g.opt.tries);
              }
            } else if (g.opt.tries != 0 && passwords_seen >= g.opt.tries) {
              given_up = true;
            }

            write(g.fd_ptym, g.opt.password, strlen(g.opt.password));
            write(g.fd_ptym, "\r", 1);

            write(fd_to_pty, "********\r", strlen("********\r"));

            ncache -= re_match[0].rm_eo;
            cache  += re_match[0].rm_eo;
          }
        } else {
          cache  = buf2;
          ncache = 0;
        }

        if (cache + ncache >= buf2 + 2 * BUFFSIZE) {
          if (ncache > BUFFSIZE) {
            cache += ncache - BUFFSIZE;
            ncache = BUFFSIZE;
          }
          memmove(buf2, cache, ncache);
          cache = buf2;
        }
      }
    }
    /*
     * copy data from stdin to ptym
     */
    if (!stdin_eof && FD_ISSET(STDIN_FILENO, &readfds)) {
      if ((nread = read(STDIN_FILENO, buf1, BUFFSIZE)) < 0) {
        fatal_sys("read error from stdin");
      }else if (nread == 0) {
        /* EOF on stdin means we're done */
        stdin_eof = true;
      } else {
        g.now_interactive = true;
        //write2(g.fd_ptym, fd_to_pty, buf1, nread);
        assert(chan_send(chans[CHAN_TYPE_STDOUT], (void *)&(struct stdout_content_t){
          .content = (buf1),
          .length  = nread,
          .fd1     = g.fd_ptym,
          .fd2     = fd_to_pty,
          .sender  = strdup(__FUNCTION__),
        }
                         ) == 0);
      }
    }
  }

L_done:
  /* the child has exited but there may be still some data for us
   * to read */
  while ((nread = read_if_ready(g.fd_ptym, buf2, BUFFSIZE)) > 0) {
//    write2(STDOUT_FILENO, fd_from_pty, buf2, nread);
    assert(chan_send(chans[CHAN_TYPE_STDOUT], (void *)&(struct stdout_content_t){
      .content = (buf2),
      .length  = nread,
      .fd1     = STDOUT_FILENO,
      .fd2     = fd_from_pty,
      .sender  = strdup(__FUNCTION__),
    }
                     ) == 0);
  }
  assert((chan_send(chans[CHAN_TYPE_PROCESS_DONE], (void *)NULL)) == 0);
  assert(chan_recv(chans[CHAN_TYPE_STDOUT_PROCESSORS_DONE], NULL) == 0);
  if (fd_to_pty >= 0) {
    close(fd_to_pty);
  }
  if (fd_from_pty >= 0) {
    close(fd_from_pty);
  }

  if (exit_code < 0) {
    exit(ERROR_GENERAL);
  } else {
    exit(exit_code);
  }
} /* big_loop */


int main(int argc, char *argv[]) {
  char           slave_name[32];
  pid_t          pid;
  struct termios orig_termios;
  struct winsize size;

  startup();

  getargs(argc, argv);

  g.stdin_is_tty = isatty(STDIN_FILENO);

  sig_handle(SIGCHLD, sig_child);

  if (g.stdin_is_tty) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) {
      fatal_sys("tcgetattr error on stdin");
    }
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, (char *)&size) < 0) {
      fatal_sys("TIOCGWINSZ error");
    }

    pid = pty_fork(&g.fd_ptym, slave_name, sizeof(slave_name),
                   &orig_termios, &size);
  } else {
    pid = pty_fork(&g.fd_ptym, slave_name, sizeof(slave_name),
                   NULL, NULL);
  }

  if (pid < 0) {
    fatal_sys("fork error");
  } else if (pid == 0) {
    /*
     * child
     */
    if (g.opt.nohup_child) {
      sig_handle(SIGHUP, SIG_IGN);
    }
    if (execvp(g.opt.command[0], g.opt.command) < 0) {
      fatal_sys("can't execute: %s", g.opt.command[0]);
    }
  }

  /*
   * parent
   */

  /* stdout also needs to be checked. Or `passh ls -l | less' would not
   * restore the saved tty settings. */
  if (g.stdin_is_tty && isatty(STDOUT_FILENO)) {
    /* user's tty to raw mode */
    if (tty_raw(STDIN_FILENO, &g.save_termios) < 0) {
      fatal_sys("tty_raw error");
    }

    /* reset user's tty on exit */
    g.reset_on_exit = true;
    if (atexit(tty_atexit) < 0) {
      fatal_sys("atexit error");
    }

    sig_handle(SIGWINCH, sig_winch);
  }

  big_loop();

  return(0);
} /* main */

/* vi:set ts=8 sw=4 sta et: */
