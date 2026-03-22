# umk - Simple Build System

A lightweight build system with clean syntax, designed as a simpler alternative to make for C/C++ projects.

## Features

- **Simple syntax** - clean and readable
- **Variables** - `CC = gcc`, `CFLAGS = -Wall`
- **Pattern rules** - `%.o: %.c` with paths (`kernel/%.o: kernel/%.c`)
- **Automatic wildcard** - `$(wildcard *.c)` finds all source files
- **Conditionals** - `if $(DEBUG) == 1` / `else` / `endif`
- **Built-in functions** - `$(shell date)`, `$(wildcard *.c)`
- **Special variables** - `$@` (target), `$<` (first dependency), `$^` (all dependencies)
- **Parallel execution** - `-j N` for faster builds
- **Command flags** - `-fg(name)` (before main command), `+fg(name)` (after main command)
- **Call targets** - `call target` to invoke other build rules
- **Timestamp checking** - rebuilds only when files change
- **Colored output** - errors in red
- **Dry-run mode** - `-n` shows what would be executed

## Quick Start

Create a `UMK` file in your project:

```umk
# Variables
CC = gcc
CFLAGS = -Wall -Wextra

# Pattern rule for C files
%.o: %.c
    $(CC) $(CFLAGS) -c -o $@ $<
eoc

# Main target
app: main.o helper.o
    $(CC) -o app $^
eoc

# Build command
build:
    call app
    echo "=== BUILD COMPLETE ==="
    +flags:
        -fg(clean):
            call clean
            call app
        eofg
    ;
eoc

# Clean command
clean:
    rm -f *.o app
eoc
