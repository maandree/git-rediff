/* See LICENSE file for copyright and license details. */
#ifdef __linux__
# include <linux/prctl.h>
# include <sys/prctl.h>
#endif
#include <libsimple.h>
#include <libsimple-arg.h>
#include <signal.h>
#include <termios.h>

NUSAGE(2, "[--merge|--symmetric|--select-head|--select-tail] [--remove-base] [--no-reduce] [-i] [<path>...]");

#if defined(__clang__)
# pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

enum successfulness {
	MERGED = 0,
	CONFLICT = 1,
	ERROR = 2
};

enum interactive_command {
	INTERACTIVE_ERROR,
	INTERACTIVE_REDIFF,
	INTERACTIVE_EDIT,
	INTERACTIVE_SKIP_FILE
};

static int interactive = 0;
static int originally_interactive;
#ifndef TEST
static int ttyfd = -1;
# define ttyfd_in ttyfd
# define ttyfd_out ttyfd
static struct termios tty_settings;
#else
static int ttyfd_in = -1;
static int ttyfd_out = -1;
# define ttyfd ttyfd_out
#endif
static struct stat tty_st;
static int merge = 0;
static int symmetric = 0;
static int remove_base = 0;
static int select_head = 0;
static int select_tail = 0;
static int reduce = 1;
static const char *editor;
static char *editor_freeable = NULL;

struct line {
	union {
		size_t in_off;
		const unsigned char *in;
	};
	const char *text;
	size_t len;
};

struct text {
	struct line *lines;
	size_t nlines;
	size_t lines_size;
};

struct subhunk {
	union {
		struct line *head;
		unsigned char *in;
	};
	struct text text;
};

struct hunk {
	struct subhunk *subs;
	size_t nsubs;
};

#ifndef TEST
static void
restore_tty(void)
{
	(void)tcsetattr(ttyfd, TCSANOW, &tty_settings);
	close(ttyfd);
}

static void
restore_tty_on_signal(int signo)
{
	(void)tcsetattr(ttyfd, TCSANOW, &tty_settings);
	raise(signo);
}

static void
configure_interactive_tty(void)
{
	struct termios settings = tty_settings;
	settings.c_lflag &= (tcflag_t)~(ICANON | ISIG);
	settings.c_cc[VMIN] = 1;
	settings.c_cc[VTIME] = 0;
	if (tcsetattr(ttyfd, TCSANOW, &settings))
		eprintf("tcsetattr /dev/tty:");
}
#endif

static int
is_controlling_tty(int fd)
{
	struct stat st;
	return !fstat(fd, &st) && S_ISCHR(st.st_mode) && st.st_dev == tty_st.st_dev && st.st_rdev == tty_st.st_rdev;
}

static int
send_line(int fd, const char *fname, const struct line *line)
{
	size_t off = 0;
	ssize_t r;
	while (off < line->len) {
		r = write(fd, &line->text[off], line->len - off);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			weprintf("write %s:", fname);
			return -1;
		}
		off += (size_t)r;
	}
	return 0;
}

static int
send_text(int fd, const char *fname, const struct text *text)
{
	size_t i;
	for (i = 0; i < text->nlines; i++)
		if (send_line(fd, fname, &text->lines[i]))
			return -1;
	return 0;
}

static void
ensure_nonstandard(int *fdp)
{
	int r;
	if (*fdp > 2)
		return;
	r = fcntl(*fdp, F_DUPFD, 3);
	if (r < 3)
		eprintf("fcntl <pipe> F_DUPFD 3:");
	*fdp = r;
}

#if defined(__GNUC__)
__attribute__((__pure__))
#endif
static int
in_all(const unsigned char *in, size_t full_bytes, unsigned char last_byte)
{
	while (full_bytes--)
		if (!(~*in++ & ((1U << CHAR_BIT) - 1U)))
			return 0;
	return *in == last_byte;
}

static int
line_is_marker(struct line *line, char marker_symbol, size_t conflict_marker_size)
{
	size_t i;

	if (!conflict_marker_size)
		conflict_marker_size = 7;

	if (line->len < conflict_marker_size)
		return 0;

	for (i = 0; i < conflict_marker_size; i++)
		if (line->text[i] != marker_symbol)
			return 0;

	return i == line->len || line->text[i] == ' ' || line->text[i] == '\n' || line->text[i] == '\r';
}

static int
line_is_a_marker(struct line *line, size_t conflict_marker_size, char *marker_symbolp)
{
	switch (line->len ? line->text[0] : '\0') {
	case '<':
	case '|':
	case '=':
	case '>':
		if (line_is_marker(line, line->text[0], conflict_marker_size)) {
			*marker_symbolp = line->text[0];
			return 1;
		}
		/* fall through */
	default:
		return 0;
	}
}

