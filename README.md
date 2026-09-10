# logos-package-manager

C++ library and CLI for local Logos package management — installing `.lgx` packages, scanning installed modules, platform variant selection, and LGX extraction.

This repo handles **local operations** only. It does not fetch packages from the network — that is the responsibility of [logos-package-downloader](https://github.com/logos-co/logos-package-downloader).

## Library API

```cpp
#include <package_manager_lib.h>

PackageManagerLib pm;

// Configure embedded directories (multiple, read-only at runtime)
pm.setEmbeddedModulesDirectory("/path/to/embedded/modules");       // clears + sets first
pm.addEmbeddedModulesDirectory("/path/to/more/embedded/modules");  // appends additional
pm.setEmbeddedUiPluginsDirectory("/path/to/embedded/plugins");
pm.addEmbeddedUiPluginsDirectory("/path/to/more/embedded/plugins");

// Configure user directories (single, writable, where new packages are installed)
pm.setUserModulesDirectory("/path/to/user/modules");
pm.setUserUiPluginsDirectory("/path/to/user/plugins");

// Install from a local .lgx file (auto-detects type, installs to user directory)
std::string errorMsg;
std::string installedPath = pm.installPluginFile("/path/to/module.lgx", errorMsg);
// skipIfNotNewer: skip installation if an equal-or-newer version is already installed
std::string path = pm.installPluginFile("/path/to/module.lgx", errorMsg, /*skipIfNotNewer=*/true);

// Scan installed packages (returns struct vectors)
// Each InstalledPackage carries all manifest.json fields plus installDir,
// mainFilePath, installType, and a nested Hashes struct. For ui_qml packages,
// `view` is the required QML entry point and `mainFilePath` is the optional
// backend plugin path.
std::vector<InstalledPackage> modules   = pm.getInstalledModules();     // type=core only
std::vector<InstalledPackage> uiPlugins = pm.getInstalledUiPlugins();   // type=ui,ui_qml only
std::vector<InstalledPackage> all       = pm.getInstalledPackages();    // all types

// Direct dependencies are already on InstalledPackage::dependencies; the
// library does not ship a parallel name-only API. Callers that want a flat
// transitive list build it over resolveDependencies / resolveDependents
// (see the `lgpm` CLI `deps` / `dependents` subcommands for a reference
// implementation — ~15 lines of BFS with a seen-set).
//
// A manifest `dependencies[]` entry is either a plain name or an object
// { name, version?, signer? } (LGX spec, "Dependency entries"). Both forms
// declare the same edge, so `dependencies` lists the NAME of every entry.
// The entries that also declared a semver range or a signer DID are repeated
// in `dependencyConstraints` with the constraint attached; the library records
// them but does not evaluate them (that is the resolver's job, in
// logos-package-downloader).
for (const PackageDependency& c : pkg.dependencyConstraints)
    std::cout << c.toString() << "\n";   // e.g. waku_module ^1.2.0 [signer=did:jwk:…]

// Dependency graph as a rich struct tree. Returns std::nullopt when the
// package is not installed. Children with status == NotInstalled are
// leaves; Cycle marks a back-edge during the walk.
std::optional<DependencyTreeNode> tree = pm.resolveDependencies("waku_module");

// Reverse-edge tree, rooted at the queried package. `children` are the
// direct reverse neighbours; their `children` are transitive ones.
// Returns std::nullopt when the package is not installed.
std::optional<DependentTreeNode> users = pm.resolveDependents("waku_module");

// Both tree types ship a BFS-with-dedup `flatten()` — handy for callers
// that only want "every unique descendant", no tree shape.
if (tree)  std::vector<DependencyTreeNode> allDeps   = tree->flatten();
if (users) std::vector<DependentTreeNode>  allUsers  = users->flatten();

// JSON serialization lives in a separate header (opt-in; the core lib
// header does not pull in nlohmann::json). nlohmann ADL `to_json` hooks
// are provided for every struct above.
#include <package_manager_json.h>
#include <nlohmann/json.hpp>
std::string modulesJson = nlohmann::json(modules).dump(2);

// Platform variant helpers. The vocabulary and the fallback order are
// logos-package's — a variant name is a key in the signed hash tree, so lgpm
// asks liblgx rather than tabulating its own. Desktop, mobile (android-arm64,
// android-x86_64, ios-arm64, ios-sim-arm64) and web are one table; see
// logos-package docs/spec.md § Platform Variant Vocabulary.
std::string variant = PackageManagerLib::currentPlatformVariant();     // e.g. "darwin-arm64"
std::vector<std::string> variants = PackageManagerLib::platformVariantsToTry(); // ordered fallback list

// Low-level LGX extraction
pm.extractLgxPackage("/path/to/module.lgx", "/output/dir", errorMsg);
pm.copyLibraryFromExtracted("/extracted/dir", "/target/dir",
                            /*isCoreModule=*/true, outModuleName, errorMsg);

// Signature verification policy
pm.setSignaturePolicy(SignaturePolicy::WARN);     // NONE, WARN (default), REQUIRE
pm.setKeyringDirectory("/path/to/trusted-keys");  // default: ~/.config/logos/trusted-keys/

// Standalone signature verification
SignatureVerificationResult sigInfo = pm.verifyPackageSignature("/path/to/module.lgx");
// sigInfo.is_signed, sigInfo.signature_valid, sigInfo.package_valid,
// sigInfo.signer_did, sigInfo.signer_name, sigInfo.signer_url, sigInfo.trusted_as, sigInfo.error

// Utilities
bool newer = PackageManagerLib::versionGreaterOrEqual("1.2.0", "1.1.0");
PackageManagerLib::copyDirectoryContents("/src", "/dst", errorMsg);
```

### C API

A C-compatible API is available via `lgpm.h` for use from non-C++ consumers:

```c
#include <lgpm.h>

lgpm_context_t ctx = lgpm_create();
lgpm_set_embedded_modules_dir(ctx, "/path/to/embedded/modules");
lgpm_add_embedded_modules_dir(ctx, "/path/to/more/embedded/modules");
lgpm_set_user_modules_dir(ctx, "/path/to/user/modules");

// Signature policy
lgpm_set_signature_policy(ctx, "require");  // "none", "warn", "require"
lgpm_set_keyring_path(ctx, "/path/to/trusted-keys");

char* result = lgpm_install_file(ctx, "/path/to/module.lgx", false, NULL, NULL);
lgpm_free_string(result);

// Flat name-only dependency queries (mirror liblogos' logos_core_get_module_*).
// Null-terminated array; caller frees with lgpm_free_string_array.
const char** deps       = lgpm_get_module_dependencies(ctx, "waku_module", false);
const char** allDeps    = lgpm_get_module_dependencies(ctx, "waku_module", true);
const char** dependents = lgpm_get_module_dependents(ctx, "waku_module", false);
const char** allUsers   = lgpm_get_module_dependents(ctx, "waku_module", true);
for (size_t i = 0; deps[i]; ++i) printf("%s\n", deps[i]);
lgpm_free_string_array(deps);
lgpm_free_string_array(allDeps);
lgpm_free_string_array(dependents);
lgpm_free_string_array(allUsers);

lgpm_free(ctx);
```

## CLI (`lgpm`)

```
lgpm [options] <command> [arguments]

Commands:
  install --file <path>       Install from a local LGX file
  install --dir <path>        Install all LGX files in a directory
  list                        List installed packages
  info <package>              Show installed package details
  deps <package>              List packages that <package> depends on
  dependents <package>        List installed packages that depend on <package>

Options:
  --modules-dir <path>        Target directory for core modules
  --ui-plugins-dir <path>     Target directory for UI plugins
  --recursive, -r             For deps/dependents: walk the graph transitively
  --json                      Output in JSON format
  --allow-unsigned            Accept unsigned packages without warning
  --require-signatures        Reject unsigned packages and untrusted signers
  --keyring <path>            Override keyring directory
  -h, --help                  Show help
  -v, --version               Show version
```

### Examples

```bash
# Install a local .lgx package
lgpm --modules-dir ./modules install --file ./waku_module.lgx

# Install all .lgx files in a directory
lgpm --modules-dir ./modules --ui-plugins-dir ./plugins install --dir ./packages/

# List installed modules
lgpm --modules-dir ./modules --ui-plugins-dir ./plugins list

# Show package info (JSON)
lgpm --modules-dir ./modules info waku_module --json

# What does waku_module depend on? (one name per line)
lgpm --modules-dir ./modules deps waku_module
lgpm --modules-dir ./modules deps waku_module --recursive

# What depends on waku_module?
lgpm --modules-dir ./modules --ui-plugins-dir ./plugins dependents waku_module
lgpm --modules-dir ./modules --ui-plugins-dir ./plugins dependents waku_module -r --json
```

## How to Build

### Using Nix (Recommended)

The package manager ships as a C++ library plus the `lgpm` CLI. There are two flavors: a **dev** build for local iteration and a **portable** build for distribution. They differ in which `.lgx` platform variant the CLI selects when installing packages — the dev build picks the `-dev`-suffixed variant (e.g. `darwin-arm64-dev`), the portable build picks the plain one (e.g. `darwin-arm64`). Installing a package built for the wrong flavor fails with a variant-mismatch error.

#### Dev Build

A standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it. It matches the local/dev `.lgx` packages produced by [`nix-bundle-lgx`](https://github.com/logos-co/nix-bundle-lgx) (the default, non-portable bundler).

```bash
nix build                        # library + CLI (combined, dev)
nix build '.#lib'                # library only
nix build '.#cli'                # CLI only
./result/bin/lgpm --help
```

#### Portable Builds

Portable builds select the plain platform variant, matching **portable** `.lgx` packages — releases from [logos-modules](https://github.com/logos-co/logos-modules), downloads via [logos-package-downloader](https://github.com/logos-co/logos-package-downloader), or bundles generated with `nix bundle --bundler github:logos-co/nix-bundle-lgx#portable ...`. The `cli-bundle-dir` and `cli-appimage` outputs go further and are **fully self-contained** — no `/nix/store` references at runtime — for distribution.

| Output | Platform | Format |
|---|---|---|
| `cli-portable` / `lib-portable` | Linux, macOS | Nix derivation (portable variant selection) |
| `cli-bundle-dir` | Linux, macOS | Self-contained flat directory with `bin/` and `lib/` |
| `cli-appimage` | Linux | Single-file `.AppImage` executable |

##### Portable Nix derivation (all platforms)
```bash
nix build '.#cli-portable'       # CLI only
nix build '.#lib-portable'       # library only
./result/bin/lgpm --help
```

##### Self-contained directory bundle (all platforms)
```bash
nix build '.#cli-bundle-dir'
./result/bin/lgpm --help
```

##### Linux AppImage (Linux only)
```bash
nix build '.#cli-appimage'
./result/lgpm.AppImage --help
```

#### Development Shell

```bash
nix develop
```

**Note:** In zsh, quote targets containing `#` to prevent glob expansion (e.g., `'.#cli'`).

If you don't have flakes enabled globally:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

## Testing

```bash
nix flake check                  # run all tests
nix build .#tests                # build and run tests
```

## Dependencies

- `logos-package` — LGX format library
- `nlohmann_json` — JSON parsing

## Executable doc-tests

The [`doctests/`](doctests/) directory holds **executable docs** powered by **[logos-doctest](https://github.com/logos-co/logos-doctest)**

| Spec | What it covers |
|------|----------------|
| [`lgpm-cli.test.yaml`](doctests/lgpm-cli.test.yaml) | Builds `lgpm`, hand-crafts two `.lgx` packages, installs them, and exercises every subcommand (`install`, `list`, `info`, `deps`, `dependents`). |
| [`install-real-module.test.yaml`](doctests/install-real-module.test.yaml) | Clones a real module ([`logos-accounts-module`](https://github.com/logos-co/logos-accounts-module)), builds its `.lgx` via the module's `#lgx` flake output, and installs it with `lgpm`. |

```bash
./doctests/run.sh
```

Or invoke the `doctest` CLI directly on a single spec:

```bash
# Execute a spec end-to-end (builds lgpm, installs packages, asserts output)
nix run github:logos-co/logos-doctest -- run doctests/lgpm-cli.test.yaml --verbose

# Render the Markdown doc from a spec (into the gitignored outputs/ dir)
nix run github:logos-co/logos-doctest -- generate doctests/lgpm-cli.test.yaml -o doctests/outputs/lgpm-cli.md
```

Add `--report report.html` to the `run` to get a self-contained two-column HTML
report — the rendered doc on the left, the exact commands that ran and their
output on the right — handy for reviewing a run or publishing from CI:

```bash
nix run github:logos-co/logos-doctest -- run doctests/lgpm-cli.test.yaml --report report.html
```
