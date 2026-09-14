# package_manager_lib for a phone: the lgpm C++/C library as ONE STATIC ARCHIVE
# for aarch64-android, with the lgx archive it links staged beside it -- the
# same shape ./mobile-ios.nix produces for the two iOS targets, for a different
# reason.
#
# ON iOS THE LIBRARY IS STATIC BECAUSE THE PLATFORM LOADS NOTHING ELSE. Here it
# is static because logos-nix's DT_NEEDED gate lets a shipped .so name only what
# Android guarantees at the app's API level or what the app packages beside it;
# a Bare module linking libpackage_manager_lib.so and liblgx.so would put two
# more unbundled sonames in every APK that carries it. See
# logos-package's nix/mobile-android.nix, which answers the same question for
# lgx and is why the archive staged here is self-contained.
#
# WHY THE lgx ARCHIVE TRAVELS WITH IT: a consumer links BOTH --
# package_manager_module's CMakeLists names `package_manager_lib` and `lgx` in
# EXTERNAL_LIBS -- and on a desktop the two arrive together because nix/lib.nix
# stages liblgx into the same lib/. Staging it keeps the mobile package the same
# SHAPE as the desktop one, which is the contract logos-module-builder's
# `mobilePackages` seam is written against.
{ pkgs, src, version, lgx }:

let
  buildPkgs = pkgs.pkgsBuildBuild;
  buildInputs = [
    lgx
    pkgs.nlohmann_json
  ];
in
pkgs.stdenv.mkDerivation {
  pname = "logos-package-manager-lib-android";
  inherit src version buildInputs;

  nativeBuildInputs = [ buildPkgs.cmake buildPkgs.ninja ];

  cmakeFlags = [
    "-GNinja"
    # A cross toolchain re-roots find_package/find_library at the target
    # sysroot; BOTH is what lets the store prefixes above be searched too.
    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DLGX_ROOT=${lgx}"
    "-DLGPM_STATIC_LIB=ON"
  ];

  postInstall = ''
    # The CLI builds here -- its guard only excludes iOS -- and an executable is
    # not something an APK can hold.
    rm -rf $out/bin

    cp ${lgx}/lib/liblgx.a $out/lib/
    cp ${lgx}/include/lgx.h $out/include/
  '';

  meta = {
    description = "lgpm as a static archive for aarch64-android";
    platforms = pkgs.lib.platforms.aarch64;
  };
}