static int
line_is_either_marker(struct line *line, const char *marker_symbols, size_t conflict_marker_size)
{
	char marker_symbol;
	return line_is_a_marker(line, conflict_marker_size, &marker_symbol) && strchr(marker_symbols, marker_symbol);
}

static void
append_lines(struct text *textp, const struct line *lines, size_t n)
{
	if (n > textp->lines_size - textp->nlines) {
		textp->lines_size = textp->nlines + n;
		textp->lines = ereallocarray(textp->lines, textp->lines_size, sizeof(*textp->lines));
	}
	memcpy(&textp->lines[textp->nlines], lines, n * sizeof(*lines));
	textp->nlines += n;
}

static void
append_line(struct text *textp, const struct line *line)
{
	append_lines(textp, line, 1U);
}

static void
append_text(struct text *textp, const struct text *text)
{
	append_lines(textp, text->lines, text->nlines);
}

#if defined(__GNUC__)
__attribute__((__pure__))
#endif
static int
texts_equal(const struct text *a, const struct text *b)
{
	size_t i;
	if (a->nlines != b->nlines)
		return 0;
	for (i = 0; i < a->nlines; i++)
		if (a->lines[i].len != b->lines[i].len)
			return 0;
	for (i = 0; i < a->nlines; i++)
		if (memcmp(a->lines[i].text, b->lines[i].text, a->lines[i].len))
			return 0;
	return 1;
}

static char *
diff_subhunks(const struct subhunk *f1, const struct subhunk *f2)
{
	size_t context = MAX(f1->text.nlines, f2->text.nlines);
	pid_t pid1, pid2, piddiff;
	int pipe1[2], pipe2[2], pipediff[2];
	char *ret = NULL;
	size_t ret_size = 0;
	size_t ret_off = 0;
	int status;
	size_t i, n;
	int rem, ret_next = 0;
	ssize_t r;
	union {
		struct {
			char context[sizeof("-U") + 3U * sizeof(context)];
			char pipe1[sizeof("/dev/fd/-") + 3U * sizeof(pipe1[0])];
			char pipe2[sizeof("/dev/fd/-") + 3U * sizeof(pipe2[0])];
		};
		char text[8096];
	} buf;

	if (pipe(pipediff))
		eprintf("pipe:");
	if (pipe(pipe1))
		eprintf("pipe:");
	if (pipe(pipe2))
		eprintf("pipe:");

	ensure_nonstandard(&pipe2[0]);
	ensure_nonstandard(&pipe1[0]);

	pid1 = fork();
	if (pid1 < 0)
		eprintf("fork:");
	if (pid1 == 0) {
		close(pipediff[0]);
		close(pipediff[1]);
		close(pipe1[0]);
		close(pipe2[0]);
		close(pipe2[1]);
		if (send_text(pipe1[1], "<pipe>", &f1->text))
			_exit(1);
		close(pipe1[1]);
		_exit(0);
	}

	pid2 = fork();
	if (pid2 < 0)
		eprintf("fork:");
	if (pid2 == 0) {
		close(pipediff[0]);
		close(pipediff[1]);
		close(pipe2[0]);
		close(pipe1[0]);
		close(pipe1[1]);
		if (send_text(pipe2[1], "<pipe>", &f2->text))
			_exit(1);
		close(pipe2[1]);
		_exit(0);
	}

	close(pipe1[1]);
	close(pipe2[1]);

	piddiff = fork();
	if (piddiff < 0)
		eprintf("fork:");
	if (piddiff == 0) {
		close(pipediff[0]);
		if (pipediff[1] != STDOUT_FILENO) {
			if (dup2(pipediff[1], STDOUT_FILENO) != STDOUT_FILENO)
				eprintf("dup2 <pipe> <stdout>:");
			close(pipediff[1]);
		}
		sprintf(buf.context, "-U%zu", context);
		sprintf(buf.pipe1, "/dev/fd/%i", pipe1[0]);
		sprintf(buf.pipe2, "/dev/fd/%i", pipe2[0]);
#ifdef PR_SET_PDEATHSIG
		prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
		execlp("diff", "diff", buf.context, "--", buf.pipe1, buf.pipe2, NULL);
		_exit(125);
	}

	close(pipediff[1]);
	close(pipe1[0]);
	close(pipe2[0]);

	rem = 3;
	for (;;) {
		r = read(pipediff[0], buf.text, sizeof(buf.text));
		if (r <= 0) {
			if (!r)
				break;
			if (errno == EINTR)
				continue;
			eprintf("read <diff(1) subprocess stdout pipe>:");
		}
		n = (size_t)r;
		for (i = 0; i < n; i++) {
			if (ret_next) {
				ret_next = 0;
				if (ret_off == ret_size) {
					if (ret_size > SIZE_MAX - 512U) {
						errno = ENOMEM;
						eprintf("realloc:");
					}
					ret = erealloc(ret, ret_size += 512U);
				}
				ret[ret_off++] = buf.text[i];
			} else if (buf.text[i] == '\n') {
				if (rem) {
					if (!--rem)
						ret_next = 1;
					continue;
				}
				ret_next = 1;
			}
		}
	}

	if (waitpid(pid1, &status, 0) != pid1)
		eprintf("waitpid <file sender subprocess> 0:");
	if (status)
		eprintf("waitpid <file sender subprocess> 0: process exited abnormally");

	if (waitpid(pid2, &status, 0) != pid2)
		eprintf("waitpid <file sender subprocess> 0:");
	if (status)
		eprintf("waitpid <file sender subprocess> 0: process exited abnormally");

	if (waitpid(piddiff, &status, 0) != piddiff)
		eprintf("waitpid <diff(1) subprocess> 0:");
	if (status == 0) {
		ret_off = f1->text.nlines;
		ret = erealloc(ret, ret_off + 1U);
		memset(ret, ' ', f1->text.nlines);
	} else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
		ret = erealloc(ret, ret_off + 1U);
	} else {
		eprintf("waitpid <diff(1) subprocess> 0: process exited abnormally");
	}
	ret[ret_off] = '\0';
	return ret;
}

