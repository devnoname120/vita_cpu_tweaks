#!/bin/sh
# Builds the handout zip a tester gets, from tracked inputs plus the current
# build. Output lands in dist/, which is not tracked.
#
#   tools/make_tester_zip.sh
#
# Stamps the README with the commit and the skprx's sha256 so a tester's log
# can always be tied back to an exact build.
set -e
cd "$(dirname "$0")/.."

skprx=build/prefetch_enabler.skprx
[ -f "$skprx" ] || { echo "no $skprx -- build first" >&2; exit 1; }

commit=$(git rev-parse --short HEAD)
dirty=$(git status --porcelain --untracked-files=no)
[ -z "$dirty" ] || commit="$commit-dirty"
sum=$(shasum -a 256 "$skprx" | cut -d' ' -f1)

stage=dist/for-testing
rm -rf "$stage"
mkdir -p "$stage"
cp "$skprx" "$stage/prefetch_enabler.skprx"
cp docs/for-testers.config.txt "$stage/prefetch_enabler.txt"
sed -e "s/^build:  .*/build:   $commit/" \
    -e "s/^sha256: .*/sha256:  $sum/" \
    docs/for-testers.md > "$stage/README-FOR-TESTERS.md"

out="dist/prefetch_enabler-test-$commit.zip"
rm -f "$out"
(cd dist && zip -q -r "../$out" for-testing)
echo "$out"
echo "  build  $commit"
echo "  sha256 $sum"
