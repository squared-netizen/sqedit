# sqedit

A text tool built on parsing expression grammars, in the spirit of LPeg, to
replace sed and grep in scripts: readable patterns, literals with no
escaping, and a count that is checked rather than hoped for. Part of the
[squared SDK](https://squared-netizen.github.io/squared-netizen/).

| Job | sed or grep | sqedit |
| --- | --- | --- |
| Take a field | `sed -n 's/^libc++: //p' PINNED` | `sqedit pick '"libc++: " {.*}' PINNED` |
| A field from JSON | `sed -n 's/^  "version": "\([^"]*\)".*/\1/p'` | `sqedit json version manifest.json` |
| Print lines | `sed -n '2,29p' "$0"` | `sqedit lines 2..29 "$0"` |
| Lines to a pattern | `sed -n '2,/^set -euo/p'` | `sqedit lines '2.."set -euo"' "$0"` |
| Change a line | `sed -i 's\|id = gethostid ();\|id = 0;\|' f; grep -q 'id = 0;' f` | `sqedit sub --in-place --expect 1 '"id = gethostid ();"' 'id = 0;' f` |
| Literal, no pattern | (escape by hand) | `sqedit sub -F --in-place --expect 1 'execve("/bin/sh", ' 'execve("/system/bin/sh", ' f` |
| Wrap a line | `sed -i '/^    if (setgid/{i\ ... n;a\ ...}' f` | `sqedit wrap --in-place --expect 1 --lines 2 '"    if (setgid(getgid()) != 0)"' '#if !defined(__ANDROID__)' '#endif' f` |
| Find lines | `grep -E`, `grep -q`, `grep -c` | `sqedit match '"class " %w+' f` (`-o -c -q -v -x -n`) |

`--expect N` makes a command that changes a file fail, with nothing written,
when the file holds a different number of matches than the script assumes.
Exit status as grep's: 0 matched (or changed as expected), 1 no match or a
count other than `--expect`, 2 a usage, pattern or file error, reported with
where it is (`sqedit: pattern:1:14: expected a closing quote`).

## Patterns

Parsing expression grammars, written as LPeg's `re` module writes them. A
literal is quoted text and means only itself; everything unquoted is pattern.

| Write | Matches |
| --- | --- |
| `"text"` or `'text'` | exactly that text |
| `.` | any one character (a UTF-8 code point, not a byte) |
| `[a-z_]`, `[^"]` | one character in, or not in, a set |
| `p q` | p, then q |
| `p / q` | p; if p fails, q (ordered: the first that matches wins) |
| `p*`, `p+`, `p?` | repetition, greedy and possessive (no backtracking into it) |
| `&p`, `!p` | p must, or must not, follow; nothing is consumed |
| `{p}` | capture what p matched |
| `{:name: p :}` | a named capture |
| `{~ p ~}` | a substitution capture: the match, with the parts inside marked `-> 'new'` replaced |
| `name <- p` | a rule; rules make a grammar, and may call each other and themselves |
| `%nl`, `%s`, `%d`, `%w` | newline, space, digit, alphanumeric (and `%a %l %u %x %p %c %g`; `%S` and the like, their complements) |

A match is anchored where it starts, as in every PEG; the commands search
for the first place a pattern matches, line by line or, with `--whole`,
across the file. In `sub`'s replacement, `{name}` is a named capture, `{1}`
the first capture, `{0}` the whole match.

`-g file.peg` makes a file's rules usable by name in a pattern. sqedit ships
`json` (a string and a number), `c` (a string literal, a comment, an
include) and `ini` (a `key: value` line): `-g json`. JSON in a JSON file is
`sqedit json`'s job, not a grammar's.

## Build

```sh
CXX=/path/to/clang++ sqmake test
```

clang and libc++ 19 or later (23 is what sqterm and CI use), C++23 with
`import std;`. Offline, and with no sed, grep or awk anywhere in the build:
sqedit is built before anything uses it. The matching is a library
(`sqedit.peg`, `build/libsqedit.a`); the command is a thin front end to it.

The design, decided 2026-11-18, is in "sqedit: design".

MIT licence.