static void
diff_hunk(struct subhunk *ret, const struct hunk *hunk)
{
	char *diff, *p;
	size_t k, i, j, in_size = 0, in_off = 0;
	size_t in_step, bit_off;
	unsigned char bit;
	struct subhunk ret_buf, ret_tmp;

	in_step = hunk->nsubs / CHAR_BIT;
	in_step += (size_t)!!(hunk->nsubs % CHAR_BIT);

	bit = (unsigned char)1;
	bit_off = 0;
	ret->text.lines_size = ret->text.nlines = hunk->subs[0].text.nlines;
	in_size = ret->text.nlines * in_step;
	ret->in = emalloc(in_size);
	ret->text.lines = ecalloc(ret->text.lines_size, sizeof(*ret->text.lines));
	memcpy(ret->text.lines, hunk->subs[0].text.lines, ret->text.nlines * sizeof(*hunk->subs[0].text.lines));
	memset(ret->in, 0, in_size);
	for (i = 0; i < ret->text.nlines; i++) {
		ret->text.lines[i].in_off = in_off;
		ret->in[in_off + bit_off] = bit;
		in_off += in_step;
	}

	ret_buf.in = ret->in;
	ret_buf.text.lines = NULL;
	ret_buf.text.lines_size = 0;

	for (k = 1; k < hunk->nsubs; k++) {
		bit <<= 1;
		if (!bit) {
			bit = (unsigned char)1;
			bit_off += 1;
		}
		diff = diff_subhunks(ret, &hunk->subs[k]);
		i = j = 0;
		ret_buf.text.nlines = 0;
		for (p = diff; *p; p++) {
			if (*p == '-') {
				append_line(&ret_buf.text, &ret->text.lines[i]);
				i++;
			} else if (*p == '+') {
				append_line(&ret_buf.text, &hunk->subs[k].text.lines[j]);
				if (in_off == in_size) {
					if (in_step > (SIZE_MAX - in_size) / 16U) {
						errno = ENOMEM;
						eprintf("realloc:");
					}
					in_size += 16U * in_step;
					ret_buf.in = ret->in = erealloc(ret->in, in_size);
					memset(&ret->in[in_off], 0, in_size - in_off);
				}
				ret_buf.text.lines[ret_buf.text.nlines - 1U].in_off = in_off;
				ret_buf.in[ret_buf.text.lines[ret_buf.text.nlines - 1U].in_off + bit_off] = bit;
				in_off += in_step;
				j++;
			} else if (*p == ' ') {
				append_line(&ret_buf.text, &ret->text.lines[i]);
				ret_buf.in[ret_buf.text.lines[ret_buf.text.nlines - 1U].in_off + bit_off] |= bit;
				i++;
				j++;
			} else {
				eprintf("output of diff(1) was corrupted");
			}
		}
		ret->text.nlines = ret_buf.text.nlines;
		ret_tmp.text = ret->text;
		ret->text = ret_buf.text;
		ret_buf.text = ret_tmp.text;
		free(diff);
	}

	for (i = 0; i < ret->text.nlines; i++)
		ret->text.lines[i].in = &ret->in[ret->text.lines[i].in_off];

	free(ret_buf.text.lines);
}

