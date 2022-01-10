#!/bin/bash

BINUTILS_VERVION=2.37
GCC_VERSION=11.2.0

DEPS=(wget build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo)

export PREFIX="$(pwd)/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

function is_pkg_installed(){
    $(dpkg -s $1 >/dev/null 2>&1)
    return $?
}

function build_binutils(){
    if [[ -f binutils-${BINUTILS_VERVION}.tar.gz ]]; then
        echo "binutils-${BINUTILS_VERVION}.tar.gz already downloaded"
    else
        wget https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERVION}.tar.gz
    fi
    if [[ ! -d binutils-${BINUTILS_VERVION} ]]; then
        tar -xzvf binutils-${BINUTILS_VERVION}.tar.gz
    fi
    mkdir -p build-binutils
    cd build-binutils
        ../binutils-${BINUTILS_VERVION}/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
        make
        make install
    cd ..
}

function build_gcc(){
    if [[ -f gcc-${GCC_VERSION}.tar.gz ]]; then
        echo "gcc-${GCC_VERSION}.tar.gz already downloaded"
    else
        wget https://mirrors.aliyun.com/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.gz
    fi
    if [[ ! -d  gcc-${GCC_VERSION} ]]; then
        tar -zxvf gcc-${GCC_VERSION}.tar.gz
    fi
    mkdir -p build-gcc
    cd build-gcc
        configure_gcc_redzone $(realpath ../gcc-${GCC_VERSION})
        ../gcc-${GCC_VERSION}/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headers
        make all-gcc
        make all-target-libgcc
        make install-gcc
        make install-target-libgcc
    cd ..
}

function configure_gcc_redzone(){
    gcc_dir=$1
    tee ${gcc_dir}/gcc/config/i386/t-${TARGET}<<EOF 
# Add libgcc multilib variant without red-zone requirement
 
MULTILIB_OPTIONS += mno-red-zone
MULTILIB_DIRNAMES += no-red-zone
EOF
    sed -i "s|x86_64-\*-elf\*)|x86_64-\*-elf\*)\n\ttmake_file=\"\${tmake_file} i386/t-x86_64-elf\" # include the new multilib configuration|g" ${gcc_dir}/gcc/config.gcc
}

function check_requirement(){
    local deps=""
    for pkg in ${DEPS[@]}; do
        $(is_pkg_installed $pkg)
        if [[ $? -ne 0 ]]; then
            echo "$pkg is not installed"
            deps="$deps $pkg"
        fi
    done
    if [[ -n "$deps" ]]; then
        sudo apt install -y ${deps[@]}
    fi
}

check_requirement
build_binutils
build_gcc