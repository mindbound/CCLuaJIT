![CCLuaJIT Logo](logo.png)
-----
[![Open Source Love](https://badges.frapsoft.com/os/v1/open-source.svg?v=103)](https://github.com/ellerbrock/open-source-badges/) [![MIT Licence](https://badges.frapsoft.com/os/mit/mit.svg?v=103)](https://opensource.org/licenses/mit-license.php)
# CCLuaJIT
This is a mod that changes ComputerCraft (or CC:Tweaked) to use LuaJIT instead of the mods default Lua executor, in the interest of performance.

## Attributions
Special thanks to Mike Pall for the amazing [LuaJIT](http://luajit.org/)

Also, special thanks to kkaylium for creating the logo for the mod!

## Credits
CCLuaJIT was created by Vereena Inara ([vereena0x13](https://github.com/vereena0x13)); the original repository is [vereena0x13/CCLuaJIT](https://github.com/vereena0x13/CCLuaJIT).

The Minecraft 1.21.1 / CC: Tweaked 1.121.0 port on the `1.21.1_CCT` branch is a rewrite by Arets Paeglis ([mindbound](https://github.com/mindbound)). Its plan and design are in [`docs/port-1.21.1/`](docs/port-1.21.1/PLAN.md).

## Licences
CCLuaJIT is under the MIT licence ([`LICENSE`](LICENSE)). The native libraries it ships contain [LuaJIT](https://luajit.org/) (MIT; its notice covers the Lua and dlmalloc code in it too), and the Windows library also contains parts of the mingw-w64 runtime. Their notices are in [`LICENSES/`](LICENSES/) and ship in the mod jar next to each library. The CC: Tweaked test suite vendored for the headless tests (`common/src/test/resources/test-rom/`) and `CctSuiteRunner.java`, which copies code from CC: Tweaked's test harness, are under the MPL-2.0 ([`LICENSES/MPL-2.0.txt`](LICENSES/MPL-2.0.txt)). Neither is part of the mod jar.
