#!/bin/sh
set -eu

UMK=${1:-./umk}

case "$UMK" in
    /*)
        ;;
    *)
        UMK="$PWD/$UMK"
        ;;
esac

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

START=$PWD

# Test 1: simple rule builds a file.
mkdir "$ROOT/01-rule"
cd "$ROOT/01-rule"

cat > UMK <<'EOF'
a: a.c
    touch a
eoc
EOF

touch a.c
"$UMK" a
test -f a

cd "$START"

# Test 2: dry-run does not create the target.
mkdir "$ROOT/02-dry"
cd "$ROOT/02-dry"

cat > UMK <<'EOF'
a: a.c
    touch a
eoc
EOF

touch a.c
"$UMK" -n a
test ! -f a

cd "$START"

# Test 3: hash cache prevents unnecessary rebuild.
mkdir "$ROOT/03-cache"
cd "$ROOT/03-cache"

cat > UMK <<'EOF'
out.txt: in.txt
    cp in.txt out.txt
    echo run >> log.txt
eoc
EOF

echo x > in.txt

"$UMK" out.txt
[ "$(wc -l < log.txt)" -eq 1 ]

"$UMK" out.txt
[ "$(wc -l < log.txt)" -eq 1 ]

echo y > in.txt

"$UMK" out.txt
[ "$(wc -l < log.txt)" -eq 2 ]

cd "$START"

# Test 4: command target always runs.
mkdir "$ROOT/04-phony"
cd "$ROOT/04-phony"

cat > UMK <<'EOF'
cmd:
    echo run >> cmd.log
eoc
EOF

"$UMK" cmd
"$UMK" cmd
[ "$(wc -l < cmd.log)" -eq 2 ]

cd "$START"

# Test 5: command dependency forces rebuild.
mkdir "$ROOT/05-command-dep"
cd "$ROOT/05-command-dep"

cat > UMK <<'EOF'
gen:
    echo gen >> gen.log
eoc

out: gen
    touch out
    echo run >> out.log
eoc
EOF

"$UMK" out
[ "$(wc -l < out.log)" -eq 1 ]

"$UMK" out
[ "$(wc -l < out.log)" -eq 2 ]

cd "$START"

# Test 6: command flags work.
mkdir "$ROOT/06-flags"
cd "$ROOT/06-flags"

cat > UMK <<'EOF'
build:
    echo base >> build.log
    +flags:
        -fg(extra):
            echo extra >> build.log
        eofg
    ;
eoc
EOF

"$UMK" build
[ "$(wc -l < build.log)" -eq 1 ]

"$UMK" build --extra
[ "$(wc -l < build.log)" -eq 3 ]

cd "$START"

# Test 7: parallel jobs build dependencies.
mkdir "$ROOT/07-parallel"
cd "$ROOT/07-parallel"

cat > UMK <<'EOF'
a: a.c
    touch a
eoc

b: b.c
    touch b
eoc

all: a b
    touch all
eoc
EOF

touch a.c b.c

"$UMK" -j2 all

test -f a
test -f b
test -f all

cd "$START"

# Test 8: variable expansion works.
mkdir "$ROOT/08-vars"
cd "$ROOT/08-vars"

cat > UMK <<'EOF'
MSG = hello
show:
    echo $(MSG) >> show.log
eoc
EOF

"$UMK" show
grep -q hello show.log

cd "$START"

echo "all tests passed"
