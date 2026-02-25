# Build

Build from the repo root with:

```bash
cd /home/delahantyj@hhmi.org/gitrepos/crimson/build && make -j$(nproc) 2>&1
```

**Important:** The build output is very large (~60KB, 1100+ lines) because CMake
lists every dependency target. Do NOT pipe through `tail` or other filters — the
tool's output truncation can interact with pipes and produce empty output, making
it look like the build produced nothing. Run the command without pipes and check
the persisted output file for the final lines (`Built target redgui` and exit
code 0 indicate success).
