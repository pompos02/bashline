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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_MS 500

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

/* global prompt state */
typedef struct
{
  Buffer prompt;
  git_repository* repo;
  git_reference* head;
  char* pwd;
  bool git_initialized;
} State;

static State g_state = {0};

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

static void
buffer_reserve(Buffer* buf, size_t extra)
{
  size_t needed;
  size_t cap;
  char* data;

  if (extra > SIZE_MAX - buf->len - 1) exit(EXIT_FAILURE);
  needed = buf->len + extra + 1;
  if (needed <= buf->cap) return;

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
  if (!data) exit(EXIT_FAILURE);
  buf->data = data;
  buf->cap = cap;
}

static void
buffer_append_n(Buffer* buf, const char* text, size_t len)
{
  if (!len) return;
  buffer_reserve(buf, len);
  memcpy(buf->data + buf->len, text, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
}

static void
buffer_append(Buffer* buf, const char* text)
{
  buffer_append_n(buf, text, strlen(text));
}

static void
buffer_printf(Buffer* buf, const char* format, ...)
{
  va_list args;
  va_list copy;
  int length;

  va_start(args, format);
  va_copy(copy, args);
  length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (length < 0) exit(EXIT_FAILURE);
  buffer_reserve(buf, (size_t)length);
  vsnprintf(buf->data + buf->len, buf->cap - buf->len, format, args);
  va_end(args);
  buf->len += (size_t)length;
}

static void
append_ps1_escaped(Buffer* buf, const char* text)
{
  for (; *text; text++)
  {
    if (*text == '\\' || *text == '$' || *text == '`')
      buffer_append_n(buf, "\\", 1);
    buffer_append_n(buf, text, 1);
  }
}

static void
append_ps1_escaped_n(Buffer* buf, const char* text, size_t n)
{
  for (size_t i = 0; i < n && text[i]; ++i)
  {
    if (text[i] == '\\' || text[i] == '$' || text[i] == '`')
      buffer_append_n(buf, "\\", 1);
    buffer_append_n(buf, &text[i], 1);
  }
}


static char*
current_directory(void)
{
  const char* pwd = getenv("PWD");

  if (pwd && pwd[0] == '/') return strdup(pwd);
  return getcwd(NULL, 0);
}

static void
append_short_pwd()
{
  const char* path = g_state.pwd;
  const char* home = getenv("HOME");
  const char* part;
  if (home && home[0] && strcmp(home, "/") != 0 &&
      (strcmp(g_state.pwd, home) == 0 || (strncmp(g_state.pwd, home, strlen(home)) == 0 && g_state.pwd[strlen(home)] == '/')))
  { /* pwd contains home */
    path += strlen(home);
    buffer_append(&g_state.prompt, "~");
    if (*path == '\0') return; // exactly at home return
  }

  while (*path == '/') path++;
  if (*path == '\0') { buffer_append(&g_state.prompt, "/"); return; }
  part = path;
  while (*part)
  {
    size_t len = strcspn(part, "/");
    const char* next = part + len;

    while (*next == '/') next++;

    buffer_append(&g_state.prompt, "/");

    if (*next == '\0')
    { /* append the final full dir */
      append_ps1_escaped_n(&g_state.prompt, part, len);
      break;
    }

    size_t shown = part[0] == '.' && len > 1 ? 2 : 1;
    append_ps1_escaped_n(&g_state.prompt, part, shown);
    part = next;
  }
}

static int
timeout_ms(void)
{
  const char* value = getenv("PROMPT_TIMEOUT_MS");
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
stash_counter(size_t index, const char* message, const git_oid* oid, void* payload)
{ /* we only care about the count of the stashes, cast to void the unused */
  size_t* count = payload;
  (void)index; (void)message; (void)oid;
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
  options.show    = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
  options.flags   = GIT_STATUS_OPT_INCLUDE_UNTRACKED      |
                    GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX  |
                    GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
  if (git_status_list_new(&list, repo, &options) < 0) return -1;

  for (i = 0; i < git_status_list_entrycount(list); i++)
  {
    const git_status_entry* entry = git_status_byindex(list, i);
    unsigned int status = entry->status;

    if (status & GIT_STATUS_CONFLICTED) { counts->conflicted++; continue; }
    if (status & GIT_STATUS_INDEX_DELETED) counts->deleted++;
    if (status & GIT_STATUS_WT_DELETED) counts->deleted++;
    if (status & GIT_STATUS_INDEX_RENAMED) counts->renamed++;
    if (status & GIT_STATUS_WT_RENAMED) counts->renamed++;
    if (status & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_TYPECHANGE)) counts->staged++;
    if (status & (GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_TYPECHANGE)) counts->modified++;
    if (status & GIT_STATUS_WT_NEW) counts->untracked++;
  }

  git_status_list_free(list);
  return 0;
}

static void
append_divergence(Buffer* out, git_repository* repo, git_reference* head)
{
  git_reference* upstream = NULL;
  const git_oid* local_oid = git_reference_target(head);
  const git_oid* upstream_oid;
  size_t ahead = 0;
  size_t behind = 0;

  if (!git_reference_is_branch(head) || !local_oid || git_branch_upstream(&upstream, head) < 0)
    return;
  upstream_oid = git_reference_target(upstream);
  if (upstream_oid) git_graph_ahead_behind(&ahead, &behind, repo, local_oid, upstream_oid);
  git_reference_free(upstream);

  if (ahead && behind)  buffer_printf(out, "⇕⇡%zu⇣%zu", ahead, behind);
  else if (ahead)       buffer_printf(out, "⇡%zu", ahead);
  else if (behind)      buffer_printf(out, "⇣%zu", behind);
}

static bool
open_repository(void)
{
  git_buf discovered = GIT_BUF_INIT;
  bool opened;

  opened = git_repository_discover(&discovered, g_state.pwd, 0, NULL) == 0 && git_repository_open(&g_state.repo, discovered.ptr) == 0;
  git_buf_dispose(&discovered);
  return opened;
}

static bool
state_init(void)
{
  g_state.pwd = current_directory();
  if (!g_state.pwd) return false;

  if (git_libgit2_init() < 0) return true;
  g_state.git_initialized = true;

  if (open_repository()) git_repository_head(&g_state.head, g_state.repo);
  return true;
}

static void
state_destroy(void)
{
  git_reference_free(g_state.head);
  git_repository_free(g_state.repo);
  if (g_state.git_initialized) git_libgit2_shutdown();
  free(g_state.pwd);
  buffer_free(&g_state.prompt);
  memset(&g_state, 0, sizeof(g_state));
}

static bool
append_git_branch(void)
{
  git_reference* head_symbolic = NULL;
  const char* branch = NULL;
  char detached_oid[8] = {0};
  bool found = false;

  if (g_state.head)
  { /* HEAD resolves to a commit or is detached to a commit */
    if (git_repository_head_detached(g_state.repo) == 1)
    {
      const git_oid* oid = git_reference_target(g_state.head);
      branch = "HEAD";
      if (oid) git_oid_tostr(detached_oid, sizeof(detached_oid), oid);
    }
    else
    {
      branch = git_reference_shorthand(g_state.head);
    }
  }
  else if (git_reference_lookup(&head_symbolic, g_state.repo, "HEAD") == 0)
  { /* HEAD maybe a branch without a commit */
    const char* target = git_reference_symbolic_target(head_symbolic);
    const char* prefix = "refs/heads/";
    if (target && strncmp(target, prefix, strlen(prefix)) == 0)
      branch = target + strlen(prefix);
  }
  if (branch)
  {
    buffer_append(&g_state.prompt, COLOR_DEFAULT "-[git://" COLOR_ACCENT);
    append_ps1_escaped(&g_state.prompt, branch);
    if (detached_oid[0]) buffer_printf(&g_state.prompt, COLOR_DEFAULT " " COLOR_DANGER "%s", detached_oid);
    found = true;
  }
  git_reference_free(head_symbolic);
  return found;
}

static void
append_count(Buffer* out, const char* symbol, size_t count)
{
  if (count) buffer_printf(out, "%s%zu", symbol, count);
}

static void
write_git_details(int fd)
{ /* this function runs on a child process */
  StatusCounts counts = {0};
  Buffer state = {0};
  size_t stash_count = 0;
  size_t prefix_length;

  if (collect_status(g_state.repo, &counts) < 0) return;
  git_stash_foreach(g_state.repo, stash_counter, &stash_count);

  buffer_append(&state, COLOR_DANGER " ");
  prefix_length = state.len;
  append_count(&state, "=", counts.conflicted);
  append_count(&state, "*", stash_count);
  append_count(&state, "x", counts.deleted);
  append_count(&state, "»", counts.renamed);
  append_count(&state, "!", counts.modified);
  append_count(&state, "+", counts.staged);
  append_count(&state, "?", counts.untracked);
  if (g_state.head) append_divergence(&state, g_state.repo, g_state.head);

  if (state.len > prefix_length) write_all(fd, state.data, state.len);

  buffer_free(&state);
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

static int64_t
monotonic_ms(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void
collect_git_details(int timeout)
{
  int pipefd[2];
  pid_t child;
  int64_t deadline;
  size_t prompt_start = g_state.prompt.len;
  bool complete = false;
  char chunk[1024];
  int ready = 0;

  if (pipe2(pipefd, O_CLOEXEC) < 0) return;
  child = fork();
  if (child < 0) { close(pipefd[0]); close(pipefd[1]); return; }
  if (child == 0)
  {
    close(pipefd[0]);
    write_git_details(pipefd[1]);
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);
  deadline = timeout ? monotonic_ms() + timeout : 0;

  for (;;)
  { /* wait for the child output */
    struct pollfd descriptor = {.fd = pipefd[0], .events = POLLIN | POLLHUP};
    int64_t remaining = deadline ? deadline - monotonic_ms() : -1;

    if (deadline && remaining <= 0) break;
    ready = poll(&descriptor, 1, deadline ? (int)remaining : -1);
    if (ready >= 0 || errno != EINTR) break;
  }
  if (ready > 0)
  { /* the child process produced an output */
    for (;;)
    { /* read the output of the child */
      ssize_t length = read(pipefd[0], chunk, sizeof(chunk));
      if (length > 0) buffer_append_n(&g_state.prompt, chunk, (size_t)length);
      else if (length == 0) { complete = true; break; }
      else if (errno != EINTR) break;
    }
  }
  close(pipefd[0]);
  if (!complete)
  {
    g_state.prompt.len = prompt_start;
    g_state.prompt.data[g_state.prompt.len] = '\0';
    buffer_append(&g_state.prompt, "[!!]");
    kill(child, SIGKILL);
  }
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR);
}

static int
print_prompt(int last_status)
{
  int result;

  /* append user, hostname and pwd */
  buffer_append(&g_state.prompt, COLOR_DEFAULT "┌[\\u@\\h]-(" COLOR_ACCENT); append_short_pwd(); buffer_append(&g_state.prompt, COLOR_DEFAULT ")");
  if (g_state.repo && append_git_branch())
  {
    collect_git_details(timeout_ms());
    buffer_append(&g_state.prompt, COLOR_DEFAULT "]");
  }
  if (last_status != 0)
    buffer_printf(&g_state.prompt, COLOR_DEFAULT "(" COLOR_DANGER "%d" COLOR_DEFAULT ")", last_status);
  buffer_append(&g_state.prompt, "\n" COLOR_DEFAULT "└");
  if (last_status != 0) buffer_append(&g_state.prompt, COLOR_DANGER);
  buffer_append(&g_state.prompt, "> " COLOR_RESET);
  result = fwrite(g_state.prompt.data, 1, g_state.prompt.len, stdout) == g_state.prompt.len;
  return result ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main(int argc, char** argv)
{
  char* end;
  long status = 0;
  int result;

  if (argc > 2) return 68;
  if (argc == 2)
  {
    errno = 0;
    status = strtol(argv[1], &end, 10);
    if (errno || *end || status < 0 || status > 255) return 68;
  }
  if (!state_init())
  {
    perror("prompt: current directory");
    return EXIT_FAILURE;
  }
  result = print_prompt((int)status);
  state_destroy();
  return result;
}
