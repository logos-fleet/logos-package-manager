# package_manager_lib for a phone: the lgpm C++/C library as ONE STATIC ARCHIVE
# per iOS target, with the lgx archive it links staged beside it.
#
# WHY THE lgx ARCHIVE TRAVELS WITH IT. A consumer of this package links BOTH --
# package_manager_module's CMakeLists names `package_manager_lib` and `lgx` in
# EXTERNAL_LIBS, and on a desktop the two arrive together because
# nix/lib.nix stages liblgx.dylib into the same lib/. Staging it here keeps the
# mobile package the same SHAPE as the desktop one, which is the whole contract
# logos-module-builder's `mobilePackages` seam is written against.
#
# NOT SHARED, and not by preference: iOS loads no dynamic library of its own, so
# LGPM_STATIC_LIB is what this target supports (the CLI is not built there at
# all -- an executable is not something an app bundle can hold).
{ pkgs, src, version, lgx }:

let
  buildInputs = [
    lgx
    pkgs.nlohmann_json
  ];
in
pkgs.mkIosCmakeStage {
  pname = "logos-package-manager-lib-ios";
  inherit src version buildInputs;
  cmakeFlags = [
    # An iOS toolchain puts find_package in root-only mode: being on
    # CMAKE_PREFIX_PATH is not enough, every input has to be named as a ROOT.
    "-DCMAKE_FIND_ROOT_PATH=${pkgs.lib.concatStringsSep ";" (map toString buildInputs)}"
    "-DLGX_ROOT=${lgx}"
    "-DLGPM_STATIC_LIB=ON"
  ];
  postInstall = ''
    cp ${lgx}/lib/liblgx.a $out/lib/
    cp ${lgx}/include/lgx.h $out/include/
  '';
}
