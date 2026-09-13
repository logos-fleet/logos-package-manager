#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class SignaturePolicy {
    NONE,    // Accept all packages without checking signatures
    WARN,    // Accept unsigned packages with a warning (default)
    REQUIRE  // Reject unsigned packages and packages with unknown signers
};

// Where a scanned package lives. "embedded" is a read-only, shipped-with-app
// directory; "user" is the writable directory new installs go to. When the
// same package name exists in both, user wins at scan time.
enum class InstallType {
    Embedded,
    User,
};

// Whether a dependency is installed, absent, part of a cycle, installed at a
// version its dependant refused, or installed under a different publisher.
//
// APPEND new enumerators, never insert: this enum crosses a shared library
// boundary (logos-package-manager-module compiles against this header and
// links libpackage_manager_lib at run time), so existing values are ABI.
enum class DependencyStatus {
    Installed,
    NotInstalled,
    Cycle,
    // Installed, but the version does not satisfy the semver range this edge
    // declared. Distinct from NotInstalled: different remedy, and only absence
    // can be asserted without reading a constraint. `version` and
    // `installType` are populated.
    VersionMismatch,
    // Installed under that name, and provably not the package the dependant
    // named: the install's signature does not verify against the key in the
    // edge's `signer` DID. An IDENTITY answer, not an authorization one —
    // whether a publisher may be installed at all is the separate trust-anchor
    // gate in installPluginFile. `version` and `installType` are populated.
    SignerMismatch,
    // The edge names a `signer`, a package by that name is installed, and
    // nothing records who published it — absence of evidence, not a mismatch.
    // Which way that falls is UnknownSignerPolicy. `version` and `installType`
    // are populated.
    SignerUnknown,
};

// What resolveDependencies reports for a `signer`-pinned edge when the
// installed package records no publisher at all: embedded packages never pass
// through installPluginFile and so can never acquire the `signer` sidecar, and
// neither can installs predating it or made under SignaturePolicy::NONE.
//
// Default Lenient (flip with setUnknownSignerPolicy), because under Strict a
// pin on an embedded dependency is unsatisfiable by construction, forever,
// with nothing the user can do. Residual: an unsigned impostor under a pinned
// name also records nothing, so it reports SignerUnknown and does not block —
// bounded by SignaturePolicy::REQUIRE, which refuses unsigned installs.
enum class UnknownSignerPolicy {
    // No record -> SignerUnknown. Surfaced; claims no mismatch it cannot prove.
    Lenient,
    // No record -> SignerMismatch. Fail closed: only a signature verified
    // against the pinned key satisfies the pin.
    Strict,
};

struct SignatureVerificationResult {
    bool is_signed = false;
    bool signature_valid = false;
    bool package_valid = false;
    std::string signer_did;    // did:jwk:... string
    std::string signer_name;   // self-asserted display name
    std::string signer_url;    // self-asserted URL
    std::string trusted_as;    // keyring name if trusted, empty otherwise
    std::string error;         // error message if any
};

struct UninstallResult {
    bool success = false;
    std::string errorMsg;
    std::vector<std::string> removedFiles;   // paths removed (top-level dir)
};

// Nested to mirror the manifest.json shape. Only `root` is read today, but
// keeping the struct leaves room for additional hash fields without a
// subsequent refactor.
struct Hashes {
    std::string root;            // Merkle root over the package contents
};

// One entry of a manifest's `dependencies` array. Mirrors lgx::Dependency
// (logos-package src/core/manifest.h) and the LGX spec's "Dependency entries"
// section, which defines two on-disk forms:
//
//   plain string  "waku_module"                     -> { name }
//   object        { "name": "waku_module",
//                   "version"?: "^1.2.0",           -> npm-style semver range
//                   "signer"?:  "did:jwk:..." }     -> publisher DID pin
//
// The distinction is NOT cosmetic: the scan used to accept only the string
// form, so an object entry lost THE EDGE ITSELF (not merely its constraint)
// and vanished from resolveDependencies / resolveDependents. Every consumer
// that needs a bare name reads `.name`, which both forms populate.
//
// Implicitly constructible from a string so `dependencies.push_back("dep")`
// and brace-init from a name list keep working unchanged.
struct PackageDependency {
    std::string name;                       // required; canonical package name
    std::optional<std::string> version;     // semver range; absent = any version
    std::optional<std::string> signer;      // did:jwk:...; absent = any signer

