Runs aggregated: r1, r2, r3, r4 (each cell = median over runs of the per-run statistic)

| benchmark | Cobalt cold (1st iter, fresh JVM) | Cobalt warm (median iters 4-10) | Cobalt best iter | Cobalt warm, shared JVM (2nd pass) | LuaJIT JIT 1st iter | LuaJIT JIT | LuaJIT -joff | -joff + count hook/1000 | JIT + count hook/1000 | JIT + jit.profile 'l' | JIT + jit.profile 'f' | PUC Lua 5.4 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| fib | 0.880 | 1.068 | 0.836 | 1.165 | 0.041 | 0.042 | 0.179 | 0.200 | 0.038 | 0.037 | 0.043 | 0.208 |
| mandel | 0.734 | 0.551 | 0.531 | 0.948 | 0.018 | 0.019 | 0.109 | 0.178 | 0.017 | 0.017 | 0.018 | 0.179 |
| tables | 1.939 | 1.525 | 1.339 | 1.473 | 0.285 | 0.267 | 0.359 | 0.454 | 0.248 | 0.272 | 0.248 | 0.547 |
| oop | 0.963 | 0.828 | 0.814 | 1.040 | 0.046 | 0.045 | 0.224 | 0.246 | 0.044 | 0.043 | 0.047 | 0.366 |
| strings | 0.794 | 0.380 | 0.369 | 0.426 | 0.078 | 0.079 | 0.134 | 0.146 | 0.074 | 0.075 | 0.081 | 0.325 |
| sha256 | 0.962 | 0.737 | 0.697 | 0.833 | 0.014 | 0.013 | 0.092 | 0.119 | 0.012 | 0.013 | 0.013 | 0.348 |
| framebuf | 1.190 | 0.875 | 0.867 | 1.158 | 0.203 | 0.222 | 0.404 | 0.438 | 0.190 | 0.184 | 0.209 | 0.649 |
| serialize | 1.249 | 0.835 | 0.797 | 0.888 | 0.188 | 0.182 | 0.272 | 0.277 | 0.183 | 0.173 | 0.189 | 0.386 |
| lzw | 0.847 | 0.611 | 0.593 | 0.753 | 0.176 | 0.174 | 0.240 | 0.269 | 0.165 | 0.249 | 0.186 | 0.367 |
| compile | 0.913 | 0.639 | 0.628 | 0.669 | 0.304 | 0.277 | 0.218 | 0.221 | 0.217 | 0.286 | 0.239 | 0.260 |

(all times in seconds per run)

Speedup = Cobalt time / other time (higher = faster than Cobalt)

| benchmark | JIT vs Cobalt warm | JIT vs Cobalt best | JIT vs Cobalt shared-JVM | JIT vs Cobalt cold | -joff vs Cobalt warm | -joff vs Cobalt best | -joff+hook vs Cobalt warm | JIT+profile'l' vs Cobalt warm | PUC 5.4 vs Cobalt warm |
|---|---|---|---|---|---|---|---|---|---|
| fib | 25.7x | 20.1x | 28.1x | 21.2x | 6.0x | 4.7x | 5.3x | 28.5x | 5.1x |
| mandel | 29.0x | 27.9x | 49.9x | 38.6x | 5.1x | 4.9x | 3.1x | 32.4x | 3.1x |
| tables | 5.7x | 5.0x | 5.5x | 7.3x | 4.2x | 3.7x | 3.4x | 5.6x | 2.8x |
| oop | 18.2x | 17.9x | 22.9x | 21.2x | 3.7x | 3.6x | 3.4x | 19.0x | 2.3x |
| strings | 4.8x | 4.7x | 5.4x | 10.1x | 2.8x | 2.8x | 2.6x | 5.1x | 1.2x |
| sha256 | 54.6x | 51.7x | 61.7x | 71.3x | 8.0x | 7.6x | 6.2x | 59.0x | 2.1x |
| framebuf | 4.0x | 3.9x | 5.2x | 5.4x | 2.2x | 2.1x | 2.0x | 4.7x | 1.3x |
| serialize | 4.6x | 4.4x | 4.9x | 6.8x | 3.1x | 2.9x | 3.0x | 4.8x | 2.2x |
| lzw | 3.5x | 3.4x | 4.3x | 4.9x | 2.5x | 2.5x | 2.3x | 2.5x | 1.7x |
| compile | 2.3x | 2.3x | 2.4x | 3.3x | 2.9x | 2.9x | 2.9x | 2.2x | 2.5x |
| **geomean** | **8.9x** | **8.3x** | **10.5x** | **11.8x** | **3.7x** | **3.5x** | **3.2x** | **9.1x** | **2.2x** |
| min..max | 2.3..54.6x | 2.3..51.7x | 2.4..61.7x | 3.3..71.3x | 2.2..8.0x | 2.1..7.6x | 2.0..6.2x | 2.2..59.0x | 1.2..5.1x |

sha256 on LuaJIT JIT with a correct unsigned bit32 shim: 0.033 s vs 0.013 s with bit32=bit (signed, CCLuaJIT behaviour)

Checksums (must agree across runtimes):
- fib: OK 5702887
- mandel: OK 15165/9479017
- tables: OK 176244023698 1110 20232
- oop: OK 400590.000 200515.000 54235106 800000
- strings: OK 18668908
- sha256: OK 53bb2f03652be9fdc8d9ee24cbca9c819fd20a6ac4c7ac26c4b67e1c2b42bcc9
- framebuf: OK 96641135
- serialize: OK 15508320.0 1281040 12960 528560
- lzw: OK 42759:439040
- compile: OK 4200 files 28735800 bytes