static int
read_lines(struct text *lines_out, char **text_out, int fd, const char *fname)
{
	struct line *lines;
	size_t text_len = 0;
	size_t text_size = 0;
	ssize_t r;
	void *new;
	size_t i;

	*lines_out = (struct text){0};
	*text_out = NULL;

	for (;;) {
		if (text_len == text_size) {
			if (text_size > SIZE_MAX - 8096U) {
				errno = ENOMEM;
				weprintf("realloc:");
				goto fail;
			}
			text_size += 8096U;
			new = realloc(*text_out, text_size);
			if (!new)
				goto fail;
			*text_out = new;
		}
		r = read(fd, &(*text_out)[text_len], text_size - text_len);
		if (r <= 0) {
			if (!r)
				break;
			if (errno == EINTR)
				continue;
			weprintf("read %s:", fname);
			goto fail;
		}
		text_len += (size_t)r;
	}

	lines_out->nlines = (text_len ? 1U : 0U);
	for (i = 0; i + 1U < text_len; i++)
		if ((*text_out)[i] == '\n')
			lines_out->nlines += 1U;

	if (!lines_out->nlines)
		return 0;

	lines = lines_out->lines = ecalloc(lines_out->nlines, sizeof(*lines_out->lines));

	for (i = 0; i < lines_out->nlines; i++)
		lines[i].in = NULL;

	lines->text = *text_out;
	lines++;
	for (i = 0; i + 1U < text_len;) {
		if ((*text_out)[i++] == '\n') {
			lines->text = &(*text_out)[i];
			lines[-1].len = (size_t)(lines->text - lines[-1].text);
			lines++;
		}
	}
	lines[-1].len = (size_t)(&(*text_out)[text_len] - lines[-1].text);

	return 0;

fail:
	free(*text_out);
	*text_out = NULL;
	return -1;
}

static void
display_help(size_t nsubhunks)
{
	dprintf(ttyfd_out, "\033[1;31m");
	dprintf(ttyfd_out, "h - select head\n");
	dprintf(ttyfd_out, "t - select tail\n");
	if (nsubhunks == 3U)
		dprintf(ttyfd_out, "b - remove base\n");
	dprintf(ttyfd_out, "m - merge\n");
	dprintf(ttyfd_out, "y - select symmetric branches\n");
	dprintf(ttyfd_out, "r - reduce\n");
	dprintf(ttyfd_out, "e - manually edit this hunk\n");
	dprintf(ttyfd_out, "s - skip this hunk\n");
	dprintf(ttyfd_out, "d - skip this hunk and all later hunks in the file\n");
	dprintf(ttyfd_out, "q - quit; skip this hunk and all remaining hunks\n");
	dprintf(ttyfd_out, "? - print help\n");
	dprintf(ttyfd_out, "\033[0m");
}

static void
display_line(const struct line *line, const char *colour)
{
	ssize_t n;
	size_t off;

	if (colour)
		dprintf(ttyfd_out, "\033[%sm", colour);

	for (off = 0; off < line->len;) {
		n = write(ttyfd_out, &line->text[off], line->len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			eprintf("write /dev/tty:");
		}
		off += (size_t)n;
	}

	if (colour)
		dprintf(ttyfd_out, "\033[0m");
}

static void
display_hunk(const struct text *text, size_t first, size_t last, const struct hunk *hunk, size_t marker_size)
{
	size_t i, start, end, subhunk = 0;
	size_t before = 0U, after = 0U;
	const char *colour;

	for (; before < 3U && before < first; before++)
		if (line_is_marker(&text->lines[first - (before + 1U)], '>', marker_size))
			break;
	start = first - before;

	for (; after < 3U && after + 1U < text->nlines - last; after++)
		if (line_is_marker(&text->lines[last + (after + 1U)], '<', marker_size))
			break;
	end = last + after;

	for (i = start; i <= end; i++) {
		/* for simplicity subhunks are indexed from 1 */
		if (first > i || i > last) {
			colour = NULL;
		} else if (line_is_a_marker(&text->lines[i], marker_size, &(char){0})) {
			colour = "36";
			subhunk++;
		} else if (subhunk == 1U || subhunk == hunk->nsubs) {
			colour = "32";
		} else if (hunk->nsubs == 3U && subhunk == 2U) {
			colour = "31";
		} else {
			colour = "35";
		}
		display_line(&text->lines[i], colour);
	}
}

