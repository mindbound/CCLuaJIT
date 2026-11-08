"""Extract the CC:T 1.121.0 ROM (bios.lua and rom/) from the CC:Tweaked core jar.

The BIOS and ROM are CC:T code under LicenseRef-CCPL / MPL-2.0, so this MIT repository does not contain them.
The harnesses that run ROM code read it at run time from a directory this script fills:

- pause-prototype/tests/lib/cc.lua reads os.pullEvent(Raw) from bios.lua, and the parallel API (with cc.expect and
  cc.internal.exception) from rom/. Its directory is $CCT_ROM; pause-prototype/run_tests.sh and
  pause-prototype/linux/run-linux.sh default it to cct-rom/ next to this script.
- harnesses/version-survey/mini_cc.lua reads the bit stub from bios.lua and serves rom/ to the ROM's cc.require.
  Its directory is <ROOT>/rom-1.121.0, where ROOT is the survey's scratch workspace outside the repository.

    python -I extract_cct_rom.py <cc-tweaked-1.21.1-core-1.121.0.jar> cct-rom      (from this directory)

<dir> must not exist yet or be empty, so that files from two CC:T versions cannot mix. The jar must be 1.121.0,
which the harnesses were written for (checked from rom/help/changelog.md). git ignores every cct-rom/ and
rom-1.121.0/ directory under this one; keep any other <dir> outside the repository.
(The core jar is in the Gradle cache after a dev build, or at
 https://maven.squiddev.cc/cc/tweaked/cc-tweaked-1.21.1-core/1.121.0/.)
"""
import os
import re
import sys
import zipfile

PREFIX = "data/computercraft/lua/"
REQUIRED = ["bios.lua", "rom/apis/parallel.lua", "rom/modules/main/cc/internal/tiny_require.lua",
            "rom/modules/main/cc/expect.lua", "rom/modules/main/cc/internal/exception.lua"]
VERSION_FILE, VERSION_LINE = "rom/help/changelog.md", "# New features in CC: Tweaked 1.121.0"
# The same test as cc.lua and mini_cc.lua make: the first line, ended by LF or CRLF.
VERSION_RE = re.compile(re.escape(VERSION_LINE.encode()) + rb"\r?\n")


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: python -I extract_cct_rom.py <cc-tweaked-1.21.1-core-1.121.0.jar> <dir>")
    jar, out = sys.argv[1], os.path.abspath(sys.argv[2])
    if os.path.exists(out) and (not os.path.isdir(out) or os.listdir(out)):
        sys.exit("%s exists and is not an empty directory: delete it first" % out)
    try:
        z = zipfile.ZipFile(jar)
    except (OSError, zipfile.BadZipFile) as e:
        sys.exit("cannot read %s as a jar: %s" % (jar, e))
    with z:
        entries = [n for n in z.namelist() if n.startswith(PREFIX) and not n.endswith("/")]
        # Check everything before writing anything.
        names = set(n[len(PREFIX):] for n in entries)
        missing = [r for r in REQUIRED + [VERSION_FILE] if r not in names]
        if missing:
            sys.exit("%s has no %s%s: is it the CC:T core jar?" % (jar, PREFIX, missing[0]))
        changelog = z.read(PREFIX + VERSION_FILE)
        if not VERSION_RE.match(changelog):
            first = changelog.split(b"\n", 1)[0].decode("utf-8", "replace")
            sys.exit("%s is not CC:T 1.121.0 (%s starts with %r)" % (jar, VERSION_FILE, first))
        for name in entries:
            # Entry names come from the jar: write only plain relative paths inside <dir>.
            if any(p in ("", ".", "..") or ":" in p or "\\" in p for p in name[len(PREFIX):].split("/")):
                sys.exit("refusing jar entry %r" % name)
        for name in entries:
            path = os.path.join(out, *name[len(PREFIX):].split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(z.read(name))
    print("wrote %d files to %s" % (len(entries), out))


if __name__ == "__main__":
    main()
