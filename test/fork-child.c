/*
 * Copyright (c) 2026 Lubos Dolezel
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Guest regression: a kqueue is not inherited by a child process (see
 * kqueue(2)), so whatever the child does with it must not affect the
 * parent. After a child forks and exits, the parent's registrations must
 * still deliver events.
 *
 * Build it against Darling's Darwin headers and run it inside Darling. It
 * prints "PASS: registrations survive a fork child" and exits with 0.
 * With the bug, the child's teardown removed the parent's registrations,
 * and the first wait timed out.
 */
#include <sys/event.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef NDEBUG
#error Assertions required
#endif

static void expect_read(int kq, int fd)
{
	struct kevent ev;
	struct timespec timeout = { 2, 0 };
	int n = kevent(kq, NULL, 0, &ev, 1, &timeout);
	printf("event count=%d\n", n);
	assert(n == 1);
	assert(ev.filter == EVFILT_READ && (int)ev.ident == fd);
}

static void fork_and_reap(void)
{
	int status;
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0)
		_exit(0);
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
	int p[2];
	char c;
	struct kevent change;

	alarm(30);
	assert(pipe(p) == 0);
	int kq = kqueue();
	assert(kq >= 0);
	EV_SET(&change, p[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
	assert(kevent(kq, &change, 1, NULL, 0, NULL) == 0);

	/* A child that exits right away. */
	fork_and_reap();
	assert(write(p[1], "x", 1) == 1);
	expect_read(kq, p[0]);
	assert(read(p[0], &c, 1) == 1);

	/* A child that uses its copy of the descriptor before it exits. */
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct kevent ev;
		struct timespec zero = { 0, 0 };
		(void)kevent(kq, NULL, 0, &ev, 1, &zero);
		(void)close(kq);
		_exit(0);
	}
	int status;
	assert(waitpid(pid, &status, 0) == pid);
	assert(write(p[1], "y", 1) == 1);
	expect_read(kq, p[0]);
	assert(read(p[0], &c, 1) == 1);

	puts("PASS: registrations survive a fork child");
	return 0;
}