    PackageDependency() = default;
    PackageDependency(std::string n) : name(std::move(n)) {}   // NOLINT: intended implicit
    PackageDependency(const char* n) : name(n) {}              // NOLINT: intended implicit

    // True when only `name` is set — the entry round-trips as a plain string.
    bool isSimple() const { return !version.has_value() && !signer.has_value(); }

    // Single-line human form used by `lgpm info` and error messages.
    std::string toString() const {
        std::string s = name;
        if (version) s += " " + *version;
        if (signer)  s += " [signer=" + *signer + "]";
        return s;
    }

    bool operator==(const PackageDependency& o) const {
        return name == o.name && version == o.version && signer == o.signer;
    }
    bool operator!=(const PackageDependency& o) const { return !(*this == o); }
};

// A scanned, installed package. Mirrors the manifest.json fields plus the
// resolved install location. `icon` and `view` are optional; `view` is only
// meaningful for ui_qml packages (the QML entry point).
struct InstalledPackage {
    std::string name;
    std::string displayName;
    std::string version;
    std::string description;
    std::string type;                       // "core" | "ui" | "ui_qml"
    std::string category;
    std::string author;
    std::string license;
    std::string icon;
    std::string view;
    std::string manifestVersion;
    // Dependency NAMES — one per manifest `dependencies[]` entry, in declared
    // order, for BOTH the string and the object form. This is the edge set the
    // dependency graph is built from, so an object-form entry must appear here
    // exactly like a string one.
    std::vector<std::string> dependencies;
    // The subset of those entries that declared a version range and/or a signer
    // DID, carrying the constraint verbatim. Empty for a package whose
    // dependencies are all bare names — which is every package in the workspace
    // today, which is why the scan dropping object entries went unnoticed.
    //
    // Kept separate from `dependencies` (rather than widening its element type)
    // so the name-only wire format and every existing consumer are unchanged;
    // the semantic evaluation of these constraints belongs to the resolver in
    // logos-package-downloader and, at load time, to the runtime.
    std::vector<PackageDependency> dependencyConstraints;
    // The DID from the installed `manifest.sig`, set only once that signature
    // verified against the key the DID itself carries — self-consistent, so it
    // proves nothing about identity: whoever replaces manifest.sig replaces the
    // DID beside it and this reports THEIRS. Use it for DISPLAY; never compare
    // it to a pin — resolveDependencies and the trust-anchor gate supply an
    // outside key instead.
    //
    // nullopt = no usable signature installed: no manifest.sig (every embedded
    // and every unsigned package) or one that does not verify even under its
    // own DID. It never means "unsigned".
    std::optional<std::string> signerDid;

    // Concrete dependencies the package can call but does NOT require
    // (manifest `optional_dependencies`). Kept apart from `dependencies`
    // because an absent one is not a broken install: the runtime never
    // auto-loads them and never fails a load over them.
    std::vector<PackageDependency> optionalDependencies;
    // Interface contracts bound to a provider by name at runtime (manifest
    // `interface_dependencies`). Names only, and unresolvable to a package by
    // construction — carried so a UI can show what a module expects to find.
    std::vector<std::string> interfaceDependencies;
    Hashes hashes;
    InstallType installType;
    std::string installDir;
    std::string mainFilePath;
};

