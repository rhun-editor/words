# Words

Completes the word you are typing with the words of your open files, in
[rhun](https://rhun.app). The list comes up after two letters and a pause, or on `ctrl+space`;
the words used most come first, each with how often this file uses it, or which other open file
it comes from. **Count Distinct Words** in the palette tells you how many different words the
file has. A small C++17 program on
[rhunext.h](https://github.com/rhun-editor/extension-protocol/tree/main/c), one header with
nothing but the C library, and an example of rhun's completion.

Install it from rhun: the extensions button in the title bar, **Install from GitHub…**, then
`rhun-editor/words`. It starts with the first file you open.

A word is three bytes or more of letters, digits, `_` and non-ASCII characters, not starting with
a digit.

## Permissions

It reads the files you have open. It changes nothing itself (you pick the word), has no internet
and runs no other programs.

## Build

```sh
c++ -std=c++17 -O2 -o words main.cpp
tests/run.sh            # headless, against the rhun on your PATH
```

A tag `vX.Y.Z` equal to `version` in `extension.ini` builds Linux, macOS and Windows on their own
runners and publishes what Install from GitHub takes (section 11 of the spec).
