#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <git2.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_MS 30

#define COLOR_DEFAULT "\\[\\e[39m\\]"
#define COLOR_ACCENT "\\[\\e[33m\\]"
#define COLOR_DANGER "\\[\\e[38;2;244;56;65m\\]"
#define COLOR_RESET "\\[\\e[0m\\]"

typedef struct
{
  char* data;
  size_t len;
  size_t cap;
} Buffer;

static Buffer prompt;

typedef struct
{
  size_t conflicted;
  size_t deleted;
  size_t renamed;
  size_t modified;
  size_t staged;
  size_t untracked;
} StatusCounts;

static void write_all(int fd, const char* data, size_t length);

static void
buffer_free(Buffer* buf)
{
  free(buf->data);
  memset(buf, 0, sizeof(*buf));
}

static int
buffer_reserve(Buffer* buf, size_t extra)
{
  size_t needed;
  size_t cap;
  char* data;

  if (extra > SIZE_MAX - buf->len - 1) return -1;
  needed = buf->len + extra + 1;
  if (needed <= buf->cap) return 0;

  cap = buf->cap ? buf->cap : 128;
  while (cap < needed)
  {
    if (cap > SIZE_MAX / 2)
    {
      cap = needed;
      break;
    }
    cap *= 2;
  }
  data = realloc(buf->data, cap);
  if (!data) return -1;
  buf->data = data;
  buf->cap = cap;
  return 0;
}

static int
buffer_append_n(Buffer* buf, const char* text, size_t len)
{
  if (buffer_reserve(buf, len) < 0) return -1;
  memcpy(buf->data + buf->len, text, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
 return 0;
}

static int
buffer_append(Buffer* buf, const char* text)
{
  return buffer_append_n(buf, text, strlen(text));
}

static int
buffer_printf(Buffer* buf, const char* format, ...)
{
  va_list args;
  va_list copy;
  int length;

  va_start(args, format);
  va_copy(copy, args);
  length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (length < 0 || buffer_reserve(buf, (size_t)length) < 0)
  {
    va_end(args);
    return -1;
  }
  vsnprintf(buf->data + buf->len, buf->cap - buf->len, format, args);
  va_end(args);
  buf->len += (size_t)length;
  return 0;
}

static int
append_ps1_escaped_n(Buffer* buf, const char* text, size_t len)
{
  const char* end = text + len;

  for (; text < end; text++)
  {
    if (*text == '\\' || *text == '$' || *text == '`')
    {
      if (buffer_append_n(buf, "\\", 1) < 0) return -1;
    }
    if (buffer_append_n(buf, text, 1) < 0) return -1;
  }
  return 0;
}

static int
append_ps1_escaped(Buffer* buf, const char* text)
{
  return append_ps1_escaped_n(buf, text, strlen(text));
}

static char*
current_directory(void)
{
  const char* pwd = getenv("PWD");

  if (pwd && pwd[0] == '/') return strdup(pwd);
  return getcwd(NULL, 0);
}

static int
append_short_pwd(Buffer* out, const char* pwd)
{
  const char* home = getenv("HOME");
  const char* path = pwd;
  const char* part;
  bool home_prefix = false;

  if (home && home[0] && strcmp(home, "/") != 0 &&
      (strcmp(path, home) == 0 || (strncmp(path, home, strlen(home)) == 0 && path[strlen(home)] == '/')))
  { /* does pwd contain home */
    home_prefix = true;
    path += strlen(home);
    if (buffer_append(out, "~") < 0) return -1;
  }

  while (*path == '/') path++;
  part = path;
  while (*part)
  {
    const char* slash = strchr(part, '/');
    size_t len = slash ? (size_t)(slash - part) : strlen(part);
    bool last = !slash || slash[1] == '\0';

    if (len)
    {
      if (buffer_append(out, "/") < 0) return -1;
      if (last)
      {
        if (append_ps1_escaped_n(out, part, len) < 0) return -1;
        break;
      }
      len = part[0] == '.' && len > 1 ? 2 : 1;
      if (append_ps1_escaped_n(out, part, len) < 0) return -1;
    }
    part = slash + 1;
    while (*part == '/') part++;
  }

  if (!out->len && buffer_append(out, "/") < 0) return -1;
  if (home_prefix && !*path) return 0;
  return 0;
}

static bool
env_enabled(const char* name)
{
  const char* value = getenv(name);

  return value && value[0] && strcmp(value, "0") != 0 &&
         strcasecmp(value, "false") != 0 && strcasecmp(value, "no") != 0;
}

static int
timeout_ms(void)
{
  const char* value = getenv("PROMPT_NATIVE_TIMEOUT_MS");
  char* end;
  long parsed;

  if (!value || !*value) return DEFAULT_TIMEOUT_MS;
  errno = 0;
  parsed = strtol(value, &end, 10);
  if (errno || *end || parsed < 0 || parsed > INT_MAX)
    return DEFAULT_TIMEOUT_MS;
  return (int)parsed;
}

static int
stash_counter(size_t index, const char* message, const git_oid* oid,
              void* payload)
{
  size_t* count = payload;

  (void)index;
  (void)message;
  (void)oid;
  (*count)++;
  return 0;
}

static int
collect_status(git_repository* repo, StatusCounts* counts)
{
  git_status_options options = {0};
  git_status_list* list = NULL;
  size_t i;

  options.version = GIT_STATUS_OPTIONS_VERSION;
  options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                  GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                  GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
  if (git_status_list_new(&list, repo, &options) < 0) return -1;

  for (i = 0; i < git_status_list_entrycount(list); i++)
  {
    const git_status_entry* entry = git_status_byindex(list, i);
    unsigned int status = entry->status;

    if (status & GIT_STATUS_CONFLICTED)
    {
      counts->conflicted++;
      continue;
    }
    if (status & GIT_STATUS_INDEX_DELETED) counts->deleted++;
    if (status & GIT_STATUS_WT_DELETED) counts->deleted++;
    if (status & GIT_STATUS_INDEX_RENAMED) counts->renamed++;
    if (status & GIT_STATUS_WT_RENAMED) counts->renamed++;
    if (status & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED))
      counts->staged++;
    if (status & GIT_STATUS_WT_MODIFIED) counts->modified++;
    if (status & GIT_STATUS_WT_NEW) counts->untracked++;
  }

  git_status_list_free(list);
  return 0;
}

