#!/bin/sh
# Remove CMake's in-source build artifacts.
#
# Uses `git clean -ffXd`, NOT `-fxd`:
#   -X  : remove ONLY ignored files. All build output is gitignored, so this
#         wipes the build while keeping every uncommitted change - tracked edits
#         AND any new untracked files you haven't added yet.
#   -ff : also clear nested git repos under ignored paths (e.g.
#         _deps/googletest-src), which plain `git clean` silently skips and
#         which otherwise linger as stale state (a problem when the tree is
#         rsync'd between machines).
#   -d  : recurse into untracked directories.
git clean -ffXd
rm -rf build/ tests/build
