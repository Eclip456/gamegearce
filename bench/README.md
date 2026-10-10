# Speed benchmark

Measures how fast the Z80 core runs on a TI-84 Plus CE, without needing a
calculator: `GGBENCH.8xp` runs 2,000,000 Z80 cycles of ZEXDOC and times them
with the calculator's hardware timer, and `cemu/run` runs it in
[CEmu](https://github.com/CE-Programming/CEmu)'s core with no TI OS (the flash
is filled with `RET`, so the program's few OS calls return at once).

On a real calculator the C core measured 5% in GGCE; this benchmark gives 4.6%.

```sh
python3 cemu/embed.py path/to/zexdoc.com     # writes src/workload.h
make                                         # builds bin/GGBENCH.8xp
git clone https://github.com/CE-Programming/CEmu /tmp/CEmu
make -C /tmp/CEmu/core lib
make -C cemu CEMU_DIR=/tmp/CEmu
cemu/run bin/GGBENCH.8xp
```