// A node in the forward dependency tree produced by resolveDependencies.
// Recursion stops at NotInstalled and Cycle nodes (no manifest available);
// for those, `version` is empty and `installType` is unspecified.
struct DependencyTreeNode {
    std::string name;
    DependencyStatus status;
    // Both populated whenever nodeResolvedToAnInstalledPackage(status); empty
    // and unspecified for NotInstalled and Cycle.
    std::string version;
    InstallType installType;
    // The constraint the PARENT declared on this edge, verbatim; absent when
    // the parent named the dependency without one, and always absent on the
    // root, which no edge points at. Both are judged — `requiredVersion`
    // against `version`, `requiredSigner` by verifying the installed signature
    // against the pinned key (never by comparing DID strings) — and `status`
    // reports whichever failed.
    std::optional<std::string> requiredVersion;
    std::optional<std::string> requiredSigner;
    // Was the edge reaching this node declared OPTIONAL? A per-edge fact like
    // the two above, and always false on the root, which no edge points at.
    //
    // A NotInstalled node carrying this is NOT a broken install — the runtime
    // will not load it and will not fail over its absence — so a health check
    // or a missing-dependency marker must skip it. Inherited by a required
    // child of an optional edge: what you need only if you installed something
    // you did not have to install is not itself required.
    bool optional = false;
    // What the installed package's signature says about itself — see
    // InstalledPackage::signerDid. Present on any node that resolved to an
    // installed package carrying a usable signature. Sits next to
    // `requiredSigner` so a report can name both sides ("requires <pin>,
    // signed by <did>"); `status` is NOT derived by comparing the two.
    std::optional<std::string> signerDid;
    std::vector<DependencyTreeNode> children;

    // BFS enumeration of descendants (this node is excluded), deduplicated
    // by name so diamonds and cycles don't produce repeats. The `children`
    // vectors on returned copies are left empty — consumers iterate the
    // flat output without double-counting.
    //
    // The dedup interacts with the per-edge constraint fields: two parents may
    // depend on the same package under different constraints, and the flat list
    // keeps the edge BFS reached first — EXCEPT that a later edge judging the
    // package MORE HARSHLY promotes the row and carries its own constraint,
    // since a package satisfies its dependants only if it satisfies all of
    // them. The row still reports one failing constraint, not all; callers that
    // need every constraint walk the tree instead of flattening it.
    std::vector<DependencyTreeNode> flatten() const;
};

// A node in the reverse dependency tree produced by resolveDependents.
// Every node is an installed package (reverse edges are synthesised only
// from installed manifests), so the legacy "status" field used by the
// forward tree has no counterpart here.
struct DependentTreeNode {
    std::string name;
    std::string version;
    std::string type;
    InstallType installType;
    std::string installDir;
    std::vector<DependentTreeNode> children;

    // Same semantics as DependencyTreeNode::flatten — BFS descendants,
    // name-deduped, children cleared on returned copies.
    std::vector<DependentTreeNode> flatten() const;
};

const char* installTypeToString(InstallType t);
const char* dependencyStatusToString(DependencyStatus s);

// Did this node resolve to a package that is actually ON DISK? True for every
// status except NotInstalled and Cycle, and so decides whether `version` /
// `installType` carry meaning. A predicate rather than an `||` chain at each
// site: an appended installed-but-rejected status would otherwise silently
// start reporting an empty version at call sites in other repositories.
bool nodeResolvedToAnInstalledPackage(DependencyStatus s);

class PackageManagerLib
{
public:
    PackageManagerLib();
    ~PackageManagerLib();

    // Directory management — embedded directories (multiple, read-only at runtime)
    void setEmbeddedModulesDirectory(const std::string& dir);
    void addEmbeddedModulesDirectory(const std::string& dir);
    void setEmbeddedUiPluginsDirectory(const std::string& dir);
    void addEmbeddedUiPluginsDirectory(const std::string& dir);

    std::vector<std::string> embeddedModulesDirectories() const { return m_embeddedModulesDirs; }
    std::vector<std::string> embeddedUiPluginsDirectories() const { return m_embeddedUiPluginsDirs; }

    // Directory management — user directories (single, writable, where new packages are installed)
    void setUserModulesDirectory(const std::string& dir);
    void setUserUiPluginsDirectory(const std::string& dir);

    std::string userModulesDirectory() const { return m_userModulesDir; }
    std::string userUiPluginsDirectory() const { return m_userUiPluginsDir; }

    // All modules directories (embedded + user) for scanning
    std::vector<std::string> allModulesDirectories() const;
    // All UI plugins directories (embedded + user) for scanning
    std::vector<std::string> allUiPluginsDirectories() const;
    // All directories (modules + UI plugins) for scanning
    std::vector<std::string> allDirectories() const;

