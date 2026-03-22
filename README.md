# umk - Simple Build System

umk is a simple build system with clean syntax, variables, pattern rules, and parallel execution.

## Features

- Variables: `CC = gcc`, `CFLAGS = -Wall`
- Pattern rules: `%.o: %.c`
- Conditionals: `if $(DEBUG) == 1`
- Built-in functions: `$(wildcard *.c)`, `$(shell date)`
- Special variables: `$@`, `$<`, `$^`
- Parallel execution: `-j N`
- Command flags: `-fg(name)` (before), `+fg(name)` (after)
- Colored output

## Quick Start

- Create `UMK` file:

```
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

umk: umk.c
	gcc -O2 -Wall -Wextra -o umk umk.c

install: umk
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m755 umk $(DESTDIR)$(BINDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/umk

clean:
	rm -f umk

.PHONY: install uninstall clean
```

- Run:
```
umk build --preclean
```
## Installation:
```
git clone https://github.com/USER12mSD4C/umk
cd umk
make
sudo make install
```
