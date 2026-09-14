{
  description = "Logos Package Manager - Local package management library and CLI";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-package.url = "github:logos-co/logos-package";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-package, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info baked into the lgpm binary so `--version` reports the release
      # version, this repo's commit, and the locked commits of the flake inputs.
      # `revOf` yields the input's locked rev, a "<sha>-dirty" marker for a dirty
      # checkout, or "dirty" for a path override.
      revOf = input: input.rev or input.dirtyRev or "dirty";
      buildInfo = {
        # VERSION is only present on release branches. On master (pre-release CI
        # builds) there is no VERSION file, so fall back to a "pre-release-{sha7}"
        # string derived from self.rev. Dirty local builds lack self.rev and get
        # an empty string, which the CLI renders as "dev".
        version = if builtins.pathExists ./VERSION
          then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
          else if (self ? rev) then "pre-release-${builtins.substring 0 7 self.rev}" else "";
        commit = revOf self;
        commits = [
          { name = "logos-package"; commit = revOf logos-package; }
          { name = "logos-nix"; commit = revOf logos-nix; }
          { name = "nix-bundle-dir"; commit = revOf nix-bundle-dir; }
          { name = "nix-bundle-appimage"; commit = revOf nix-bundle-appimage; }
        ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosPackageLib = logos-package.packages.${system}.lib;
        dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
      });
      # Adds the "x86_64-windows" pseudo-system. PACKAGES only -- `checks` cannot
      # run (ctest would have to execute PE binaries on the Linux build host)
      # and a cross devShell offers no way to run what it produces.
      forAllTargets = logos-nix.lib.forAllTargets;

      # The three mobile targets, each under the build platform that can
      # produce it: iOS needs Xcode (aarch64-darwin, no choice), Android builds
      # from either member of logos-nix's androidBuildSystems -- and a cross
      # derivation's `system` is its BUILD platform, so it is published under
      # each of them rather than under a target key nothing can realise.
      #
      # ALL THREE ARE STATIC ARCHIVES. iOS loads no dynamic library of its own;
      # Android would take one, but logos-nix's DT_NEEDED gate refuses an
      # unbundled soname inside an APK. Same shape, two reasons -- see
      # nix/mobile-ios.nix and nix/mobile-android.nix.
      iosBuildSystem = "aarch64-darwin";
      iosTargets = [ "aarch64-ios" "aarch64-ios-simulator" ];
      # `version` is the only thing read out of the desktop common config, and
      # it is a string: nothing in it is instantiated for a phone.
      mobileLib = { file, pkgs, lgx }: {
        lib = import file {
          inherit pkgs lgx;
          src = ./.;
          inherit (import ./nix/default.nix { inherit pkgs; logosPackageLib = lgx; }) version;
        };
      };
      mobileLibs = nixpkgs.lib.genAttrs iosTargets (target:
        mobileLib {
          file = ./nix/mobile-ios.nix;
          pkgs = logos-nix.lib.mkIosPkgs { inherit target; buildSystem = iosBuildSystem; };
          lgx = logos-package.legacyPackages.${iosBuildSystem}.mobile.${target}.lib;
        });
      androidLibs = buildSystem: {
        aarch64-android = mobileLib {
          file = ./nix/mobile-android.nix;
          pkgs = logos-nix.lib.mkAndroidPkgs { inherit buildSystem; };
          lgx = logos-package.legacyPackages.${buildSystem}.mobile.aarch64-android.lib;
        };
      };
    in
    {
      packages = forAllTargets ({ pkgs, system }:
        let
          logosPackageLib = logos-package.packages.${system}.lib;
          dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
          # Windows exposes the bare CLI only. nix-bundle-dir has no PE backend --
          # and does not need one for a plain console tool: PE import tables
          # carry DLL BASE NAMES rather than paths (no rpath exists in the
          # format), Windows searches the executable's own directory first, and
          # nixpkgs' win-dll-link.sh already stages every dependency DLL there.
          # So a dereferencing copy of $out/bin is self-contained, and the path
          # rewriting the ELF/Mach-O bundlers exist to do has no analogue here.
          # Verified end to end: lgx.exe round-trips a package on a Windows box
          # with no Nix installed.
          isWindows = pkgs.stdenv.hostPlatform.isWindows;

          # Common configuration (dev, default)
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          # Common configuration (portable)
          commonPortable = import ./nix/default.nix { inherit pkgs logosPackageLib; portableBuild = true; };
          src = ./.;

          # Library package (dev)
          lib = import ./nix/lib.nix { inherit pkgs common src logosPackageLib; };

          # Library package (portable)
          libPortable = import ./nix/lib.nix { inherit pkgs src logosPackageLib; common = commonPortable; };

          # CLI package (dev)
          cli = import ./nix/cli.nix { inherit pkgs common src logosPackageLib buildInfo; };

          # CLI package (portable)
          cliPortable = import ./nix/cli.nix { inherit pkgs src logosPackageLib buildInfo; common = commonPortable; };

          # Combined package
          combined = pkgs.symlinkJoin {
            name = "logos-package-manager";
            paths = [ lib cli ];
          };
        in
        {
          # Individual outputs
          logos-package-manager-lib = lib;
          logos-package-manager-cli = cli;
          lib = lib;
          lib-portable = libPortable;
          cli = cli;
          cli-portable = cliPortable;

        } // pkgs.lib.optionalAttrs (!isWindows) {
          # Bundle outputs
          cli-bundle-dir = dirBundler cliPortable;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          cli-appimage = nix-bundle-appimage.lib.${system}.mkAppImage {
            drv = cliPortable;
            name = "lgpm";
            bundle = dirBundler cliPortable;
            desktopFile = ./assets/lgpm.desktop;
            icon = ./assets/lgpm.png;
          };
        } // pkgs.lib.optionalAttrs (!isWindows) {
          # Tests
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };
        } // {
          # Default package (combined)
          default = combined;
        }
      );

      # `legacyPackages`, not `packages`: a cross derivation's `system` is its
      # BUILD platform, so an iOS archive published under `packages` would
      # collide with the native aarch64-darwin set and `nix flake check` would
      # try to realise it as a Mac one. The shape is what
      # logos-module-builder's `mobilePackages` seam reads.
      legacyPackages =
        nixpkgs.lib.genAttrs logos-nix.lib.androidBuildSystems
          (buildSystem: { mobile = androidLibs buildSystem; })
        // {
          # Merged rather than assigned: aarch64-darwin builds both Android and
          # iOS, and writing it twice would drop one of the two.
          ${iosBuildSystem}.mobile = mobileLibs // (androidLibs iosBuildSystem);
        };

      checks = forAllSystems ({ pkgs, system, logosPackageLib, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          src = ./.;
        in {
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };
        }
      );

      devShells = forAllSystems ({ pkgs, logosPackageLib, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.nlohmann_json
            pkgs.zstd
          ];

          shellHook = ''
            export LGX_ROOT="${logosPackageLib}"
            echo "Logos Package Manager development environment"
            echo "LGX_ROOT: $LGX_ROOT"
          '';
        };
      });
    };
}
