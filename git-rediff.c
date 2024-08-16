/* See LICENSE file for copyright and license details. */
#ifdef __linux__
# include <linux/prctl.h>
# include <sys/prctl.h>
#endif
#include <libsimple.h>
#include <libsimple-arg.h>

NUSAGE(2, "[<path>...]");

#if defined(__clang__)
# pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

enum successfulness {
	MERGED = 0,
	CONFLICT = 1,
	ERROR = 2
};

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
line_startswith(struct line *line, const char *head)
{
	size_t len = strlen(head);
	return line->len >= len && !strncmp(line->text, head, len);
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
			char context[sizeof("-U") + 3 * sizeof(context)];
			char pipe1[sizeof("/dev/fd/-") + 3 * sizeof(pipe1[0])];
			char pipe2[sizeof("/dev/fd/-") + 3 * sizeof(pipe2[0])];
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
					if (ret_size > SIZE_MAX - 512) {
						errno = ENOMEM;
						eprintf("realloc:");
					}
					ret = erealloc(ret, ret_size += 512);
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

static enum successfulness
rediff_hunk(struct text *resp, const struct hunk *hunk, const struct line *tail)
{
	struct subhunk diff;
	size_t i, j, full_bytes;
	unsigned char last_byte;
	struct hunk uncommon;
	int in_uncommon = 0;
	enum successfulness ret = MERGED;

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
rediff_file(struct text *text_out, const struct text *text_in, const char *fname)
{
	size_t i, t;
	struct hunk hunk = {0};
	ssize_t subhunk = -1;
	enum successfulness ret = MERGED, r;

	*text_out = (struct text){0};

	for (i = 0; i < text_in->nlines; i++) {
		if (line_startswith(&text_in->lines[i], "<<<<<<<")) {
			if (subhunk >= 0)
				goto syntax_error;
			goto new_subhunk;
		} else if (line_startswith(&text_in->lines[i], "|||||||") || line_startswith(&text_in->lines[i], "=======")) {
			if (subhunk < 0)
				goto syntax_error;
			if (!line_startswith(hunk.subs[subhunk].head, "<<<<<<<") &&
			    !line_startswith(hunk.subs[subhunk].head, "|||||||"))
				goto syntax_error;
		new_subhunk:
			subhunk++;
			if ((size_t)subhunk == hunk.nsubs)
				hunk.subs = ereallocarray(hunk.subs, ++hunk.nsubs, sizeof(*hunk.subs));
			hunk.subs[subhunk].text.nlines = 0;
			hunk.subs[subhunk].head = &text_in->lines[i];
		} else if (line_startswith(&text_in->lines[i], ">>>>>>>")) {
			if (subhunk < 0)
				goto syntax_error;
			if (!line_startswith(hunk.subs[subhunk].head, "======="))
				goto syntax_error;
			t = hunk.nsubs;
			hunk.nsubs = (size_t)subhunk + 1U;
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

static enum successfulness
rediff(const char *fname)
{
	struct text text_in, text_out;
	char *text;
	int fd, close_fd;
	enum successfulness ret;

	if (!strcmp(fname, "-")) {
		fname = "<stdout>";
		close_fd = 0;
		fd = STDOUT_FILENO;
		if (read_lines(&text_in, &text, STDIN_FILENO, "<stdin>"))
			return ERROR;
	} else {
		close_fd = 1;
		fd = open(fname, O_RDWR);
		if (fd < 0) {
			weprintf("open %s O_RDWR:", fname);
			return ERROR;
		}
		if (read_lines(&text_in, &text, fd, "<stdin>")) {
			close(fd);
			return ERROR;
		}
		if (lseek(fd, 0, SEEK_SET) != 0) {
			weprintf("lseek %s 0 SEEK_SET:", fname);
			return ERROR;
		}
	}

	ret = rediff_file(&text_out, &text_in, fname);
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
	default:
		usage();
	} ARGEND;

	if (fstat(STDERR_FILENO, &(struct stat){0})) {
		int fd;
		if (errno != EBADF)
			eprintf("fstat <stderr>:");
		fd = open("/dev/null", O_WRONLY);
		if (fd < 0)
			eprintf("open /dev/null O_WRONLY:");
		if (fd != STDERR_FILENO) {
			if (dup2(fd, STDERR_FILENO) != STDERR_FILENO)
				eprintf("dup2 /dev/null <stderr>:");
			close(fd);
		}
	}

	if (argc) {
		for (; *argv; argv++) {
			r = rediff(*argv);
			ret = MAX(ret, r);
		}
	} else {
		ret = rediff("-");
	}

	return (int)ret;
}