    // Install from local LGX file
    // Returns the install ROOT (the configured user modules / UI plugins
    // directory), or empty string on error (sets errorMsg).
    //
    // If installedPluginPath is non-null, receives the path identifying what
    // was installed — see resolveInstalledPackagePath() for the exact rule.
    // It is NEVER empty when this function reports success, so callers may
    // use it as the "something was installed" signal. It is the installed
    // main FILE when the package ships one, and the installed module
    // DIRECTORY otherwise (a QML-only ui_qml package legitimately has no
    // backend library, so its manifest carries an empty "main").
    //
    // If isCoreModule is non-null, receives whether the module type is "core".
    std::string installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                  bool skipIfNotNewerVersion = false,
                                  std::string* installedPluginPath = nullptr,
                                  bool* isCoreModule = nullptr);

    // Decide what installPluginFile() reports for a package that has just
    // been copied into `moduleDir` (= <installRoot>/<moduleName>). `variants`
    // is normally platformVariantsToTry().
    //
    // Returns <moduleDir>/<main> when the installed manifest.json names a main
    // file for this platform AND that file is present; otherwise returns
    // `moduleDir` itself. Only returns empty when `moduleDir` does not exist.
    //
    // The directory fallback is the fix for a real defect: a QML-only ui_qml
    // package has "main": {}, so this used to yield an empty string on a
    // completely successful install. Callers treat an empty path as failure —
    // logos-package-manager-ui rendered a red RETRY, and
    // package_manager_module skipped its uiPluginFileInstalled event, so the
    // freshly installed plugin only appeared after an app restart.
    //
    // Exposed publicly so the behaviour can be unit-tested directly: the
    // "main": {} shape cannot be produced through liblgx's C API (lgx_add_variant
    // only allows a main-less directory variant when the package manifest
    // already declares type "ui_qml", and there is no lgx_set_type()).
    static std::string resolveInstalledPackagePath(const std::string& moduleDir,
                                                   const std::vector<std::string>& variants);

    // Module scanning — returns the scanned packages as populated structs.
    // Each struct carries all manifest.json fields plus the resolved
    // install location and install type. For ui_qml packages, `view` is
    // the required QML entry point and `mainFilePath` is backend-only
    // metadata that may be empty.
    //
    // When the same package name appears in both an embedded and the user
    // directory, the user-directory copy wins and the embedded entry is
    // dropped from the result.
    //
    // Serialization to JSON is the caller's responsibility — see
    // package_manager_json.h for to_json hooks that match the legacy wire
    // format used by the C ABI and the `lgpm --json` CLI output.
    std::vector<InstalledPackage> getInstalledModules();
    std::vector<InstalledPackage> getInstalledUiPlugins();
    std::vector<InstalledPackage> getInstalledPackages();

    // Dependency resolution over the currently-installed package set.
    //
    // resolveDependencies — walks the forward dep tree rooted at
    // `packageName`. Recursion stops at NotInstalled and Cycle nodes (we
    // don't have a manifest for them). Returns `std::nullopt` if
    // `packageName` is itself absent from the installed set.
    std::optional<DependencyTreeNode> resolveDependencies(const std::string& packageName);

    // resolveDependents — walks the reverse dep tree rooted at
    // `packageName`. The returned root carries its own manifest metadata;
    // `children` are its direct dependents; deeper levels are transitive.
    // Returns `std::nullopt` if `packageName` is itself absent from the
    // installed set. Callers that want a flat list build it via
    // `tree->flatten()`.
    std::optional<DependentTreeNode> resolveDependents(const std::string& packageName);

    // Uninstall a previously-installed package. Refuses if the resolved
    // install lives in an embedded directory. Best-effort recursive
    // delete; does not touch runtime load state.
    UninstallResult uninstallPackage(const std::string& packageName);

    // LGX extraction and installation helpers
    bool extractLgxPackage(const std::string& lgxPath, const std::string& outputDir, std::string& errorMsg);
    bool copyLibraryFromExtracted(const std::string& extractedDir, const std::string& targetDir,
                                  bool isCoreModule, std::string& outModuleName, std::string& errorMsg);

    // Variant selection (platform detection)
    // Install for a platform OTHER than the one we are running on.
    //
    // Needed for cross-builds: the Nix install bundler runs lgpm on the Linux
    // builder to lay out a Windows package. Without this it fail-closes with
    // "Package does not contain variant for platform: linux-x86_64 (package
    // provides: windows-x86_64)" -- correct behaviour, and the protection is
    // worth keeping, so this override is EXPLICIT and never inferred. Empty
    // string restores the compiled-in platform.
    static void setPlatformVariantOverride(const std::string& variant);
    static std::string platformVariantOverride();

    static std::string currentPlatformVariant();
    static std::vector<std::string> platformVariantsToTry();

    // WHAT THIS INSTALL SELECTS OUT OF A PACKAGE, when that is not the variant
    // this host LOADS.
    //
    // platformVariantsToTry() is the loader's answer, and it must stay the
    // loader's: a host that reached past its own image on its own would install
    // a web payload where it opens a plugin (see the `web` rows in
    // tests/test_variant.cpp). A Store shell is the case that is not covered by
    // it -- its native variant arrived in the app image at BUILD time and a
    // phone may not download native code (ADR 0003), so the only thing it
    // installs at run time is a `web` variant, into the Web container.
    //
    // So the embedder DECLARES it, exactly as the cross-build override above is
    // declared and never inferred. Each entry is expanded through liblgx's
    // spellings, so a declaration of "web" accepts every alias liblgx gives it.
    // An EMPTY list restores this host's own variant; it does not mean "install
    // nothing".
    //
    // Per instance rather than process-wide: the override next door is a
    // cross-BUILD tool's answer for one lgpm invocation, while this is a
    // property of the shell holding the library.
    void setInstallVariants(const std::vector<std::string>& variants);
    // The declaration as given, before expansion. Empty when none was made.
    std::vector<std::string> declaredInstallVariants() const { return m_installVariants; }
    // What installPluginFile() actually looks for, expanded: the declaration
    // when there is one, and platformVariantsToTry() when there is not.
    std::vector<std::string> installVariantsToTry() const;

    // Signature policy configuration
    void setSignaturePolicy(SignaturePolicy policy);
    SignaturePolicy signaturePolicy() const { return m_signaturePolicy; }
    void setKeyringDirectory(const std::string& dir);
    std::string keyringDirectory() const { return m_keyringDir; }

    // What resolveDependencies reports for a signer-pinned edge whose
    // installed package records no publisher. Default Lenient.
    void setUnknownSignerPolicy(UnknownSignerPolicy policy);
    UnknownSignerPolicy unknownSignerPolicy() const { return m_unknownSignerPolicy; }

    // Standalone signature verification
    SignatureVerificationResult verifyPackageSignature(const std::string& lgxPath);

    // Utilities
    static bool versionGreaterOrEqual(const std::string& a, const std::string& b);
    static bool copyDirectoryContents(const std::string& srcDir, const std::string& destDir, std::string& errorMsg);

    // Validates a module name that came from an untrusted manifest before it is
    // used as a path component when installing. A name is only safe to join onto
    // an install directory if it is a single, plain path segment: rejects empty,
    // ".", "..", anything containing a path separator ('/' or '\\'), a NUL byte,
    // or a leading drive separator. This is the guard against F-006 (path
    // traversal via the manifest "name" field).
    static bool isValidModuleName(const std::string& name);

private:
    std::vector<std::string> m_embeddedModulesDirs;
    std::string m_userModulesDir;
    std::vector<std::string> m_embeddedUiPluginsDirs;
    std::string m_userUiPluginsDir;
    // Empty = this host's own; see setInstallVariants().
    std::vector<std::string> m_installVariants;
    SignaturePolicy m_signaturePolicy = SignaturePolicy::WARN;
    // Build-wide default; see UnknownSignerPolicy.
    UnknownSignerPolicy m_unknownSignerPolicy = UnknownSignerPolicy::Lenient;
    std::string m_keyringDir;
};
