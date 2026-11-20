#!/bin/sh
# tests/run_check.sh SQEDIT: sqedit's commands, end to end.
#
# Each row of the design's commands table, on files shaped like the ones
# the sed calls it replaces read: its output as written here, and, where sed
# and grep are on this machine, the same as theirs (the design's phase 2
# gate). Neither is needed to run it: on a phone without them, the expected
# outputs alone are checked.
set -u
SQEDIT=${1:?usage: run_check.sh path/to/sqedit}
case $SQEDIT in /*) ;; *) SQEDIT=$PWD/$SQEDIT ;; esac
T=${TMPDIR:-/tmp}/sqedit-check.$$
mkdir -p "$T" || exit 2
trap 'rm -r -f -- "$T"' EXIT
cd "$T" || exit 2

failed=0
pass=0
ok() { pass=$((pass + 1)); }
bad() { failed=$((failed + 1)); printf 'FAIL %s\n' "$1" >&2; }
# same NAME EXPECTED ACTUAL
same() { if [ "$2" = "$3" ]; then ok; else bad "$1"; printf '  expected: %s\n  actual:   %s\n' "$2" "$3" >&2; fi; }
# status NAME EXPECTED COMMAND...
status() { name=$1 want=$2; shift 2; "$@" >/dev/null 2>&1; got=$?; same "$name (exit status)" "$want" "$got"; }
have() { command -v "$1" >/dev/null 2>&1; }

# --- the files ---------------------------------------------------------------
printf 'llvm: 23.1.0\nlibc++: 23.1.0+r1\nndk: r28\n' > PINNED
cat > script.sh <<'EOF'
#!/usr/bin/env bash
# script.sh: does a thing.
#
#   script.sh [--fast] FILE
#
# Options are read once.
set -euo pipefail
echo "the work"
EOF
cat > manifest.json <<'EOF'
{
  "format": "squared-cartridge",
  "version": "1.2.3",
  "consumers": { "sqterm": { "key": "abc", "links": ["a", "b"] } }
}
EOF
cat > hostid.c <<'EOF'
long get(void) {
  long id = gethostid ();
  return id;
}
EOF
cat > exec.c <<'EOF'
int run(char **argv) { return execve("/bin/sh", argv, 0); }
EOF
cat > setgid.c <<'EOF'
int drop(void) {
    if (setgid(getgid()) != 0)
        return -1;
    return 0;
}
EOF
cat > classes.cpp <<'EOF'
class Widget {};
struct Point {};
class Window : Widget {};
EOF

# --- pick: take a field --------------------------------------------------------
got=$("$SQEDIT" pick '"libc++: " {.*}' PINNED)
same "pick a field" "23.1.0+r1" "$got"
have sed && same "pick = sed -n s///p" "$(sed -n 's/^libc++: //p' PINNED)" "$got"
got=$("$SQEDIT" pick '{%d+} "." {%d+}' PINNED | head -n 1)
same "pick two captures, tab-separated" "$(printf '23\t1')" "$got"

# --- json ------------------------------------------------------------------------
same "json a top-level string" "1.2.3" "$("$SQEDIT" json version manifest.json)"
have sed && same "json = sed on the version line" \
    "$(sed -n 's/^  "version": "\([^"]*\)".*/\1/p' manifest.json)" "$("$SQEDIT" json version manifest.json)"
same "json a dotted path" "abc" "$("$SQEDIT" json consumers.sqterm.key manifest.json)"
same "json an array element" "b" "$("$SQEDIT" json consumers.sqterm.links.1 manifest.json)"
same "json an object, as JSON" '["a","b"]' "$("$SQEDIT" json consumers.sqterm.links manifest.json)"
status "json a path that is not there" 1 "$SQEDIT" json consumers.nothing manifest.json
same "json from standard input" "1.2.3" "$("$SQEDIT" json version < manifest.json)"

# --- lines -----------------------------------------------------------------------
got=$("$SQEDIT" lines 2..5 script.sh)
same "lines 2..5" "$(printf '# script.sh: does a thing.\n#\n#   script.sh [--fast] FILE\n#')" "$got"
have sed && same "lines = sed -n 2,5p" "$(sed -n '2,5p' script.sh)" "$got"
got=$("$SQEDIT" lines '2.."set -euo"' script.sh)
have sed && same "lines to a pattern = sed -n 2,/^set -euo/p" "$(sed -n '2,/^set -euo/p' script.sh)" "$got"
same "lines to a pattern ends on it" "set -euo pipefail" "$(printf '%s\n' "$got" | tail -n 1)"
same "lines 7" "set -euo pipefail" "$("$SQEDIT" lines 7 script.sh)"
same "lines ..\$ from the start" "$(cat script.sh)" "$("$SQEDIT" lines '..$' script.sh)"
same "lines 8.." 'echo "the work"' "$("$SQEDIT" lines 8.. script.sh)"
status "lines past the end" 1 "$SQEDIT" lines 40..50 script.sh

