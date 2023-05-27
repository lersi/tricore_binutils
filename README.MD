# TriCore Binutills

A binutills package for the TriCore architecture

## Install dependencies

The following packages are required in order to compile bin utils

`sudo apt install g++-9 gcc-9 flex bison texinfo make`

## Build 

First, export the following variables in order to configure the install destination path and the target (tricore)

```sh
export PREFIX=<some full path>
export TARGET=tricore
```

After that, create a build dir, configure the project and compile it:

```sh
mkdir build && cd build
CC=gcc-9 CXX=g++-9 CFLAGS=-w ../configure --prefix=$PREFIX --target=$TARGET --enable-lto --with-sysroot --disable-nls --disable-werror
make -j <your core count>
make install
```

## Copy Rights

Thanks to @bkoppelmann for the build instructions in [repo](https://github.com/bkoppelmann/package_940).  
Thanks to @volumit for providing the sources in [repo](https://github.com/volumit/package_940).
