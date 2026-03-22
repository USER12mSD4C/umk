#!/bin/sh

# install.sh - установка утилиты umk

set -e

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_info() {
    echo "${GREEN}[INFO]${NC} $1"
}

print_error() {
    echo "${RED}[ERROR]${NC} $1" >&2
}

print_warning() {
    echo "${YELLOW}[WARNING]${NC} $1"
}

# Проверка наличия исходников
if [ ! -f "umk.c" ]; then
    print_error "umk.c not found in current directory"
    exit 1
fi

# Проверка наличия компилятора
if ! command -v gcc >/dev/null 2>&1; then
    print_error "gcc not found. Please install gcc first."
    exit 1
fi

# Парсим аргументы
PREFIX="/usr/local"
UNINSTALL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --prefix DIR    Install to DIR (default: /usr/local)"
            echo "  --uninstall     Uninstall umk"
            echo "  --help, -h      Show this help"
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

BIN_DIR="$PREFIX/bin"
MAN_DIR="$PREFIX/share/man/man1"
UMK_BIN="$BIN_DIR/umk"
MAN_PAGE="$MAN_DIR/umk.1"

if [ "$UNINSTALL" -eq 1 ]; then
    print_info "Uninstalling umk..."
    
    if [ -f "$UMK_BIN" ]; then
        rm -f "$UMK_BIN"
        print_info "Removed: $UMK_BIN"
    fi
    
    if [ -f "$MAN_PAGE" ]; then
        rm -f "$MAN_PAGE"
        print_info "Removed: $MAN_PAGE"
    fi
    
    print_info "Uninstall completed"
    exit 0
fi

# Компиляция
print_info "Compiling umk..."
gcc -O2 -Wall -Wextra -o umk umk.c

if [ ! -f "umk" ]; then
    print_error "Compilation failed"
    exit 1
fi

print_info "Compilation successful"

# Создание директорий
print_info "Creating directories..."
mkdir -p "$BIN_DIR"
mkdir -p "$MAN_DIR"

# Копирование бинарника
print_info "Installing umk to $BIN_DIR..."
cp umk "$UMK_BIN"
chmod 755 "$UMK_BIN"

# Создание man страницы
print_info "Creating man page..."
cat > "$MAN_PAGE" << 'EOF'
.\" Man page for umk
.TH UMK 1 "March 2026" "umk" "User Commands"
.SH NAME
umk \- simple build system
.SH SYNOPSIS
.B umk
\fI<command>\fR [\fIflags...\fR] [\fIoptions\fR]
.SH DESCRIPTION
\fBumk\fR is a simple build system with a clean syntax.
It reads configuration from \fBUMK\fR file in current directory.
.SH OPTIONS
.TP
\fB\-j\fR \fIN\fR
Run \fIN\fR jobs in parallel
.TP
\fB\-\-no\-color\fR
Disable colored output
.TP
\fB\-\-help\fR
Show help message
.SH COMMANDS
Commands are defined in \fBUMK\fR file. Example:
.RS
.PP
build:
    gcc main.c \-o app
    +flags:
        \-fg(preclean):
            rm \-f *.o
        eofg
    ;
eoc
.RE
.SH FLAGS
Flags are defined with \fB\-fg(name)\fR (before command) or \fB+fg(name)\fR (after command).
Use \fB\-\-name\fR to invoke them.
.SH VARIABLES
Variables can be defined with \fBNAME = value\fR and used with \fB$(NAME)\fR.
.SH BUILTIN FUNCTIONS
.TP
\fB$(wildcard pattern)\fR
Expand to list of files matching pattern
.TP
\fB$(shell command)\fR
Execute shell command and return output
.SH SPECIAL VARIABLES
.TP
\fB$@\fR
Target name in pattern rules
.TP
\fB$<\fR
First dependency in pattern rules
.TP
\fB$^\fR
All dependencies in pattern rules
.SH EXAMPLES
.PP
Build with flags:
.RS
umk build \-\-preclean \-\-postclean
.RE
.PP
Parallel build with 4 jobs:
.RS
umk build \-j 4
.RE
.PP
Disable colors:
.RS
umk build \-\-no\-color
.RE
.SH FILES
\fBUMK\fR \- configuration file in current directory
.SH AUTHOR
Written by umk developers
EOF

# Обновление базы man
if command -v mandb >/dev/null 2>&1; then
    mandb "$PREFIX/share/man" >/dev/null 2>&1 || true
fi

print_info "Installation completed!"
echo ""
echo "umk installed to: $UMK_BIN"
echo "Man page installed to: $MAN_PAGE"
echo ""
echo "To use:"
echo "  umk <command> [flags...]"
echo "  umk build --preclean --postclean"
echo "  umk clean"
echo ""
echo "For more info: man umk"