# --- sub -------------------------------------------------------------------------
cp hostid.c hostid.sed.c
"$SQEDIT" sub --in-place --expect 1 '"id = gethostid ();"' 'id = 0;' hostid.c
same "sub --in-place" "  long id = 0;" "$("$SQEDIT" lines 2 hostid.c)"
if have sed; then
    sed -i 's|id = gethostid ();|id = 0;|' hostid.sed.c
    same "sub = sed -i s|||" "$(cat hostid.sed.c)" "$(cat hostid.c)"
fi
cp hostid.c before.c
status "sub --expect: a count that is not N fails" 1 "$SQEDIT" sub --in-place --expect 1 '"gethostid"' 'x' before.c
same "sub --expect wrote nothing" "$(cat hostid.c)" "$(cat before.c)"
"$SQEDIT" sub -F --in-place --expect 1 'execve("/bin/sh", ' 'execve("/system/bin/sh", ' exec.c
same "sub -F: a literal, no escaping" 'int run(char **argv) { return execve("/system/bin/sh", argv, 0); }' "$(cat exec.c)"
same "sub to standard output" "x 2 y 2" "$(printf 'x 1 y 1\n' | "$SQEDIT" sub '"1"' '2')"
same "sub with a named capture" "1.2.3 -> v1.2.3" \
    "$(printf '1.2.3\n' | "$SQEDIT" sub '{:v: %d+ ("." %d+)* :}' '{v} -> v{v}')"
same "sub with numbered captures" "b=a" "$(printf 'a=b\n' | "$SQEDIT" sub '{%a+} "=" {%a+}' '{2}={1}')"
same "sub {0} and braces" "[x] {x}" "$(printf 'x\n' | "$SQEDIT" sub '"x"' '[{0}] {{{0}}}')"
same "sub --whole across lines" "a b" "$(printf 'a\nb' | "$SQEDIT" sub --whole '%nl' ' ')"
printf 'keep\n' > nonl.txt && printf 'no newline' >> nonl.txt
"$SQEDIT" sub --in-place '"keep"' 'kept' nonl.txt
same "sub keeps a missing last newline" "$(printf 'kept\nno newline')" "$(cat nonl.txt)"
status "sub with no match" 1 "$SQEDIT" sub '"absent"' 'x' hostid.c

# --- wrap ------------------------------------------------------------------------
cp setgid.c setgid.sed.c
"$SQEDIT" wrap --in-place --expect 1 --lines 2 '"    if (setgid(getgid()) != 0)"' '#if !defined(__ANDROID__)' '#endif' setgid.c
want=$(printf 'int drop(void) {\n#if !defined(__ANDROID__)\n    if (setgid(getgid()) != 0)\n        return -1;\n#endif\n    return 0;\n}')
same "wrap --lines 2" "$want" "$(cat setgid.c)"
if have sed; then
    sed -i '/^    if (setgid(getgid()) != 0)/{i\
#if !defined(__ANDROID__)
n;a\
#endif
}' setgid.sed.c
    same "wrap = sed -i /re/{i\\ n;a\\}" "$(cat setgid.sed.c)" "$(cat setgid.c)"
fi

