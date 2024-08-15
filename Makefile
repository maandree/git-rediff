.POSIX:

CONFIGFILE = config.mk
include $(CONFIGFILE)

OBJ =\
	git-rediff.o

HDR =

all: git-rediff
$(OBJ): $(HDR)

.c.o:
	$(CC) -c -o $@ $< $(CFLAGS) $(CPPFLAGS)

git-rediff: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

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
	-rm -f -- git-rediff

.SUFFIXES:
.SUFFIXES: .o .c

.PHONY: all install uninstall clean
