# G1 run outputs (S1 step 6)

The logs behind [`reports/g1-evidence.md`](../../reports/g1-evidence.md): gate G1 of M1.md §3.16 on commit 5381504, run on 2026-11-09. Machine paths are replaced by `<repo>` (the checkout), `<scratch>` (the session's scratch folder) and `<home>`.

| Folder | What |
|---|---|
| `ci/` | The job logs of CI run 42593991784 that the report uses (`natives-linux.log`, `natives-windows.log`, `mod.log`, `headless-windows.log`, from `gh run view --job <id> --log`), and `run-jobs.json`, the output of `gh run view 42593991784 --json jobs` (each job's conclusion, start and end time). |
| `runs/` | The scripts of the local runs and what each printed, one pair per phase: `win-a`, `alma-a` (release builds, lint, Tier 0, scan self-test; Alma also Tier 1), `win-b`, `ubuntu-b` (Gradle tasks on Windows, Tier 1 on Ubuntu, the ns per call), `negctl-win`, `negctl-alma` (the negative control), `alma-c`, `ubuntu-c` and their second runs `alma-c2`, `ubuntu-c2` (Tier 2 under WSL), `ubuntu-t0-release` (Tier 0 with ASan and UBSan), `win-d`, `alma-d`, `ubuntu-d` (the `CCLJ_DEBUG=1` builds), `alma-e` (the Linux release build again). `common.sh`, `probe-linux.sh`, `specs-g1.sh`, `linux-c.sh`, `ubuntu-t0.sh`, `negctl.py` and `print-cp.gradle` are helpers they call. They are records: with the placeholder paths they do not run as they are. |
| `logs/` | The full log of each step: builds (`*-build-*.log`), lint, Tier 0 (`*-tier0*.log`), Tier 1 (`*-tier1*.log`, and the Gradle task logs under `win-gradle-*/logs/`), Tier 2 (`specs-*.log` and `*.tsv`), and the negative control. The two negative-control logs were 1.1 GB each; `negctl-alma-head.log` and `negctl-win-task-head.log` keep their first 100 KB. |
