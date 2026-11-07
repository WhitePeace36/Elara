# Maintainer: Timon Stipkovits <timon2201@gmail.com>
#
# Builds elara from this checkout. In the repository root:
#   makepkg -si
# The package is written next to this file (unless PKGDEST is set in
# makepkg.conf).

pkgname=elara
# 0.r<commits>.g<commit> of this checkout, like the .deb and .rpm (see
# CMakeLists.txt). Worked out here instead of in pkgver(): makepkg writes what
# pkgver() returns back into this file, which would change it after every commit.
pkgver=0.r$(git -C "$startdir" rev-list --count HEAD 2>/dev/null || echo 0).g$(git -C "$startdir" rev-parse --short=7 HEAD 2>/dev/null || echo 0)
pkgrel=1
pkgdesc='sched_ext CPU scheduler, tuned by nice values, for stable frame times'
arch=('x86_64' 'aarch64')
url='https://github.com/WhitePeace36/Elara'
license=('GPL-2.0-only')
depends=('libbpf' 'gcc-libs' 'glibc')
makedepends=('cmake' 'clang' 'lld' 'bpf' 'pkgconf' 'git')
# The build does its own thin LTO with clang; makepkg's -flto=auto is GCC's.
options=('!lto')
install=elara.install

build() {
  cmake -B build -S "$startdir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++
  cmake --build build
}

package() {
  DESTDIR="$pkgdir" cmake --install build
}