static int
append_divergence(Buffer* out, git_repository* repo, git_reference* head)
{
  git_reference* upstream = NULL;
  const git_oid* local_oid = git_reference_target(head);
  const git_oid* upstream_oid;
  size_t ahead = 0;
  size_t behind = 0;

  if (!git_reference_is_branch(head) || !local_oid ||
      git_branch_upstream(&upstream, head) < 0)
    return 0;
  upstream_oid = git_reference_target(upstream);
  if (upstream_oid)
    git_graph_ahead_behind(&ahead, &behind, repo, local_oid, upstream_oid);
  git_reference_free(upstream);

  if (ahead && behind) return buffer_printf(out, "⇕⇡%zu⇣%zu", ahead, behind);
  if (ahead) return buffer_printf(out, "⇡%zu", ahead);
  if (behind) return buffer_printf(out, "⇣%zu", behind);
  return 0;
}

static int
write_git_segment(int fd, const char* pwd)
{
  git_buf discovered = GIT_BUF_INIT;
  git_repository* repo = NULL;
  git_reference* head = NULL;
  git_reference* head_symbolic = NULL;
  StatusCounts counts = {0};
  Buffer state = {0};
  const char* branch = NULL;
  char detached_oid[8] = {0};
  size_t stash_count = 0;
  bool detached = false;
  int across_fs = !env_enabled("PROMPT_GIT_ONE_FILESYSTEM");
  int result = -1;

  if (git_repository_discover(&discovered, pwd, across_fs, NULL) < 0 ||
      git_repository_open(&repo, discovered.ptr) < 0)
    goto done;

  if (git_repository_head(&head, repo) == 0)
  {
    detached = git_repository_head_detached(repo) == 1;
    if (detached)
    {
      const git_oid* oid = git_reference_target(head);
      branch = "HEAD";
      if (oid) git_oid_tostr(detached_oid, sizeof(detached_oid), oid);
    }
    else
    {
      branch = git_reference_shorthand(head);
    }
  }
  else if (git_reference_lookup(&head_symbolic, repo, "HEAD") == 0)
  {
    const char* target = git_reference_symbolic_target(head_symbolic);
    const char* prefix = "refs/heads/";

    if (target && strncmp(target, prefix, strlen(prefix)) == 0)
      branch = target + strlen(prefix);
  }
  if (!branch) goto done;

  if (buffer_append(&state, COLOR_DEFAULT "git://" COLOR_ACCENT) < 0 ||
      append_ps1_escaped(&state, branch) < 0)
    goto done;
  if (detached_oid[0] &&
      buffer_printf(&state, COLOR_DEFAULT " " COLOR_DANGER "%s",
                    detached_oid) < 0)
    goto done;

  /* Everything after this checkpoint is optional if status collection times out. */
  if (buffer_append_n(&state, "\0", 1) < 0) goto done;
  write_all(fd, state.data, state.len);
  result = 0;
  state.len = 0;
  if (state.data) state.data[0] = '\0';

  if (collect_status(repo, &counts) < 0) goto done;

  git_stash_foreach(repo, stash_counter, &stash_count);
  if (counts.conflicted && buffer_printf(&state, "=%zu", counts.conflicted) < 0) goto done;
  if (stash_count && buffer_printf(&state, "*%zu", stash_count) < 0) goto done;
  if (counts.deleted && buffer_printf(&state, "x%zu", counts.deleted) < 0) goto done;
  if (counts.renamed && buffer_printf(&state, "»%zu", counts.renamed) < 0) goto done;
  if (counts.modified && buffer_printf(&state, "!%zu", counts.modified) < 0) goto done;
  if (counts.staged && buffer_printf(&state, "+%zu", counts.staged) < 0) goto done;
  if (counts.untracked && buffer_printf(&state, "?%zu", counts.untracked) < 0) goto done;
  if (head && append_divergence(&state, repo, head) < 0) goto done;
  if (state.len)
  {
    write_all(fd, COLOR_DANGER " ", strlen(COLOR_DANGER " "));
    write_all(fd, state.data, state.len);
  }

done:
  buffer_free(&state);
  git_reference_free(head_symbolic);
  git_reference_free(head);
  git_repository_free(repo);
  git_buf_dispose(&discovered);
  return result;
}

