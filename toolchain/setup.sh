#!/bin/bash

BINUTILS_VERVION=2.37
GCC_VERSION=11.2.0
GDB_VERSION=11.2

BASE_DIR="$(dirname "$(readlink -f "$0")")"

. /etc/os-release

function is_pkg_installed_dpkg(){
    $(dpkg -s $1 >/dev/null 2>&1)
    return $?
}

function install_pkg_dpkg(){
    sudo apt install -y $@
}

function is_pkg_installed_pacman(){
    $(pacman -Qi $1 >/dev/null 2>&1)
    return $?
}

function install_pkg_pacman(){
    sudo pacman -Sy --noconfirm $@
}

case $ID in
    archarm)
        DEPS=(wget base-devel gmp libmpc mpfr)
        is_pkg_installed() { is_pkg_installed_pacman $@; }
        install_pkg() { install_pkg_pacman $@; }
        ;;
    ubuntu)
        DEPS=(wget build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo)
        is_pkg_installed() { is_pkg_installed_dpkg $@; }
        install_pkg() { install_pkg_dpkg $@; }
        ;;
    *)
        echo "Current distro $ID is not supported"
        exit 1
        ;;
esac

export PREFIX="${BASE_DIR}/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

job_cores=$(($(nproc) / 2))
if [[ $job_cores -lt 1 ]]; then
  job_cores=1;
fi

function build_binutils(){
    if [[ -f binutils-${BINUTILS_VERVION}.tar.gz ]]; then
        echo "binutils-${BINUTILS_VERVION}.tar.gz already downloaded"
    else
        wget https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERVION}.tar.gz
    fi
    if [[ ! -d binutils-${BINUTILS_VERVION} ]]; then
        tar -xzvf binutils-${BINUTILS_VERVION}.tar.gz
    fi
    if [[ -d $PREFIX/x86_64-elf/bin ]]; then
        echo "binutils installed, skip"
        return 0
    fi
    mkdir -p build-binutils
    cd build-binutils
        ../binutils-${BINUTILS_VERVION}/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror --enable-targets=x86_64-pep
        make -j ${job_cores}
        make install
    cd ..
}

function build_gdb(){
    if uname -m | grep -Eq 'i[[:digit:]]86-'; then
        echo "current architecture matches target, skip build cross gdb"
        return 0
    fi
    if [[ -f gdb-${GDB_VERSION}.tar.gz ]]; then
        echo "gdb-${GDB_VERSION}.tar.gz already downloaded"
    else
        wget https://mirrors.aliyun.com/gnu/gdb/gdb-${GDB_VERSION}.tar.gz
    fi
    if [[ ! -d  gdb-${GDB_VERSION} ]]; then
        tar -zxvf gdb-${GDB_VERSION}.tar.gz
    fi
    if [[ -f $PREFIX/bin/${TARGET}-gdb ]]; then
        echo "gdb installed, skip"
        return 0
    fi
    mkdir -p build-gdb
    cd build-gdb
        ../gdb-${GDB_VERSION}/configure --target=$TARGET --prefix="$PREFIX" --disable-werror
        make all-gdb -j ${job_cores}
        make install-gdb
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
    if [[ -f $PREFIX/bin/${TARGET}-gcc ]]; then
        echo "gcc installed, skip"
        return 0
    fi
    mkdir -p build-gcc
    cd build-gcc
        configure_gcc_redzone $(realpath ../gcc-${GCC_VERSION})
        ../gcc-${GCC_VERSION}/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headers
        make all-gcc -j ${job_cores}
        make all-target-libgcc ${job_cores}
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
        install_pkg ${deps[@]}
    fi
}

cd ${BASE_DIR}
check_requirement
build_binutils
build_gdb
build_gcc
cd