static enum interactive_command
get_command(size_t nsubhunks, size_t number, size_t total)
{
	char c;
	ssize_t n;
	int matched = 0;

again:
	dprintf(ttyfd_out, "\033[1;34m(%zu/%zu) Resolve this conflict [h,t,b,m,y,r,e,s,d,q,?]? \033[0m", number, total);

	for (;;) {
		n = read(ttyfd_in, &c, 1);
		if (!n)
			c = 'q';
		if (n >= 0)
			break;
		if (errno != EINTR)
			return INTERACTIVE_ERROR;
	}
	if (c != '\n')
		dprintf(ttyfd_out, "\n");
#ifndef TEST
	if (c == 'Z' - '@') {
		if (tcsetattr(ttyfd, TCSANOW, &tty_settings))
			weprintf("tcsetattr /dev/tty:");
		raise(SIGTSTP);
		configure_interactive_tty();
		goto again;
	}
#endif

	matched |= select_head = c == 'h';
	matched |= select_tail = c == 't';
	matched |= merge       = c == 'm';
	matched |= symmetric   = c == 'y';
	matched |= reduce      = c == 'r';
	matched |= remove_base = c == 'b';
	matched |= /* skip = */  c == 's';

	switch (c) {
	case 'e':
		return INTERACTIVE_EDIT;
	case 'q':
		interactive = 0;
		/* fall through */
	case 'd':
		return INTERACTIVE_SKIP_FILE;
	case 'b':
		if (nsubhunks == 3U) {
		default:
			if (matched)
				return INTERACTIVE_REDIFF;
		}
		/* fall through */
	case '?':
		display_help(nsubhunks);
		goto again;
	}
}

static char *
get_config_editor(const char *command)
{
	ssize_t len;
	FILE *fp;

	free(editor_freeable);
	editor_freeable = NULL;

	fp = popen(command, "r");
	if (!fp)
		return NULL;
	len = getline(&editor_freeable, &(size_t){0U}, fp);
	if (len < 1 || editor_freeable[--len] != '\n') {
		pclose(fp);
		return NULL;
	}
	if (pclose(fp))
		return NULL;
	editor_freeable[len] = '\0';

	return editor_freeable;
}

static void
select_editor(void)
{
	if ((editor = getenv("GIT_EDITOR")) && *editor)
		return;
	if ((editor = get_config_editor("git config --get rediff.editor")) && *editor)
		return;
	if ((editor = get_config_editor("git config --get core.editor")) && *editor)
		return;
	if ((editor = getenv("VISUAL")) && *editor)
		return;
	if ((editor = getenv("EDITOR")) && *editor)
		return;
	editor = "vi";
}

static enum successfulness
manual_edit(struct text *resp, const struct hunk *hunk, const struct line *tail, size_t conflict_marker_size)
{
	char filename[] = ".git-rediff-edit.XXXXXX";
	struct text text;
	char *contents, *command;
	int fd;
	size_t i;

	fd = mkstemp(filename);
	if (fd < 0) {
		weprintf("mkstemp %s:", filename);
		return ERROR;
	}
	for (i = 0; i < hunk->nsubs; i++) {
		if (send_line(fd, filename, hunk->subs[i].head) ||
		    send_text(fd, filename, &hunk->subs[i].text))
			goto fail_close;
	}
	if (send_line(fd, filename, tail)) {
		weprintf("write %s:", filename);
		goto fail_close;
	}
	if (close(fd)) {
		weprintf("write %s:", filename);
		goto fail_unlink;
	}

	command = emalloc(strlen(editor) + 1U + sizeof(filename));
	stpcpy(stpcpy(stpcpy(command, editor), " "), filename);
	if (system(command)) {
		weprintf("editor failed");
		free(command);
		goto fail_unlink;
	}
	free(command);

	fd = open(filename, O_RDONLY);
	if (fd < 0)
		goto fail_unlink;
	if (read_lines(&text, &contents, fd, filename))
		goto fail_close;
	close(fd);
	unlink(filename);

	append_text(resp, &text);
	for (i = 0; i < text.nlines; i++)
		if (line_is_a_marker(&text.lines[i], conflict_marker_size, &(char){0}))
			break;
	free(text.lines);

	return i < text.nlines ? CONFLICT : MERGED;

fail_close:
	close(fd);
fail_unlink:
	unlink(filename);
	return ERROR;
}

