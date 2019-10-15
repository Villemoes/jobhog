/*
 * jobhog command -j#### args
 *
 * - parse MAKEFLAGS, looking for --jobserver-auth=R,W (or
 *   --jobserver-fds=R,W)
 *
 * - read as many tokens as possible from the jobserver pipe (13, in
 *   the example below)
 *
 * - exec into "command -j14 args". For now, jobhog itself does not
 *   accept options, so we simply execvp(&argv[1]), after replacing
 *   the first occurrence of ### in the following arguments by the
 *   appropriate decimal integer.
 *
 * - when "command" finishes, write back 13 tokens to the jobserver pipe
 *
 * We exec into command to simplify the need for us to report the
 * right termination status to our parent. However, that of course
 * makes it slightly more difficult to do anything after command
 * finishes. We fix that by creating a pipe, forking off a child, and
 * letting command inherit the write end of that pipe. The child
 * simply waits for EOF on the read end.
 *
 * We always delete MAKEFLAGS from the environment instead of trying
 * to just delete the --jobserver- part. "command" is unlikely to be
 * "make" (because then why would you use this). Whatever tree of
 * processes "command" ends up spawning, it should be treated as an
 * independent parallel build process that just happens to have 14 job
 * slots.
 */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sysexits.h>

static int rfd = -1, wfd = -1;

static int
parse_env(void)
{
	const char *env = getenv("MAKEFLAGS");
	const char *s;

	if (!env)
		return -1;
	s = strstr(env, "--jobserver-auth=");
	if (!s)
		s = strstr(env, "--jobserver-fds=");
	if (!s)
		return -1;
	s = strchr(s, '=');
	assert(s);
	if (sscanf(s, "=%d,%d", &rfd, &wfd) == 2)
		return 0;
	return -1;
}

static void
put_tokens(unsigned int tokens)
{
	static const char t[8] = "++++++++";

	while (tokens) {
		int w = tokens > sizeof(t) ? sizeof(t) : tokens;
		int r = write(wfd, t, w);
		if (r < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			return;
		}
		tokens -= r;
	}
}

static int
create_child(unsigned int tokens)
{
	int pipefd[2];
	pid_t child;

	if (pipe(pipefd) < 0) {
		put_tokens(tokens);
		return 0;
	}
	child = fork();
	if (child < 0) {
		(void) close(pipefd[0]);
		(void) close(pipefd[1]);
		put_tokens(tokens);
		return 0;
	}
	if (child == 0) {
		char d;

		(void) close(pipefd[1]);
		while (read(pipefd[0], &d, 1) < 0)
			;
		put_tokens(tokens);
		_exit(EX_OK);
	}

	(void) close(pipefd[0]);
	return tokens;
}

static unsigned int
get_tokens(void)
{
	unsigned int tokens = 0;
	int fd = -1;
	int flags, r;
	char buf[8];

	if (parse_env() < 0)
		return 0;
	flags = fcntl(rfd, F_GETFL);
	if (flags < 0) /* must be EBADF */
		return 0;
	if (!(flags & O_NONBLOCK)) {
		/*
		 * Not all versions/configurations of make use a
		 * non-blocking read end. To make this simpler, we
		 * want an O_NONBLOCK file, but we can't change the
		 * flags of the inherited fd, since that would affect
		 * our parent process as well as everybody else who
		 * inherited that open file description. So attempt to
		 * re-open the pipe via /proc/self/fd, so we can get
		 * our own open file description (struct file in
		 * kernel-speak) with O_NONBLOCK set.
		 */
		char path[sizeof("/proc/self/fd/") + 16];
		snprintf(path, sizeof(path), "/proc/self/fd/%d", rfd);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			return 0;
	} else {
		fd = rfd;
	}
	/* arbitrary sanity limit, ensures the number fits in ### */
	while (tokens < 500) {
		r = read(fd, buf, sizeof(buf));
		/*
		 * Break both on -EWOULDBLOCK, but also unexpected EOF
		 * (which shouldn't happen since we've inherited the
		 * write end as well).
		 */
		if (r <= 0)
			break;
		tokens += r;
	}
	if (fd != rfd)
		(void) close(fd);
	if (tokens)
		tokens = create_child(tokens);
	return tokens;
}

static char *
find_hashes(char **argv)
{
	for (; *argv; ++argv) {
		char *h = strstr(*argv, "###");
		if (h)
			return h;
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	unsigned int tokens;
	unsigned int len, p;
	char buf[4];
	char *h;

	if (argc < 2)
		error(EX_USAGE, 0, "missing command");
	if (argc < 3 || !(h = find_hashes(&argv[2])))
		error(EX_USAGE, 0, "missing ### argument");

	tokens = get_tokens();

	assert(tokens < 999);
	assert(strncmp(h, "###", strlen("###")) == 0);

	p = snprintf(buf, sizeof(buf), "%u", tokens + 1);
	assert(p <= 3);
	len = strlen(h) + 1;
	assert(len >= 4);
	/* Write the decimal number over the ### */
	memcpy(h, buf, p);
	/*
	 * Move the rest of argv[i], including trailing nul. This eats
	 * leftover #s if any.
	 */
	memmove(h + p, h + 3, len - 3);

	unsetenv("MAKEFLAGS");
	if (rfd >= 0)
		(void) close(rfd);
	if (wfd >= 0)
		(void) close(wfd);
	execvp(argv[1], &argv[1]);
	error(EX_OSERR, errno, "execvp(%s) failed", argv[1]);

	return 0;
}
