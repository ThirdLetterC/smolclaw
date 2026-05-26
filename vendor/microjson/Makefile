# Makefile for the microjson project
#
# SPDX-FileCopyrightText: (C) Eric S. Raymond <esr@thyrsus.com>
# SPDX-License-Identifier: BSD-2-Clause

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
DATADIR     ?= $(PREFIX)/share
MANDIR      ?= $(DATADIR)/man

CFLAGS = -O

# Add DEBUG_ENABLE for the tracing code
CFLAGS += -DDEBUG_ENABLE -g
# Add TIME_ENABLE to support RFC3339 time literals
CFLAGS += -DTIME_ENABLE

# The version for release is derived from the mostt recent stanza in the news file.
VERSION=$(shell sed -n <NEWS.adoc '/::/s/^\([0-9][^:]*\).*/\1/p' | head -1)

# Rules

# Note: to suppress the footers with timestamps being generated in HTML,
# we use "-a nofooter".
# To debug asciidoc problems, you may need to run "xmllint --nonet --noout --valid"
# on the intermediate XML that throws an error.
.SUFFIXES: .html .adoc .1

.adoc.1:
	asciidoctor -D. -a nofooter -b manpage $<
.adoc.html:
	asciidoctor -D. -a nofooter -a webfonts! $<

.PHONY: all clean cppcheck check spellcheck version dist release refresh

# Build

all: mjson.o test_microjson example1 example2 example3 example4

mjson.o: mjson.c mjson.h

test_microjson: test_microjson.o mjson.o
	$(CC) $(CFLAGS) -o test_microjson test_microjson.o mjson.o

# Worked examples.  These are essentially subsets of the regression test.
example1: example1.c mjson.c mjson.h
example2: example2.c mjson.c mjson.h
example3: example3.c mjson.c mjson.h
example4: example4.c mjson.c mjson.h

clean:
	rm -f *.o *.html test_microjson example[1234]

# Validate

SUPPRESSIONS = --suppress=unusedStructMember --suppress=unreadVariable
SUPPRESSIONS += -U__UNUSED__
cppcheck:
	@cppcheck --quiet -I. --template gcc --enable=all $(SUPPRESSIONS) *.[ch]

check: test_microjson
	@./test_microjson

spellcheck:
	spellcheck local.dic mjson.adoc

reflow:
	@clang-format --style="{IndentWidth: 8, UseTab: ForIndentation}" -i mjson.c

# Export

version:
	@echo $(VERSION)

DOCS = README.adoc COPYING NEWS.adoc control microjson.adoc mjson.adoc
SOURCES = Makefile *.[ch] $(DOCS)

microjson-$(VERSION).tar.gz: $(SOURCES)
	mkdir microjson-$(VERSION)
	cp -r $(SOURCES) microjson-$(VERSION)
	tar -czf microjson-$(VERSION).tar.gz microjson-$(VERSION)
	rm -fr microjson-$(VERSION)
	ls -l microjson-$(VERSION).tar.gz

dist: microjson-$(VERSION).tar.gz

release: microjson-$(VERSION).tar.gz microjson.html mjson.html
	shipper version=$(VERSION) | sh -e -x

refresh: microjson.html mjson.html
	shipper -N -w version=$(VERSION) | sh -e -x

# end