static enum successfulness
rediff_hunk(struct text *resp, const struct hunk *hunk, const struct line *tail)
{
	struct subhunk diff, baseless_subhunks[2];
	size_t i, j, full_bytes;
	unsigned char last_byte;
	struct hunk uncommon, baseless_hunk;
	int in_uncommon = 0;
	enum successfulness ret = MERGED;

	if (select_head || select_tail) {
		append_text(resp, &hunk->subs[select_tail ? hunk->nsubs - 1U : 0U].text);
		return MERGED;
	}

	if (merge && hunk->nsubs >= 3U) {
		if (texts_equal(&hunk->subs[0].text, &hunk->subs[hunk->nsubs - 1U].text))
			goto genuine_conflict;
		for (i = 2U; i < hunk->nsubs - 1U; i++)
			if (!texts_equal(&hunk->subs[i].text, &hunk->subs[1U].text))
				goto genuine_conflict;
		if (texts_equal(&hunk->subs[1U].text, &hunk->subs[hunk->nsubs - 1U].text))
			append_text(resp, &hunk->subs[0].text);
		else if (texts_equal(&hunk->subs[1U].text, &hunk->subs[0].text))
			append_text(resp, &hunk->subs[hunk->nsubs - 1U].text);
		else
			goto genuine_conflict;
		return MERGED;
	} else if (symmetric) {
		if (!texts_equal(&hunk->subs[0].text, &hunk->subs[hunk->nsubs - 1U].text))
			goto genuine_conflict;
		for (i = 2U; i < hunk->nsubs - 1U; i++)
			if (!texts_equal(&hunk->subs[i].text, &hunk->subs[1U].text))
				goto genuine_conflict;
		append_text(resp, &hunk->subs[0].text);
		return MERGED;
	}
genuine_conflict:

	if (remove_base && hunk->nsubs == 3U) {
		baseless_subhunks[0] = hunk->subs[0];
		baseless_subhunks[1] = hunk->subs[2];
		baseless_hunk.subs = baseless_subhunks;
		baseless_hunk.nsubs = 2U;
		hunk = &baseless_hunk;
	}

	if (!reduce) {
		for (i = 0; i < hunk->nsubs; i++) {
			append_line(resp, hunk->subs[i].head);
			append_text(resp, &hunk->subs[i].text);
		}
		append_line(resp, tail);
		return CONFLICT;
	}

	full_bytes = hunk->nsubs / CHAR_BIT;
	last_byte = 0;
	if (hunk->nsubs % CHAR_BIT) {
		last_byte = (unsigned char)(1U << ((hunk->nsubs % CHAR_BIT) - 1U));
		last_byte |= (unsigned char)(last_byte - 1U);
	}

	uncommon.nsubs = hunk->nsubs;
	uncommon.subs = ecalloc(uncommon.nsubs, sizeof(*uncommon.subs));
	for (i = 0; i < uncommon.nsubs; i++) {
		uncommon.subs[i].head = hunk->subs[i].head;
		uncommon.subs[i].text.lines = NULL;
		uncommon.subs[i].text.nlines = 0;
		uncommon.subs[i].text.lines_size = 0;
	}

	diff_hunk(&diff, hunk);
	for (i = 0; i < diff.text.nlines; i++) {
		if (in_all(diff.text.lines[i].in, full_bytes, last_byte)) {
			if (in_uncommon) {
				for (j = 0; j < uncommon.nsubs; j++) {
					append_line(resp, uncommon.subs[j].head);
					append_text(resp, &uncommon.subs[j].text);
				}
				append_line(resp, tail);
				in_uncommon = 0;
			}
			append_line(resp, &diff.text.lines[i]);
		} else {
			if (!in_uncommon) {
				in_uncommon = 1;
				for (j = 0; j < uncommon.nsubs; j++)
					uncommon.subs[j].text.nlines = 0;
			}
			ret = CONFLICT;
			for (j = 0; j < uncommon.nsubs; j++)
				if ((diff.text.lines[i].in[j / CHAR_BIT] >> (j % CHAR_BIT)) & 1U)
					append_line(&uncommon.subs[j].text, &diff.text.lines[i]);
		}
	}

	if (in_uncommon) {
		for (j = 0; j < uncommon.nsubs; j++) {
			append_line(resp, uncommon.subs[j].head);
			append_text(resp, &uncommon.subs[j].text);
		}
		append_line(resp, tail);
	}

	free(diff.in);
	free(diff.text.lines);
	for (i = 0; i < uncommon.nsubs; i++)
		free(uncommon.subs[i].text.lines);
	free(uncommon.subs);

	return ret;
}

