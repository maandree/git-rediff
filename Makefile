.POSIX:

CONFIGFILE = config.mk
include $(CONFIGFILE)

OBJ =\
	git-rediff.o

HDR =

TOBJ = $(OBJ:.o=.to)

TEST =\
	t/reduce-1\
	t/reduce-2-full\
	t/reduce-2-none\
	t/reduce-2-head\
	t/reduce-2-tail\
	t/reduce-2-middle\
	t/reduce-2-multiple-partial\
	t/reduce-2-non-aligned\
	t/reduce-3-full\
	t/reduce-3-none\
	t/reduce-3-head\
	t/reduce-3-tail\
	t/reduce-3-middle\
	t/reduce-3-multiple-partial\
	t/reduce-4-full\
	t/reduce-4-none\
	t/reduce-4-head\
	t/reduce-4-tail\
	t/reduce-4-middle\
	t/reduce-4-multiple-partial\
	t/reduce-5-full\
	t/reduce-5-none\
	t/reduce-5-head\
	t/reduce-5-tail\
	t/reduce-5-middle\
	t/reduce-5-multiple-partial\
	t/reduce-2-hunks\
	t/reduce-2-files\
	t/reduce-is-default\
	t/reduce-no-conflicts\
	t/reduce-empty-head\
	t/reduce-empty-base\
	t/reduce-empty-tail\
	t/stdin-no-operand\
	t/stdin-dash-operand\
	t/stdin-dashdash-no-operand\
	t/stdin-dashdash-dash-operand\
	t/no-reduce-1\
	t/no-reduce-2-full\
	t/no-reduce-2-none\
	t/no-reduce-2-head\
	t/no-reduce-2-tail\
	t/no-reduce-2-middle\
	t/no-reduce-2-multiple-partial\
	t/no-reduce-2-non-aligned\
	t/no-reduce-3-full\
	t/no-reduce-3-none\
	t/no-reduce-3-head\
	t/no-reduce-3-tail\
	t/no-reduce-3-middle\
	t/no-reduce-3-multiple-partial\
	t/no-reduce-4-full\
	t/no-reduce-4-none\
	t/no-reduce-4-head\
	t/no-reduce-4-tail\
	t/no-reduce-4-middle\
	t/no-reduce-4-multiple-partial\
	t/no-reduce-5-full\
	t/no-reduce-5-none\
	t/no-reduce-5-head\
	t/no-reduce-5-tail\
	t/no-reduce-5-middle\
	t/no-reduce-5-multiple-partial\
	t/no-reduce-2-hunks\
	t/no-reduce-2-files\
	t/no-reduce-no-conflicts\
	t/no-reduce-empty-head\
	t/no-reduce-empty-base\
	t/no-reduce-empty-tail\
	t/no-reduce\
	t/no-reduce-dashdash-dashprefix\
	t/invalid-option-combinations\
	t/combination-select-head-reduce\
	t/combination-select-tail-reduce\
	t/combination-merge-reduce\
	t/combination-symmetric-reduce\
	t/combination-remove-base-select-head-reduce\
	t/combination-remove-base-select-tail-reduce\
	t/combination-remove-base-merge-reduce\
	t/combination-remove-base-symmetric-reduce\
	t/combination-remove-base-reduce\
	t/all-positive-options-merge\
	t/all-positive-options-symmetric\
	t/all-positive-options-select-head\
	t/all-positive-options-select-tail\
	t/combination-2-select-head\
	t/combination-2-select-tail\
	t/combination-2-merge\
	t/combination-2-symmetric\
	t/combination-2-remove-base\
	t/combination-2-interactive\
	t/combination-select-head-2-no-select-head\
	t/combination-select-tail-2-no-select-tail\
	t/combination-merge-2-no-merge\
	t/combination-symmetric-2-no-symmetric\
	t/combination-remove-base-2-no-remove-base\
	t/combination-interactive-2-no-interactive\
	t/merge-short-option\
	t/symmetric-short-option\
	t/reduce-short-option\
	t/interactive-short-option\
	t/no-option-merge-yes-no\
	t/no-option-merge-no-yes\
	t/no-option-symmetric-yes-no\
	t/no-option-symmetric-no-yes\
	t/no-option-remove-base-yes-no\
	t/no-option-remove-base-no-yes\
	t/no-option-select-head-yes-no\
	t/no-option-select-head-no-yes\
	t/no-option-select-tail-yes-no\
	t/no-option-select-tail-no-yes\
	t/no-option-reduce-yes-no\
	t/no-option-reduce-no-yes\
	t/no-option-interactive-yes-no\
	t/no-option-interactive-no-yes\
	t/no-option-select-head-yes-then-select-tail-yes-no\
	t/no-option-select-tail-yes-then-select-head-yes-no\
	t/no-option-merge\
	t/no-option-symmetric\
	t/no-option-remove-base\
	t/no-option-select-head\
	t/no-option-select-tail\
	t/no-option-reduce\
	t/no-option-interactive\
	t/select-head-1\
	t/select-head-2\
	t/select-head-3\
	t/select-head-3-empty-head\
	t/select-head-3-empty-base\
	t/select-head-3-empty-tail\
	t/select-head-4\
	t/select-head-multiple-hunks\
	t/select-head-multiple-files\
	t/select-head-no-conflicts\
	t/select-tail-1\
	t/select-tail-2\
	t/select-tail-3\
	t/select-tail-3-empty-head\
	t/select-tail-3-empty-base\
	t/select-tail-3-empty-tail\
	t/select-tail-4\
	t/select-tail-multiple-hunks\
	t/select-tail-multiple-files\
	t/select-tail-no-conflicts\
	t/remove-base-1\
	t/remove-base-2\
	t/remove-base-3\
	t/remove-base-3-empty-head\
	t/remove-base-3-empty-base\
	t/remove-base-3-empty-tail\
	t/remove-base-4\
	t/remove-base-5\
	t/remove-base-multiple-hunks\
	t/remove-base-multiple-files\
	t/remove-base-no-conflicts\
	t/merge-tail\
	t/merge-head\
	t/merge-base\
	t/merge-tail-4\
	t/merge-head-4\
	t/merge-upper-middle-4\
	t/merge-lower-middle-4\
	t/merge-1\
	t/merge-2-different\
	t/merge-2-identical\
	t/merge-multiple-conflicts\
	t/merge-multiple-files\
	t/merge-no-conflicts\
	t/merge-empty-head\
	t/merge-empty-base\
	t/merge-empty-tail\
	t/symmetric-1\
	t/symmetric-2-different\
	t/symmetric-2-identical\
	t/symmetric-3-different-head\
	t/symmetric-3-different-tail\
	t/symmetric-3-different-base\
	t/symmetric-3-all-different\
	t/symmetric-3-all-identical\
	t/symmetric-4-different-upper-middle\
	t/symmetric-4-different-lower-middle\
	t/symmetric-4-unique-different-middles\
	t/symmetric-4-same-different-middles\
	t/symmetric-4-identical\
	t/symmetric-4-different-head\
	t/symmetric-4-different-tail\
	t/symmetric-5-different-upper-middle\
	t/symmetric-5-different-lower-middle\
	t/symmetric-no-conflicts\
	t/symmetric-multiple-conflicts\
	t/symmetric-multiple-files\
	t/symmetric-3-empty-head\
	t/symmetric-3-empty-tail\
	t/symmetric-3-empty-head-tail\
	t/marker-size-10\
	t/marker-size-3\
	t/marker-size-shorter-ignored\
	t/marker-size-longer-ignored\
	t/mixed-marker-sizes\
	t/missing-newline-non-conflict\
	t/null-bytes\
	t/unclosed-conflict-head\
	t/unclosed-conflict-head-parent\
	t/unclosed-conflict-head-tail\
	t/unclosed-conflict-head-parent-tail\
	t/unexpected-separator-tail\
	t/unexpected-separator-parent\
	t/unexpected-separator-end\
	t/interactive-skip\
	t/interactive-edit\
	t/interactive-help\
	t/interactive-multiple-commands\
	t/interactive-two-files

