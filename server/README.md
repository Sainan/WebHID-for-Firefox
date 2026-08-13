If you would like to build the server yourself, first be sure to clone this repo including submodules:
```bash
git clone --recurse-submodules https://github.com/Sainan/WebHID-for-Firefox
```
The build system that I use personally is [Sun](https://github.com/calamity-inc/Sun); just get that executable into your path and run `sun` in this directory.

Alternatively, you could manually build the server with Soup as a static library:
```bash
cd Soup
php build_lib.php
cd ..
clang main.cpp -ISoup/soup -std=c++17 -fuse-ld=lld -Lsoup -lsoup -luser32
```
Note the `clang` command is specific to Windows. Check [Why we're making Sun](https://calamity.inc/sun/) for more info. :^)