static enum successfulness
rediff_file(struct text *text_out, const struct text *text_in, size_t conflict_marker_size, const char *fname)
{
	size_t i, t, nhunks = 0, hunkno = 0;
	struct hunk hunk = {0};
	ssize_t subhunk = -1;
	enum successfulness ret = MERGED, r;
	enum interactive_command command;

	*text_out = (struct text){0};
	if (interactive) {
		command = INTERACTIVE_REDIFF;
		for (i = 0; i < text_in->nlines; i++)
			if (line_is_marker(&text_in->lines[i], '>', conflict_marker_size))
				nhunks++;
	} else {
		command = INTERACTIVE_SKIP_FILE;
	}

	for (i = 0; i < text_in->nlines; i++) {
		if (line_is_marker(&text_in->lines[i], '<', conflict_marker_size)) {
			if (subhunk >= 0)
				goto syntax_error;
			goto new_subhunk;
		} else if (line_is_either_marker(&text_in->lines[i], "|=", conflict_marker_size)) {
			if (subhunk < 0)
				goto syntax_error;
			if (!line_is_either_marker(hunk.subs[subhunk].head, "<|", conflict_marker_size))
				goto syntax_error;
		new_subhunk:
			subhunk++;
			if ((size_t)subhunk == hunk.nsubs)
				hunk.subs = ereallocarray(hunk.subs, ++hunk.nsubs, sizeof(*hunk.subs));
			hunk.subs[subhunk].text = (struct text){0};
			hunk.subs[subhunk].head = &text_in->lines[i];
		} else if (line_is_marker(&text_in->lines[i], '>', conflict_marker_size)) {
			if (subhunk < 0)
				goto syntax_error;
			if (!line_is_marker(hunk.subs[subhunk].head, '=', conflict_marker_size))
				goto syntax_error;
			t = hunk.nsubs;
			hunk.nsubs = (size_t)subhunk + 1U;
			if (command != INTERACTIVE_SKIP_FILE) {
				hunkno++;
				display_hunk(text_in, (size_t)(hunk.subs[0].head - text_in->lines), i, &hunk, conflict_marker_size);
				command = get_command(hunk.nsubs, hunkno, nhunks);
				if (command == INTERACTIVE_ERROR)
					goto error;
			}
			if (command == INTERACTIVE_EDIT)
				r = manual_edit(text_out, &hunk, &text_in->lines[i], conflict_marker_size);
			else
				r = rediff_hunk(text_out, &hunk, &text_in->lines[i]);
			ret = MAX(ret, r);
			hunk.nsubs = t;
			subhunk = -1;
		} else {
			if (subhunk < 0)
				append_line(text_out, &text_in->lines[i]);
			else
				append_line(&hunk.subs[subhunk].text, &text_in->lines[i]);
		}
	}

	if (subhunk >= 0) {
		weprintf("file %s is truncated", fname);
	error:
		ret = ERROR;
	}

	for (i = 0; i < hunk.nsubs; i++)
		free(hunk.subs[i].text.lines);
	free(hunk.subs);

	return ret;

syntax_error:
	weprintf("syntax error at %s:%zu", fname, i + 1U);
	goto error;
}

static size_t
get_conflict_marker_size(const char *path)
{
	int pipe_fds[2], status;
	pid_t pid;
	char *text = NULL;
	size_t size = 0;
	size_t len = 0;
	size_t i, off = 0;
	ssize_t r;
	size_t conflict_marker_size, digit;

	if (pipe(pipe_fds))
		eprintf("pipe:");

	pid = fork();
	if (pid < 0)
		eprintf("fork:");
	if (pid == 0) {
		close(pipe_fds[0]);
		if (pipe_fds[1] != STDOUT_FILENO) {
			close(STDOUT_FILENO);
			if (dup2(pipe_fds[1], STDOUT_FILENO) != STDOUT_FILENO)
				eprintf("dup2 <pipe> <stdout>:");
			close(pipe_fds[1]);
		}
		execlp("git", "git", "check-attr", "conflict-marker-size", "--", path, NULL);
		return 1;
	}

	close(pipe_fds[1]);

	for (;;) {
		if (len == size)
			text = erealloc(text, size += 128);
		r = read(pipe_fds[0], &text[len], size - len);
		if (r <= 0) {
			if (!r)
				break;
			if (errno == EINTR)
				continue;
			eprintf("read <pipe>:");
		}
		len += (size_t)r;
	}

	if (waitpid(pid, &status, 0) != pid)
		eprintf("waitpid <subprocess>:");
	if (status) {
	use_default:
		free(text);
		return 0;
	}

	for (i = 0; i < len; i++)
		if (text[i] == ' ')
			off = i + 1U;

	if (!off || text[--len] != '\n' || !isdigit(text[off]))
		goto use_default;

	conflict_marker_size = 0;
	for (i = off; i < len; i++) {
		if (!isdigit(text[i]))
			goto use_default;
		digit = (size_t)(text[i] & 15);
		if (conflict_marker_size > (SIZE_MAX - digit) / 10U) {
			free(text);
			return SIZE_MAX;
		}
		conflict_marker_size = conflict_marker_size * 10U + digit;
	}

	free(text);
	return conflict_marker_size;
}