static bool
has_svn_metadata(const char* pwd)
{
  char* path = strdup(pwd);
  struct stat info;
  bool found = false;

  if (!path) return false;
  for (;;)
  {
    Buffer metadata = {0};
    char* slash;

    if (buffer_printf(&metadata, "%s/.svn", path) == 0 &&
        stat(metadata.data, &info) == 0 && S_ISDIR(info.st_mode))
      found = true;
    buffer_free(&metadata);
    if (found || strcmp(path, "/") == 0) break;
    slash = strrchr(path, '/');
    if (!slash) break;
    if (slash == path)
      path[1] = '\0';
    else
      *slash = '\0';
  }
  free(path);
  return found;
}

static int
svn_segment(Buffer* out, const char* pwd)
{
  int pipefd[2];
  pid_t child;
  Buffer output = {0};
  char chunk[1024];
  ssize_t length;
  int status;
  int result = -1;

  if (!has_svn_metadata(pwd) || pipe2(pipefd, O_CLOEXEC) < 0) return -1;
  child = fork();
  if (child < 0)
  {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (child == 0)
  {
    int nullfd;

    if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
    close(pipefd[0]);
    close(pipefd[1]);
    nullfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nullfd >= 0)
    {
      dup2(nullfd, STDERR_FILENO);
      close(nullfd);
    }
    execlp("svn", "svn", "info", "--non-interactive", "--show-item",
           "relative-url", pwd, (char*)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  while ((length = read(pipefd[0], chunk, sizeof(chunk))) > 0)
  {
    if (buffer_append_n(&output, chunk, (size_t)length) < 0) break;
  }
  close(pipefd[0]);
  if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
      WEXITSTATUS(status))
    goto done;
  while (output.len && (output.data[output.len - 1] == '\n' ||
                        output.data[output.len - 1] == '\r'))
    output.data[--output.len] = '\0';
  if (output.len >= 2 && output.data[0] == '^' && output.data[1] == '/')
  {
    memmove(output.data, output.data + 2, output.len - 1);
    output.len -= 2;
  }
  if (!output.len ||
      buffer_append(out, COLOR_DEFAULT "svn://" COLOR_ACCENT) < 0 ||
      append_ps1_escaped(out, output.data) < 0)
    goto done;
  result = 0;

done:
  buffer_free(&output);
  return result;
}

static void
write_all(int fd, const char* data, size_t length)
{
  while (length)
  {
    ssize_t written = write(fd, data, length);

    if (written < 0)
    {
      if (errno == EINTR) continue;
      return;
    }
    data += written;
    length -= (size_t)written;
  }
}

static void
vcs_worker(int fd, const char* pwd)
{
  Buffer segment = {0};

  if (git_libgit2_init() < 0)
  {
    write_all(fd, "\0", 1);
    goto done;
  }
  if (write_git_segment(fd, pwd) < 0)
  {
    write_all(fd, "\0", 1);
    if (svn_segment(&segment, pwd) < 0) buffer_free(&segment);
    write_all(fd, segment.data, segment.len);
  }
  git_libgit2_shutdown();

done:
  buffer_free(&segment);
}

static int64_t
monotonic_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void
collect_vcs(Buffer* segment, const char* pwd, int deadline_ms)
{
  int pipefd[2];
  pid_t child;
  int64_t deadline;
  bool complete = false;
  bool timed_out = false;
  size_t branch_end = SIZE_MAX;
  char chunk[1024];

  if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) return;
  child = fork();
  if (child < 0)
  {
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }
  if (child == 0)
  { /* child job */
    close(pipefd[0]);
    setpgid(0, 0);
    fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL) & ~O_NONBLOCK);
    vcs_worker(pipefd[1], pwd);
    close(pipefd[1]);
    _exit(0);
  }
  /* parent job */
  close(pipefd[1]);
  setpgid(child, child);
  deadline = 0;

  while (!complete)
  {
    struct pollfd descriptor = {.fd = pipefd[0], .events = POLLIN | POLLHUP};
    int64_t remaining = deadline ? deadline - monotonic_ms() : -1;
    int ready;

    if (deadline && remaining <= 0)
    {
      timed_out = true;
      break;
    }
    ready = poll(&descriptor, 1, deadline ? (int)remaining : -1);
    if (ready < 0)
    {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0)
    {
      timed_out = true;
      break;
    }
    for (;;)
    {
      ssize_t length = read(pipefd[0], chunk, sizeof(chunk));

      if (length > 0)
      {
        char* marker = memchr(chunk, '\0', (size_t)length);

        if (marker && branch_end == SIZE_MAX)
        {
          branch_end = segment->len + (size_t)(marker - chunk);
          if (deadline_ms) deadline = monotonic_ms() + deadline_ms;
        }
        if (buffer_append_n(segment, chunk, (size_t)length) < 0)
        {
          buffer_free(segment);
          branch_end = SIZE_MAX;
          complete = true;
          break;
        }
        continue;
      }
      if (length == 0)
        complete = true;
      else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        complete = true;
      break;
    }
  }
  close(pipefd[0]);
  if (timed_out || !complete)
  {
    if (branch_end == SIZE_MAX)
      buffer_free(segment);
    else
    {
      segment->len = branch_end;
      segment->data[segment->len] = '\0';
    }
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
  }
  else if (branch_end != SIZE_MAX)
  {
    memmove(segment->data + branch_end, segment->data + branch_end + 1,
            segment->len - branch_end - 1);
    segment->len--;
    segment->data[segment->len] = '\0';
  }
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR);
}

