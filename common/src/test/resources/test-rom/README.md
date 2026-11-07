# CC: Tweaked test-rom (vendored)

This directory is a copy of CC: Tweaked's Lua test suite: `mcfly.lua`, `spec/`
and `data/`. The headless suite runner
(`common/src/test/java/io/github/mindbound/cclj/headless/CctSuiteRunner.java`,
Gradle task `:common:headlessSuite`) runs it against a real headless CC:T
computer.

## Origin

| | |
|---|---|
| Repository | <https://github.com/cc-tweaked/CC-Tweaked> |
| Tag | `v1.21.1-1.121.0` |
| Commit | `380c6fc623733950e113ca275df447dcc3c2ed69` |
| Path | `projects/core/src/test/resources/test-rom` |
| Files | 413 (this README not included) |

## Licence

- All files except `data/json-parsing/` are under the Mozilla Public License
  2.0, "The CC: Tweaked Developers". Each file carries its own SPDX copyright
  and licence header. The licence text is in `LICENSES/MPL-2.0.txt` at the
  repository root.
- `data/json-parsing/` (322 files) comes from
  [nst/JSONTestSuite](https://github.com/nst/JSONTestSuite) and is **MIT**,
  "2016 Nicolas Seriot". These files have no inline headers. CC:T declares
  their licence in its `REUSE.toml`, and the licence text is in
  `data/json-parsing/LICENSE`.
- This README is not part of the upstream suite. It is CCLuaJIT's own file
  (MIT, like the rest of the repository).
- `CctSuiteRunner.java`, which drives this suite, copies its fake peripherals
  and startup line from CC:T's `ComputerTestDelegate.java`, so that one file is
  MPL-2.0 too (see its header). Neither it nor this directory goes into the mod
  jar.

## Unmodified

The 413 upstream files are byte-for-byte unmodified. They were written
straight from the git blobs of the commit above, and every file's
`git hash-object --no-filters` matches the upstream blob ID. The root
`.gitattributes` marks this directory `-text`, so git never converts line
endings here. The golden tests under `spec/modules/cc/internal/syntax/`
compare whole files and fail on a CRLF checkout.

Do not edit these files. A behaviour that CCLuaJIT intentionally does
differently belongs in the runner's expected-failures list
(`common/src/test/resources/headless/expected-failures-<factory>.txt`), not
here.

## Updating

1. Sparse-clone the new CC:T tag, for example:
   `git clone --filter=blob:none --no-checkout --depth 1 --branch <tag> https://github.com/cc-tweaked/CC-Tweaked.git`,
   then `git sparse-checkout set --no-cone /projects/core/src/test/resources/test-rom/`.
   In Git Bash, set `MSYS_NO_PATHCONV=1` for that command. Otherwise the
   leading `/` becomes `C:/Program Files/Git/...` and nothing is checked out.
2. Write the files from the blobs (`git ls-tree -r HEAD <path>`, then
   `git cat-file blob <id>`), not from a working-tree checkout. On Windows,
   CC:T's own `* text=auto` rule turns LF into CRLF in a checkout.
3. Replace everything here except this README, and update the table above.
4. Run `./gradlew :common:headlessSuite` and update the expected-failures
   files.
