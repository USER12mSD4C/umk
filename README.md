# umk - Simple Build System

A lightweight, zero-dependency parallel build system with clean syntax, designed as a simpler, robust alternative to make for C/C++ and operating system projects.

## Features

- Simple syntax — clean and readable
- Variables — `CC = gcc`, `CFLAGS = -Wall`
- Pattern rules — `%.o: %.c` with paths (`kernel/%.o: kernel/%.c`)
- Automatic wildcard — `$(wildcard *.c)` finds all source files
- Built-in functions — `$(shell date)`, `$(wildcard *.c)`
- Special variables — `$@` (target), `$<` (first dep), `$^` (all deps)
- Parallel execution — `-j N` and `-jN`
- Command flags — `-fg(name)` (before), `+fg(name)` (after)
- Call targets — `call target` to invoke other rules
- Content hashing cache — tracks file modifications accurately by storing a 64-bit FNV-1a content hash database in `.umk_cache` instead of relying on fragile filesystem timestamps (`mtime`)
- Strict syntax validation — immediately reports formatting violations, unclosed blocks, and out-of-context commands with precise file names and line numbers
- Robust argument parsing — fully supports escaped backslashes (`\`), nested single quotes (`'`), and double quotes (`"`) inside command recipes
- Colored output
- Dry-run mode — `-n`
- Supports complex projects with multiple directories

## Command Line Options

| Option | Description |
|--------|-------------|
| `-j N`, `-jN` | Run N jobs in parallel |
| `--jobs N`, `--jobs=N` | Same as `-j N` |
| `--jobs auto` | Use the number of online CPUs |
| `-n`, `--dry-run` | Show commands without executing |
| `--no-color` | Disable colored output |
| `-h`, `--help` | Show help |
| `-v`, `--version` | Show version |

## Quick Start

Create a `UMK` file in your project:

```
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
```

Run:

```
umk build          # build the project
umk build --clean  # clean and rebuild
umk build -j 4     # parallel build
umk clean          # clean generated files
umk -n build       # dry-run
```

## Installation

### From Nix / NixOS

`umk` includes standard, non-flake Nix expressions for easy integration with the Nix ecosystem.

1. Build locally (creates a `result` symlink containing `./result/bin/umk`):
   ```bash
   nix-build
   ```

2. Add to NixOS configuration (`configuration.nix`):
   Import the project directory directly using `callPackage` inside your system package declaration:
   ```nix
   environment.systemPackages = with pkgs; [
     # ... other packages ...
     (callPackage /path/to/umk {})
   ];
   ```

### Manual

```
gcc -O3 -Wall -Wextra -o umk umk.c
sudo cp umk /usr/local/bin/
```

### From AUR (Arch Linux)

```
yay -S umk
# or
paru -S umk
```

## Syntax Reference

### Variables

```
CC = gcc
CFLAGS = -Wall
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
```

### Pattern Rules

```
# Basic
%.o: %.c
    $(CC) $(CFLAGS) -c -o $@ $<
eoc

# With directory
kernel/%.o: kernel/%.c
    $(CC) $(CFLAGS) -c -o $@ $<
eoc

# Multiple directories
drivers/%.o: drivers/%.c
    $(CC) $(CFLAGS) -c -o $@ $<
eoc
```

### Conditionals

```
if $(DEBUG) == 1
    CFLAGS = -g -O0
else
    CFLAGS = -O2
endif
```

### Commands with Flags

```
build:
    echo "Building..."
    +flags:
        -fg(preclean):
            echo "Pre-build cleanup"
        eofg
        +fg(postclean):
            echo "Post-build cleanup"
        eofg
    ;
eoc
```

Run with flags:

```
umk build --preclean --postclean
```

### Calling Targets

```
build:
    call kernel.bin
    call kom
    echo "All done"
eoc
```

### Special Variables

| Variable | Meaning |
|----------|---------|
| `$@` | Target name |
| `$<` | First dependency |
| `$^` | All dependencies |

### Built-in Functions

| Function | Description |
|----------|-------------|
| `$(wildcard pattern)` | List files matching pattern |
| `$(shell command)` | Execute command and return output |

## Command Line Options

| Option | Description |
|--------|-------------|
| `-j N`, `-jN` | Run N jobs in parallel |
| `-n, --dry-run` | Show commands without executing |
| `--no-color` | Disable colored output |

## Example: Full C Project

```
CC = gcc
CFLAGS = -Wall -Wextra -O2

SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

%.o: %.c
    $(CC) $(CFLAGS) -c -o $@ $<
eoc

app: $(OBJS)
    $(CC) -o $@ $^
eoc

build:
    call app
    echo "=== BUILD SUCCESS ==="
eoc

clean:
    rm -f *.o app
eoc
```

## Example: Operating System Project

```
AS = nasm
CC = gcc
LD = ld

ASFLAGS = -f elf64
CFLAGS = -m64 -ffreestanding -nostdlib -Iinclude
LDFLAGS = -m elf_x86_64 -T linker.ld -nostdlib

# Pattern rules for each directory
kernel/%.o: kernel/%.c
    $(CC) $(CFLAGS) -c -o $@ $<
eoc

drivers/%.o: drivers/%.c
    $(CC) $(CFLAGS) -c -o $@ $<
eoc

# Assembly files
kernel/entry.o: kernel/entry.asm
    $(AS) $(ASFLAGS) -o $@ $<
eoc

# Main kernel
kernel.bin: kernel/entry.o kernel/kernel.o drivers/vga.o
    $(LD) $(LDFLAGS) -o kernel.bin $^
eoc

# Build commands
build:
    call kernel.bin
    echo "=== BUILD COMPLETE ==="
    +flags:
        -fg(clean):
            call clean
            call kernel.bin
        eofg
    ;
eoc

clean:
    rm -rf *.o */*.o *.bin
eoc
```

## Content Hash Cache

umk uses a 128-bit UMK128 content hash.
The hash database is stored in `.umk_cache`.

umk does not use file timestamps for rebuild decisions.
If file content changes, umk rebuilds the target.

## Tests

Run the test suite from the project directory:

```sh
sh tests/run.sh ./umk
```

## Fedora

A Fedora spec file is available in `packaging/fedora/umk.spec`.

### Build a local RPM

Create a source tarball:

```sh
mkdir -p /tmp/umk-1.0.0/tests
cp umk.c umk.1 README.md LICENSE /tmp/umk-1.0.0/
cp tests/run.sh /tmp/umk-1.0.0/tests/
tar czf umk-1.0.0.tar.gz -C /tmp umk-1.0.0
```

Prepare the RPM build tree:

```sh
mkdir -p ~/rpmbuild/SOURCES ~/rpmbuild/SPECS
cp umk-1.0.0.tar.gz ~/rpmbuild/SOURCES/
cp packaging/fedora/umk.spec ~/rpmbuild/SPECS/
```

Build the RPM:

```sh
rpmbuild -bb ~/rpmbuild/SPECS/umk.spec
```

Install the RPM:

```sh
sudo dnf install ~/rpmbuild/RPMS/$(uname -m)/umk-1.0.0-1.*.rpm
```

### Build a source RPM

```sh
rpmbuild -bs ~/rpmbuild/SPECS/umk.spec
```

For an official Fedora package, the spec file should use the real
project URL and a valid Fedora packager identity in the changelog.

## License

MIT
