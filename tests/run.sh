#!/bin/sh
# Builds the extension for this machine, lays it out as rhun installs it, and runs each
# tests/NAME.txt headlessly against a rhun binary, comparing what it prints with tests/NAME.expected:
#   basic   the list after two letters, words from another open file, Count Distinct Words
#   items   protocol 1.1 (complete-items): the selected item's uses (resolve), Shift+Enter over the
#           rest of the word (the replace range), a commit character from the extension's setting
# usage: tests/run.sh [RHUN [NAME...]]   (UPDATE=1 rewrites the .expected files)
set -e
cd "$(dirname "$0")/.."
RHUN=${1:-$(command -v rhun || echo "$HOME/.local/bin/rhun")}
[ $# -gt 0 ] && shift
names=${*:-basic items}
name=$(sed -n 's/^name *= *//p' extension.ini | head -1)
owner=$(sed -n 's/^owner *= *//p' extension.ini | head -1)
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) target=linux-x86_64 ;;
    Darwin-arm64) target=macos-arm64 ;;
    *) echo "no target for $(uname -s)-$(uname -m)"; exit 77 ;;
esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
dir=$work/exts/$owner/$name
mkdir -p "$dir/bin"
c++ -std=c++17 -O2 -o "$dir/bin/$name-$target" main.cpp
cp extension.ini icon.png "$dir/"
fail=0
for test in $names; do
    t=$work/$test
    mkdir -p "$t/proj" "$t/config/rhun" "$t/state"
    printf 'apple banana apple\ncherry apple banana\napricot\n' > "$t/proj/sample.txt"
    printf 'apex apex grape\n' > "$t/proj/other.txt"
    printf '[files]\nrestore_session = false\nrestore_project = false\n[updates]\ncheck = false\n[git]\nenabled = false\n[ui]\nsidebar = false\nagents_panel = false\n' > "$t/config/rhun/config"
    [ "$test" = items ] && printf '[%s/%s]\nsetting.commit_characters = . ,\n' "$owner" "$name" > "$t/config/rhun/extensions.ini"
    sed "s|proj/|$t/proj/|" "tests/$test.txt" > "$t/$test.txt"
    HOME=$t XDG_CONFIG_HOME=$t/config XDG_STATE_HOME=$t/state \
        "$RHUN" "$t/proj" --headless 1000x600 --scale 1 --extensions "$work/exts" --script "$t/$test.txt" 2>&1 \
        | sed 's/pid=[1-9][0-9]*/pid=PID/' > "$t/out.txt"
    [ -n "$UPDATE" ] && cp "$t/out.txt" "tests/$test.expected"
    if cmp -s "$t/out.txt" "tests/$test.expected"; then
        echo "ok   $test"
    else
        echo "FAIL $test"; diff "tests/$test.expected" "$t/out.txt" || true; fail=1
    fi
done
exit $fail