static int
print_prompt(const char* pwd, int last_status)
{
  Buffer vcs = {0};
  int result = EXIT_FAILURE;

  if (buffer_append(&prompt,
                    COLOR_DEFAULT "┌[\\u@\\h" COLOR_DEFAULT "]-(" COLOR_ACCENT) < 0 ||
      append_short_pwd(&prompt, pwd) < 0 ||
      buffer_append(&prompt, COLOR_DEFAULT ")") < 0)
    goto done;
  collect_vcs(&vcs, pwd, timeout_ms());
  if (vcs.len && (buffer_append(&prompt, COLOR_DEFAULT "-[") < 0 ||
                  buffer_append_n(&prompt, vcs.data, vcs.len) < 0 ||
                  buffer_append(&prompt, COLOR_DEFAULT "]") < 0))
    goto done;
  if (last_status != 0 &&
      buffer_printf(&prompt,
                    COLOR_DEFAULT "(" COLOR_DANGER "%d" COLOR_DEFAULT ")",
                    last_status) < 0)
    goto done;
  if (buffer_append(&prompt, last_status ? "\n" COLOR_DEFAULT "└" COLOR_DANGER
                                           "> " COLOR_RESET
                                         : "\n" COLOR_DEFAULT "└" COLOR_DEFAULT
                                           "> " COLOR_RESET) < 0)
    goto done;
  if (fwrite(prompt.data, 1, prompt.len, stdout) != prompt.len) goto done;
  result = EXIT_SUCCESS;

done:
  buffer_free(&prompt);
  buffer_free(&vcs);
  return result;
}

static void
usage(FILE* stream, const char* program)
{
  fprintf(stream, "usage: %s [last-status]\n", program);
}

int
main(int argc, char** argv)
{
  char* pwd;
  char* end;
  long status = 0;
  int result;

  if (argc > 2)
  {
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }
  if (argc == 2)
  {
    errno = 0;
    status = strtol(argv[1], &end, 10);
    if (errno || *end || status < 0 || status > 255)
    {
      usage(stderr, argv[0]);
      return EXIT_FAILURE;
    }
  }
  pwd = current_directory();
  if (!pwd)
  {
    perror("prompt-native: current directory");
    return EXIT_FAILURE;
  }
  result = print_prompt(pwd, (int)status);
  free(pwd);
  return result;
}