static enum successfulness
rediff(const char *fname, int print_filename)
{
	struct text text_in, text_out;
	char *text;
	int fd, close_fd;
	enum successfulness ret;
	size_t conflict_marker_size;

	if (interactive && print_filename)
		dprintf(ttyfd_out, "\033[1m%s\033[0m\n", fname);

	if (!strcmp(fname, "-")) {
		fname = "<stdout>";
		close_fd = 0;
		fd = STDOUT_FILENO;
		if (read_lines(&text_in, &text, STDIN_FILENO, "<stdin>"))
			return ERROR;
		conflict_marker_size = get_conflict_marker_size(".");
	} else {
		close_fd = 1;
		fd = open(fname, O_RDWR | O_NOCTTY);
		if (fd < 0) {
			weprintf("open %s O_RDWR | O_NOCTTY:", fname);
			return ERROR;
		}
		if (originally_interactive && is_controlling_tty(fd)) {
			weprintf("%s: file is the controlling terminal\n", fname);
			return ERROR;
		}
		if (read_lines(&text_in, &text, fd, fname)) {
			close(fd);
			return ERROR;
		}
		if (lseek(fd, 0, SEEK_SET) != 0) {
			weprintf("lseek %s 0 SEEK_SET:", fname);
			return ERROR;
		}
		conflict_marker_size = get_conflict_marker_size(fname);
	}

	ret = rediff_file(&text_out, &text_in, conflict_marker_size, fname);
	if (ret == ERROR) {
		ret = ERROR;
		goto out;
	}

	if (send_text(fd, fname, &text_out)) {
		ret = ERROR;
		goto out;
	}

	if (close_fd) {
		off_t length = lseek(fd, 0, SEEK_CUR);
		if (length < 0) {
			weprintf("lseek %s 0 SEEK_CUR:", fname);
			ret = ERROR;
			goto out;
		}
		if (ftruncate(fd, length)) {
			weprintf("ftruncate %s <current position>:", fname);
			ret = ERROR;
			goto out;
		}
	}

out:
	if (close_fd)
		close(fd);
	free(text_out.lines);
	free(text_in.lines);
	free(text);
	return ret;
}

int
main(int argc, char *argv[])
{
	enum successfulness ret = 0, r;

	libsimple_default_failure_exit = 2;

	ARGBEGIN {
	case '-':
		if (TESTLONG("--interactive", 0))
			case 'i': interactive = 1;
		else if (TESTLONG("--no-interactive", 0))
			interactive = 0;
		else if (TESTLONG("--merge", 0))
			case 'm': merge = 1;
		else if (TESTLONG("--no-merge", 0))
			merge = 0;
		else if (TESTLONG("--symmetric", 0))
			case 'y': symmetric = 1;
		else if (TESTLONG("--no-symmetric", 0))
			symmetric = 0;
		else if (TESTLONG("--reduce", 0))
			case 'r': reduce = 1;
		else if (TESTLONG("--no-reduce", 0))
			reduce = 0;
		else if (TESTLONG("--remove-base", 0))
			remove_base = 1;
		else if (TESTLONG("--no-remove-base", 0))
			remove_base = 0;
		else if (TESTLONG("--select-head", 0))
			select_head = 1;
		else if (TESTLONG("--no-select-head", 0))
			select_head = 0;
		else if (TESTLONG("--select-tail", 0))
			select_tail = 1;
		else if (TESTLONG("--no-select-tail", 0))
			select_tail = 0;
		else
			usage();
		break;
	default:
		usage();
	} ARGEND;

	if (merge + symmetric + select_head + select_tail > 1)
		usage();

	originally_interactive = interactive;

	if (interactive) {
#ifdef TEST
		ttyfd_in = 8;
		ttyfd_out = 9;
#else
		struct sigaction sa = {
			.sa_handler = &restore_tty_on_signal,
			.sa_flags = (int)SA_RESETHAND
		};
		ttyfd = open("/dev/tty", O_RDWR | O_NOCTTY);
		if (ttyfd < 0)
			eprintf("open /dev/tty O_RDWR | O_NOCTTY:");
		if (tcgetattr(ttyfd, &tty_settings))
			eprintf("tcgetattr /dev/tty:");
		configure_interactive_tty();
		if (atexit(restore_tty))
			eprintf("atexit:");
		sigemptyset(&sa.sa_mask);
		if (sigaction(SIGHUP, &sa, NULL))
			eprintf("sigaction SIGHUP:");
		if (sigaction(SIGINT, &sa, NULL))
			eprintf("sigaction SIGINT:");
		if (sigaction(SIGQUIT, &sa, NULL))
			eprintf("sigaction SIGQUIT:");
		if (sigaction(SIGTERM, &sa, NULL))
			eprintf("sigaction SIGTERM:");
#endif
		if (fstat(ttyfd, &tty_st))
			eprintf("fstat /dev/tty:");
		select_editor();
	}

	if (fstat(STDERR_FILENO, &(struct stat){0})) {
		int fd;
		if (errno != EBADF)
			eprintf("fstat <stderr>:");
		fd = open("/dev/null", O_WRONLY | O_NOCTTY);
		if (fd < 0)
			eprintf("open /dev/null O_WRONLY | O_NOCTTY:");
		if (fd != STDERR_FILENO) {
			if (dup2(fd, STDERR_FILENO) != STDERR_FILENO)
				eprintf("dup2 /dev/null <stderr>:");
			close(fd);
		}
	}

	if (argc) {
		for (; *argv; argv++) {
			r = rediff(*argv, argc > 1);
			ret = MAX(ret, r);
		}
	} else {
		ret = rediff("-", 0);
	}

	free(editor_freeable);
	return (int)ret;
}
