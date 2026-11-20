# Contributing to sqedit

Thank you for looking. sqedit is part of the
[squared SDK](https://squared-netizen.github.io/squared-netizen/),
maintained by one person, so a short note first saves everyone time.

- **Something large** (a new feature, a change to a format or an API):
  open an issue describing it before writing the code.
- **A fix or a small change**: a pull request from a branch is welcome.
  Say what it changes and why, and how you checked it.
- **A security issue**: never in public. See [SECURITY.md](SECURITY.md).

## Before a pull request

```sh
make -j4 test CXX=/path/to/clang++   # clang and libc++ 19+ (23 in CI)
```

`make test` runs the engine's tests (`tests/test_peg.cpp`) and the
commands end to end (`tests/run_check.sh`), each compared with the sed or
grep call it replaces where those are installed. A change to the pattern
language comes with a test in `test_peg.cpp`; a change to a command, a
check in `run_check.sh`. Neither the build nor the tests may call sed,
grep or awk: sqedit is built before they are there.

The pull request's CI must be green before it is merged. Builds are
offline: nothing in a build may download anything.

## Licence

sqedit is under the MIT licence ([LICENSE](LICENSE)). By contributing,
you agree that your contribution is licensed under it too.