# --- match -----------------------------------------------------------------------
got=$("$SQEDIT" match '"class " %w+' classes.cpp)
same "match lines" "$(printf 'class Widget {};\nclass Window : Widget {};')" "$got"
have grep && same "match = grep -E" "$(grep -E 'class [[:alnum:]]+' classes.cpp)" "$got"
same "match -o" "$(printf 'class Widget\nclass Window')" "$("$SQEDIT" match -o '"class " %w+' classes.cpp)"
same "match -c" "2" "$("$SQEDIT" match -c '"class "' classes.cpp)"
same "match -v" "struct Point {};" "$("$SQEDIT" match -v '"class "' classes.cpp)"
same "match -n" "2:struct Point {};" "$("$SQEDIT" match -n '"struct"' classes.cpp)"
status "match -q, a match" 0 "$SQEDIT" match -q '"Point"' classes.cpp
status "match -q, none" 1 "$SQEDIT" match -q '"Nothing"' classes.cpp
status "match -x, the whole line" 0 "$SQEDIT" match -qx '"struct Point {};"' classes.cpp
status "match -x, not a prefix" 1 "$SQEDIT" match -qx '"struct"' classes.cpp
status "match -xF" 0 "$SQEDIT" match -qxF 'struct Point {};' classes.cpp
same "match two files names them" "classes.cpp:struct Point {};" "$("$SQEDIT" match '"Point"' classes.cpp hostid.c)"
same "match from standard input" "b" "$(printf 'a\nb\n' | "$SQEDIT" match '"b"')"
same "match with a grammar sqedit ships" '  "version": "1.2.3",' "$("$SQEDIT" match -g json "'\"version\":' %s* string" manifest.json)"
same "pick with a shipped grammar" '"1.2.3"' "$("$SQEDIT" pick -g json "'\"version\":' %s* {string}" manifest.json)"
printf 'name = sqedit\nversion: 0.1.0\n' > config.ini
same "pick with ini.peg's named captures" "$(printf 'name\tsqedit')" "$("$SQEDIT" pick -g ini 'pair' config.ini | head -n 1)"

# --- -x, and newlines in a replacement (squared's manifest edits) ---------------------
cat > AndroidManifest.xml <<'EOF'
<manifest>
    <application
        android:hasCode="false">
    </application>
</manifest>
EOF
got=$("$SQEDIT" sub -x '{[ 	]*} "<application" {[ 	]*}' '{1}<uses-permission android:name="android.permission.INTERNET" />\n{1}<application{2}' AndroidManifest.xml)
want=$(printf '<manifest>\n    <uses-permission android:name="android.permission.INTERNET" />\n    <application\n        android:hasCode="false">\n    </application>\n</manifest>')
same "sub -x with \\n: a line put before another" "$want" "$got"
have sed && same "sub -x \\n = sed's \\n insert" \
    "$(sed 's|^\([[:space:]]*\)<application\([[:space:]]*\)$|\1<uses-permission android:name="android.permission.INTERNET" />\n\1<application\2|' AndroidManifest.xml)" "$got"
status "sub -x: not the whole line, no change" 1 "$SQEDIT" sub -x '"application"' 'x' AndroidManifest.xml
same "sub \\t and \\\\" "$(printf 'a\tb\\c')" "$(printf 'a b c\n' | "$SQEDIT" sub '" b "' '\tb\\')"
same "pick -x: the value of a key=value line" "s3cr3t" "$(printf 'port=1\nsecret=s3cr3t\n' | "$SQEDIT" pick -x '"secret=" {.*}')"
have sed && same "pick -x = sed -n s/^secret=//p" "$(printf 'port=1\nsecret=s3cr3t\n' | sed -n 's/^secret=//p')" \
    "$(printf 'port=1\nsecret=s3cr3t\n' | "$SQEDIT" pick -x '"secret=" {.*}')"
same "pick -x: not inside a line" "" "$(printf 'xsecret=1\n' | "$SQEDIT" pick -x '"secret=" {.*}')"
same "pick -x: the last field" "192.168.1.5:37000" \
    "$(printf 'name _adb-tls-connect._tcp 192.168.1.5:37000\n' | "$SQEDIT" pick -x '(%S* %s+)* {%S+} %s*')"

# --- errors ------------------------------------------------------------------------
err=$("$SQEDIT" match '"unclosed' classes.cpp 2>&1); st=$?
same "a pattern error: status 2" 2 "$st"
same "a pattern error: where" "sqedit: pattern:1:1: expected a closing quote" "$err"
status "no command" 2 "$SQEDIT" nothing
status "an option that is not one" 2 "$SQEDIT" match --nothing '"x"' classes.cpp
status "a file that is not there" 2 "$SQEDIT" match '"x"' absent.txt
status "--in-place needs a file" 2 "$SQEDIT" sub --in-place '"a"' 'b'
printf 'ok \377\n' > bad.txt
status "--strict refuses a file that is not UTF-8" 2 "$SQEDIT" match --strict '"ok"' bad.txt
status "without --strict it is read" 0 "$SQEDIT" match '"ok"' bad.txt
status "exponential backtracking stops" 2 sh -c "printf '%040d\n' 0 | \"$SQEDIT\" match 's <- \"0\" s \"1\" / \"0\" s \"2\" / \"0\"'"

if [ "$failed" -ne 0 ]; then
    printf '%d of %d checks failed\n' "$failed" "$((failed + pass))" >&2
    exit 1
fi
printf 'ok   commands (%d checks%s)\n' "$pass" "$(have sed || printf ', without sed')"