THDR =\
	t/common.sh

all: git-rediff git-rediff.t
$(OBJ): $(HDR)
$(TOBJ): $(HDR)

.c.o:
	$(CC) -c -o $@ $< $(CFLAGS) $(CPPFLAGS)

.c.to:
	$(CC) -c -o $@ $< $(CFLAGS) $(CPPFLAGS) -DTEST

git-rediff: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

git-rediff.t: $(TOBJ)
	$(CC) -o $@ $(TOBJ) $(LDFLAGS)

check: git-rediff git-rediff.t $(TEST) $(THDR)
	@set -e;\
	for t in $(TEST); do\
		printf '%s\n' ./$$t >&2;\
		$(CHECK_PREFIX) ./$$t;\
	done

install: git-rediff
	mkdir -p -- "$(DESTDIR)$(PREFIX)/bin"
	mkdir -p -- "$(DESTDIR)$(MANPREFIX)/man1/"
	cp -- git-rediff "$(DESTDIR)$(PREFIX)/bin/"
	cp -- git-rediff.1 "$(DESTDIR)$(MANPREFIX)/man1/"

uninstall:
	-rm -f -- "$(DESTDIR)$(PREFIX)/bin/git-rediff"
	-rm -f -- "$(DESTDIR)$(MANPREFIX)/man1/git-rediff.1"

clean:
	-rm -f -- *.o *.a *.lo *.su *.so *.so.* *.gch *.gcov *.gcno *.gcda
	-rm -f -- git-rediff *.t *.to
	-rm -rf -- testdir.*/

.SUFFIXES:
.SUFFIXES: .o .to .c

.PHONY: all check install uninstall clean
