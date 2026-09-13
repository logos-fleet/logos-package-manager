#include "package_manager_lib.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <iostream>
#include <map>
#include <set>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "lgx.h"
#include "logos/semver.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Windows refuses to delete a file carrying FILE_ATTRIBUTE_READONLY. POSIX
// consults only the PARENT DIRECTORY's write bit, so a payload file shipped
// mode 0444 removes fine on Linux and macOS and denies on Windows -- which is
// why this was invisible until a package was installed there.
//
// The class is NOT Windows-only. A read-only DIRECTORY denies file CREATION on
// POSIX as well: a module staged by copying `nix build .#install`'s output
// inherits the store's 0555 directory mode, and every later install into it
// fails at copy_file. Measured on macOS.
//
// Everything in the Nix store is 0444, and nix-bundle-lgx copies a module's
// `icon:` straight out of it, so every .lgx with an icon carries at least one
// read-only file. Measured on the extracted payload: the root `hello.svg` was
// "ReadOnly, Archive" while both DLLs, the manifests, the qmldir and even
// `icons/hello.svg` were plain "Archive".
//
// `fs::permissions` is the portable spelling of "clear the read-only bit" --
// deliberately NOT <windows.h>, whose `interface` macro and TokenSource
// enumerator have twice broken unrelated files in this codebase.
static void clearReadOnlyRecursive(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    fs::permissions(root, fs::perms::owner_write, fs::perm_options::add, ec);
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
        ec.clear();   // one stubborn entry must not abandon the rest
    }
}

// Remove a tree without ever throwing. The THROWING overload of remove_all is
// what turned a failed temp-directory cleanup into an uncaught
// std::filesystem_error: in the lgpm CLI it reached terminate(), and inside the
// package_manager module it unwound out of the call so the install reply was
// never sent -- the files were all on disk and the UI still showed "Retry".
//
// A temp directory that will not delete is untidy, never a reason to fail an
// install that has already succeeded.
static bool removeTreeQuietly(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (!ec) return true;
    clearReadOnlyRecursive(p);          // second attempt, attributes cleared
    ec.clear();
    fs::remove_all(p, ec);
    if (ec) {
        std::cerr << "Warning: could not remove " << p.string() << ": " << ec.message()
                  << " (leaving it behind; this does not affect the install)" << std::endl;
        return false;
    }
    return true;
}

// Name for one of lgpm's own working trees, a sibling of the install it is
// replacing. Random so two installs of one package cannot collide, and
// dot-prefixed because enumerateManifests skips that namespace -- a staging
// tree carries a valid manifest.json and would otherwise shadow the very
// module it was made from if it ever survived a crash.
static std::string reservedSiblingName(const char* role, const std::string& moduleName) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << ".lgpm-" << role << "-" << moduleName << "-" << std::hex << dist(gen);
    return oss.str();
}

// Put `staged` at `dest`, replacing whatever is there, without the two installs
// ever being MERGED. rename() cannot replace a non-empty directory, so an
// existing tree is retired first: the gap between the two renames is the only
// moment the module is absent, and what is there is always one install or the
// other, never half of each.
static bool swapIntoPlace(const fs::path& staged, const fs::path& dest, std::string& errorMsg) {
    std::error_code ec;

    if (fs::exists(dest, ec)) {
        // Cleared before it is retired, not after: a read-only tree is exactly
        // what wedged the install, and removeTreeQuietly should not have to
        // discover that on its retry.
        clearReadOnlyRecursive(dest);

        fs::path retired = dest.parent_path() / reservedSiblingName("retired", dest.filename().string());
        fs::rename(dest, retired, ec);
        if (ec) {
            errorMsg = "Failed to move the existing install aside: " + dest.string() + " - " + ec.message();
            return false;
        }

        fs::rename(staged, dest, ec);
        if (ec) {
            errorMsg = "Failed to move the staged install into place: " + dest.string() + " - " + ec.message();
            std::error_code restoreEc;
            fs::rename(retired, dest, restoreEc);   // put the working install back
            if (restoreEc)
                errorMsg += "; the previous install is at " + retired.string();
            return false;
        }

        removeTreeQuietly(retired);
        return true;
    }

    fs::rename(staged, dest, ec);
    if (ec) {
        errorMsg = "Failed to move the staged install into place: " + dest.string() + " - " + ec.message();
        return false;
    }
    return true;
}

static std::vector<std::string> splitString(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delim)) {
        parts.push_back(token);
    }
    return parts;
}

static std::string joinStrings(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

const char* installTypeToString(InstallType t) {
    switch (t) {
        case InstallType::Embedded: return "embedded";
        case InstallType::User:     return "user";
    }
    return "";
}

const char* dependencyStatusToString(DependencyStatus s) {
    switch (s) {
        case DependencyStatus::Installed:       return "installed";
        case DependencyStatus::NotInstalled:    return "not_installed";
        case DependencyStatus::Cycle:           return "cycle";
        case DependencyStatus::VersionMismatch: return "version_mismatch";
        case DependencyStatus::SignerMismatch:  return "signer_mismatch";
        case DependencyStatus::SignerUnknown:   return "signer_unknown";
    }
    return "";
}

namespace {

// How harshly an EDGE judged the package it points at; higher wins. One
// ranking decides both which constraint a node reports when an edge fails more
// than one (resolveDependencies) and which edge's verdict survives the flat
// list's name dedup (flatten), so the two cannot disagree.
//
// SignerUnknown ranks BELOW a definite failure so missing evidence never masks
// an actionable rejection; identity above version, because a range means
// nothing until you know which package you are ranging over. NotInstalled and
// Cycle score -1 — not edge verdicts, so they neither promote nor are promoted;
// absence returns early before this runs.
int edgeVerdictSeverity(DependencyStatus s)
{
    switch (s) {
        case DependencyStatus::Installed:       return 0;
        case DependencyStatus::SignerUnknown:   return 1;
        case DependencyStatus::VersionMismatch: return 2;
        case DependencyStatus::SignerMismatch:  return 3;
        case DependencyStatus::NotInstalled:
        case DependencyStatus::Cycle:           return -1;
    }
    return -1;
}

} // namespace

bool nodeResolvedToAnInstalledPackage(DependencyStatus s) {
    // By exclusion on purpose: the question is "is anything on disk", and
    // exactly two statuses mean there is not. A new edge-decided status is by
    // definition about a package that IS installed, so admit it by default.
    return s != DependencyStatus::NotInstalled && s != DependencyStatus::Cycle;
}

bool PackageManagerLib::versionGreaterOrEqual(const std::string& a, const std::string& b)
{
    // Delegates to the shared implementation in logos-package. This used to
    // split on '.' and atoi() each component, which parsed "1.0.0-rc1" as
    // 1.0.0 — so a pre-release compared equal to its own release and the
    // skipIfNotNewerVersion gate below refused to install over it.
    return logos::semver::greater_or_equal(a, b);
}

PackageManagerLib::PackageManagerLib()
{
}

PackageManagerLib::~PackageManagerLib()
{
}

void PackageManagerLib::setEmbeddedModulesDirectory(const std::string& dir)
{
    m_embeddedModulesDirs.clear();
    if (!dir.empty()) m_embeddedModulesDirs.push_back(dir);
}

void PackageManagerLib::addEmbeddedModulesDirectory(const std::string& dir)
{
    if (!dir.empty()) m_embeddedModulesDirs.push_back(dir);
}

void PackageManagerLib::setUserModulesDirectory(const std::string& dir)
{
    m_userModulesDir = dir;
}

void PackageManagerLib::setEmbeddedUiPluginsDirectory(const std::string& dir)
{
    m_embeddedUiPluginsDirs.clear();
    if (!dir.empty()) m_embeddedUiPluginsDirs.push_back(dir);
}

void PackageManagerLib::addEmbeddedUiPluginsDirectory(const std::string& dir)
{
    if (!dir.empty()) m_embeddedUiPluginsDirs.push_back(dir);
}

void PackageManagerLib::setUserUiPluginsDirectory(const std::string& dir)
{
    m_userUiPluginsDir = dir;
}

std::vector<std::string> PackageManagerLib::allModulesDirectories() const
{
    std::vector<std::string> dirs = m_embeddedModulesDirs;
    if (!m_userModulesDir.empty()) dirs.push_back(m_userModulesDir);
    return dirs;
}

std::vector<std::string> PackageManagerLib::allUiPluginsDirectories() const
{
    std::vector<std::string> dirs = m_embeddedUiPluginsDirs;
    if (!m_userUiPluginsDir.empty()) dirs.push_back(m_userUiPluginsDir);
    return dirs;
}

std::vector<std::string> PackageManagerLib::allDirectories() const
{
    std::vector<std::string> dirs = m_embeddedModulesDirs;
    if (!m_userModulesDir.empty()) dirs.push_back(m_userModulesDir);
    dirs.insert(dirs.end(), m_embeddedUiPluginsDirs.begin(), m_embeddedUiPluginsDirs.end());
    if (!m_userUiPluginsDir.empty()) dirs.push_back(m_userUiPluginsDir);
    return dirs;
}

namespace {

// "web-dev" -> "web". A build variant's "-dev" suffix is this binary's own
// contract (see platformVariantsToTry), so a variant name is compared without
// it wherever the comparison is about WHICH TARGET rather than which build.
std::string variantWithoutDevSuffix(const std::string& variant)
{
    static const std::string kDev = "-dev";
    if (variant.size() > kDev.size()
        && variant.compare(variant.size() - kDev.size(), kDev.size(), kDev) == 0)
        return variant.substr(0, variant.size() - kDev.size());
    return variant;
}

} // namespace

std::string PackageManagerLib::installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                                  bool skipIfNotNewerVersion,
                                                  std::string* installedPluginPath,
                                                  bool* isCoreModuleOut)
{
    fs::path sourcePath(pluginPath);
    if (!fs::exists(sourcePath) || !fs::is_regular_file(sourcePath)) {
        errorMsg = "Source plugin file does not exist or is not a file: " + pluginPath;
        return {};
    }

    std::string ext = sourcePath.extension().string();
    // Lowercase the extension
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".lgx") {
        errorMsg = "Only LGX packages are supported. Got: " + sourcePath.extension().string();
        return {};
    }

    // If requested, skip installation when an equal-or-higher version is already present.
    if (skipIfNotNewerVersion) {
        lgx_package_t pkg = lgx_load(pluginPath.c_str());
        if (pkg) {
            const char* rawName = lgx_get_name(pkg);
            const char* rawVersion = lgx_get_version(pkg);
            std::string incomingName = rawName ? rawName : "";
            std::string incomingVersion = rawVersion ? rawVersion : "";
            lgx_free_package(pkg);

            // incomingName comes from the untrusted package; only use it as a
            // path component if it is a valid single-segment module name, or a
            // crafted name ("../x", "/abs") would let this probe arbitrary
            // manifest.json files outside the install dirs (see F-006).
            if (!incomingName.empty() && !incomingVersion.empty() && isValidModuleName(incomingName)) {
                for (const auto& baseDir : allDirectories()) {
                    if (baseDir.empty())
                        continue;
                    fs::path manifestPath = fs::path(baseDir) / incomingName / "manifest.json";
                    if (fs::exists(manifestPath)) {
                        std::ifstream mf(manifestPath);
                        if (mf.is_open()) {
                            try {
                                json doc = json::parse(mf);
                                std::string installedVersion = doc.value("version", "");
                                if (!installedVersion.empty() && versionGreaterOrEqual(installedVersion, incomingVersion)) {
                                    std::string existingDir = (fs::path(baseDir) / incomingName).string();
                                    if (installedPluginPath) *installedPluginPath = existingDir;
                                    // Read the type off the manifest we just parsed. This used to
                                    // be hardcoded false, so a skipped CORE install reported
                                    // isCoreModule=false and package_manager_module emitted
                                    // uiPluginFileInstalled for a core module — Basecamp then
                                    // rescanned UI plugins instead of refreshing core modules.
                                    if (isCoreModuleOut) *isCoreModuleOut = (doc.value("type", "") == "core");
                                    return existingDir;
                                }
                            } catch (...) {
                                // ignore parse errors
                            }
                        }
                    }
                }
            }
        }
    }

    // ── The trust-anchor gate: may this package be installed at all? ────────
    //
    // This is AUTHORIZATION, and the ACTIVE ANCHOR SET is the only thing that
    // grants it. The anchor set is the local keyring alone (m_keyringDir,
    // managed by `lgx keyring` and addTrustedKey/removeTrustedKey), and nothing
    // enters it except by an explicit user act. A signer DID carried in the
    // package, advertised by a catalog entry, listed in a repository's
    // `trustedSigners`, or arriving with a downloaded key is a SELF-ASSERTION
    // and establishes no anchor — see logos-package-downloader's
    // Repository::trustedSignerDids, which is parsed and deliberately never
    // consulted.
    //
    // Do not confuse this with a dependency's `signer` pin. That pin only
    // DISAMBIGUATES among same-named candidates; satisfying it authorises
    // nothing. It is checked by the catalog resolver (logos-package-downloader,
    // PackageDownloaderLib::signerPinMatches) and against the installed set by
    // resolveDependencies in this file — never here.
    // Both checks are needed; neither substitutes for the other.
    if (m_signaturePolicy != SignaturePolicy::NONE) {
        auto sigResult = verifyPackageSignature(pluginPath);

        if (sigResult.is_signed && !sigResult.signature_valid) {
            errorMsg = "Invalid signature: " + (sigResult.error.empty() ? "unknown error" : sigResult.error);
            return {};
        }
        if (!sigResult.package_valid) {
            errorMsg = "Package validation failed: " + (sigResult.error.empty() ? "unknown error" : sigResult.error);
            return {};
        }
        if (!sigResult.is_signed && m_signaturePolicy == SignaturePolicy::REQUIRE) {
            errorMsg = "Package is unsigned and signature policy requires signatures";
            return {};
        }
        if (sigResult.is_signed && sigResult.trusted_as.empty() &&
            m_signaturePolicy == SignaturePolicy::REQUIRE) {
            errorMsg = "Package signed by untrusted key: " + sigResult.signer_did;
            return {};
        }
        if (m_signaturePolicy == SignaturePolicy::WARN) {
            // Every negative outcome that WARN nevertheless installs gets a
            // line. This used to warn ONLY about the unsigned case, so a
            // package carrying a valid signature from a key NO ACTIVE ANCHOR
            // VALIDATES installed in total silence — quieter than an unsigned
            // one, i.e. the worse posture produced less noise than the better.
            // At WARN the anchor set changes the DIAGNOSTIC; at REQUIRE it
            // changes the DECISION (above). That asymmetry is the point of
            // having two levels, and it only works if both levels speak.
            if (!sigResult.is_signed) {
                std::cerr << "Warning: Package is unsigned: " << pluginPath << "\n";
            } else if (sigResult.trusted_as.empty()) {
                // Name the DID: it is the one actionable thing here — what the
                // user would hand to `lgx keyring add` if they decided to
                // trust this publisher. signer_name/signer_url are the
                // package's own claims about itself and are reported as such,
                // never as corroboration.
                std::cerr << "Warning: Package is signed by a key no trusted anchor validates: "
                          << pluginPath << "\n"
                          << "  signer DID: "
                          << (sigResult.signer_did.empty() ? "(none reported)" : sigResult.signer_did)
                          << "\n";
                if (!sigResult.signer_name.empty()) {
                    std::cerr << "  signer claims to be: " << sigResult.signer_name
                              << " (self-asserted, not verified)\n";
                }
                std::cerr << "  checked against keyring: "
                          << (m_keyringDir.empty() ? "(default)" : m_keyringDir) << "\n";
            }
        }
    }

    // Create temporary directory for extraction
    std::string tempDir;
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        std::error_code tmpEc;
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::ostringstream oss;
            oss << "lgpm_extract_" << std::hex << dist(gen);
            auto candidate = fs::temp_directory_path(tmpEc) / oss.str();
            if (tmpEc) break;
            if (fs::create_directory(candidate, tmpEc) && !tmpEc) {
                tempDir = candidate.string();
                break;
            }
        }
        if (tempDir.empty()) {
            errorMsg = "Failed to create temporary directory for LGX extraction";
            return {};
        }
    }

    if (!extractLgxPackage(pluginPath, tempDir, errorMsg)) {
        removeTreeQuietly(tempDir);
        return {};
    }

    // Auto-detect module type from manifest.json in the extracted variant
    // directory. The INSTALL list, not the loader's: a Store shell installs the
    // variant it declared (setInstallVariants) and looking for its own native
    // one would miss the only directory in the package.
    auto variants = installVariantsToTry();
    std::string variantDir;
    for (const auto& v : variants) {
        fs::path candidate = fs::path(tempDir) / v;
        if (fs::is_directory(candidate)) {
            variantDir = candidate.string();
            break;
        }
    }


    // Determine module type from manifest.json "type" field
    std::string detectedType;
    if (!variantDir.empty()) {
        fs::path manifestPath = fs::path(variantDir) / "manifest.json";
        if (fs::exists(manifestPath)) {
            std::ifstream mf(manifestPath);
            if (mf.is_open()) {
                try {
                    json doc = json::parse(mf);
                    detectedType = doc.value("type", "");
                } catch (...) {}
            }
        }
    }
    // WHICH VARIANT WAS SELECTED, because for one of them the type does not
    // decide where this goes. `type: ui_qml` sends a DESKTOP package to the
    // ui-plugins directory, where a shell loads a Qt plugin out of it. A `web`
    // variant of the same package is not that thing: it is served into the Web
    // container, which is a CORE container, so the core has to DISCOVER it in a
    // modules directory or it is never loaded at all. Routed by type it would
    // install into a directory nothing scans and report success -- the same
    // silent shape as an install directory the core does not scan.
    const std::string selectedVariant =
        variantDir.empty() ? std::string{} : fs::path(variantDir).filename().string();
    const bool isWebVariant = variantWithoutDevSuffix(selectedVariant) == "web";

    bool isCoreModule = (detectedType == "core") || isWebVariant;
    if (isCoreModuleOut)
        *isCoreModuleOut = isCoreModule;

    // Always install to user directories
    std::string installDir;
    if (isCoreModule) {
        installDir = m_userModulesDir;
    } else {
        installDir = m_userUiPluginsDir;
    }

    if (installDir.empty()) {
        errorMsg = isCoreModule
            ? "User modules directory is not set. Cannot install plugin."
            : "User UI plugins directory is not set. Cannot install plugin.";
        removeTreeQuietly(tempDir);
        return {};
    }

    // Create install directory if it doesn't exist
    std::error_code ec;
    fs::create_directories(installDir, ec);
    if (ec) {
        errorMsg = "Failed to create install directory: " + installDir;
        removeTreeQuietly(tempDir);
        return {};
    }

    std::string installedModuleName;
    if (!copyLibraryFromExtracted(tempDir, installDir, isCoreModule, installedModuleName, errorMsg)) {
        removeTreeQuietly(tempDir);
        return {};
    }

    // Determine the path we report for what was just installed. This is the
    // main file when the package ships one and the module directory otherwise;
    // it is never empty here, because copyLibraryFromExtracted() has just
    // created that directory.
    if (installedPluginPath) {
        *installedPluginPath = resolveInstalledPackagePath(
            (fs::path(installDir) / installedModuleName).string(),
            installVariantsToTry());
    }

    removeTreeQuietly(tempDir);
    return installDir;
}

namespace {

// Defined below, next to the other file readers.
static std::optional<std::string> readFileBytes(const fs::path& path);

// The manifest's `main`, resolved by logos-package. Variant keys are keys of
// the signed hash tree that library writes, so it owns the resolution and this
// one asks. It also applies the guards this code never had: a path escaping the
// module directory, or naming a directory, is MALFORMED_ENTRY rather than a
// path handed back to a caller that cannot load it.
struct ResolvedMain {
    lgx_main_resolution_t state = LGX_MAIN_BAD_INPUT;
    std::string path;          // absolute; empty unless RESOLVED
    std::string declaredPath;  // relative, as the manifest wrote it
};

ResolvedMain resolveMain(const fs::path& moduleDir,
                         const std::string& manifestBytes,
                         const std::vector<std::string>& variants)
{
    std::vector<const char*> candidates;
    candidates.reserve(variants.size() + 1);
    for (const auto& v : variants)
        candidates.push_back(v.c_str());
    candidates.push_back(nullptr);

    lgx_main_file_t resolved = lgx_resolve_main(moduleDir.string().c_str(),
                                                manifestBytes.data(), manifestBytes.size(),
                                                candidates.data());
    ResolvedMain out;
    out.state = resolved.state;
    if (resolved.path) out.path = resolved.path;
    if (resolved.declared_path) out.declaredPath = resolved.declared_path;
    lgx_free_main_file(resolved);
    return out;
}

} // namespace

std::string PackageManagerLib::resolveInstalledPackagePath(const std::string& moduleDir,
                                                           const std::vector<std::string>& variants)
{
    fs::path dir(moduleDir);
    const std::string moduleName = dir.filename().string();

    std::string mainFile;
    bool isQmlPackage = false;

    if (const std::optional<std::string> manifestBytes = readFileBytes(dir / "manifest.json")) {
        try {
            isQmlPackage = (json::parse(*manifestBytes).value("type", "") == "ui_qml");
        } catch (...) {}

        // The DECLARED name, not the resolved path: the synthesis below appends
        // a platform extension to an extension-less one, so it works on the
        // name the manifest wrote. A main that resolved nowhere usable leaves
        // this empty and falls through to that synthesis.
        const ResolvedMain resolved = resolveMain(dir, *manifestBytes, variants);
        if (resolved.state == LGX_MAIN_RESOLVED || resolved.state == LGX_MAIN_FILE_MISSING)
            mainFile = resolved.declaredPath;
    }

    // Only non-ui_qml packages get a main file synthesised from the module
    // name: a QML-only ui_qml package has no backend library at all, and
    // guessing "<name>.so" for it would just miss.
    if (mainFile.empty() && !isQmlPackage)
        mainFile = moduleName;

    if (!mainFile.empty()) {
        if (!isQmlPackage && mainFile.find('.') == std::string::npos) {
            // Windows first: this chain is the shape that has repeatedly let a
            // Windows build fall through to the Unix branch.
#if defined(_WIN32)
            mainFile += ".dll";
#elif defined(__APPLE__)
            mainFile += ".dylib";
#else
            mainFile += ".so";
#endif
        }
        fs::path mainPath = dir / mainFile;
        if (fs::exists(mainPath))
            return mainPath.string();

        // The manifest named a main file that is not in the payload. The copy
        // itself succeeded, so this is a packaging defect rather than an
        // install failure — report the directory (below) but say so, instead
        // of silently handing back an empty path that reads as "install
        // failed". ui_qml is excluded: for those the name is only a guess.
        if (!isQmlPackage) {
            std::cerr << "Warning: package '" << moduleName << "' has no main file at "
                      << mainPath.string() << " after install; the package declares '"
                      << mainFile << "' but it is not in the payload. Reporting the "
                      << "module directory instead.\n";
        }
    }

    std::error_code ec;
    if (fs::is_directory(dir, ec) && !ec)
        return dir.string();
    return {};
}

namespace {

// Enriched scan record. `installType` tracks whether the winning copy lives
// in an embedded (shipped, read-only) directory or a user-writable one.
struct ScanEntry {
    std::string name;
    std::string type;
    std::string version;
    std::string installDir;
    std::string mainFilePath;
    InstallType installType = InstallType::User;
    // Declared dependency entries, string and object form alike (see
    // PackageDependency). The graph walks `.name`; the constraints ride along
    // for consumers that evaluate them.
    std::vector<PackageDependency> dependencies;
    // Declared but NOT required — never part of "is this install complete".
    std::vector<PackageDependency> optionalDependencies;
    std::vector<std::string> interfaceDependencies;
    // The DID the installed manifest.sig names, verified against the key that
    // DID itself carries — see InstalledPackage::signerDid. Display only.
    // nullopt = no usable signature.
    std::optional<std::string> signerDid;
    // manifest.json exactly as it sits on disk, NOT scan.manifest re-dumped: an
    // Ed25519 signature covers exact bytes and nlohmann round-tripping is not
    // byte-preserving, so a re-dump would fail to verify a good signature and
    // every pin in the fleet would read as a mismatch.
    std::string manifestBytes;
    std::optional<std::string> manifestSigJson;   // nullopt = no manifest.sig
    json manifest;  // full manifest.json — used to emit the JSON passthrough
};

// Read a whole file as bytes. nullopt when it is missing or unreadable; an
// EMPTY file reads as an empty string, and callers rely on the distinction.
static std::optional<std::string> readFileBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return std::nullopt;
    std::ostringstream buf;
    buf << f.rdbuf();
    if (f.bad())
        return std::nullopt;
    return buf.str();
}

// The DID an installed signature names, reported only if that signature really
// verifies under the key that DID carries. A SELF-check: did:jwk embeds its own
// key, so this proves only that somebody's real Ed25519 key signed exactly
// these bytes — enough to print, not enough to settle identity. Comparing it to
// a pin would check the pin against a key the package chose; the pin block in
// resolveDependencies is what decides identity.
static std::optional<std::string> selfAssertedSignerDid(
    const std::string& manifestBytes,
    const std::optional<std::string>& manifestSigJson)
{
    if (!manifestSigJson)
        return std::nullopt;

    std::string did;
    try {
        json sig = json::parse(*manifestSigJson);
        if (!sig.is_object() || !sig.contains("did") || !sig["did"].is_string())
            return std::nullopt;
        did = sig["did"].get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
    if (did.empty())
        return std::nullopt;

    if (lgx_check_manifest_signature(manifestBytes.data(), manifestBytes.size(),
                                     manifestSigJson->c_str(), did.c_str())
        != LGX_SIG_CHECK_OK) {
        return std::nullopt;
    }
    return did;
}

// Read one line out of a sidecar file inside an installed package directory,
// trimmed. Returns empty when the file is absent or holds nothing.
static std::string readSidecarLine(const fs::path& moduleDir, const char* fileName)
{
    std::ifstream f(moduleDir / fileName);
    if (!f.is_open())
        return {};
    std::string line;
    std::getline(f, line);
    // Trim trailing whitespace / CR so the comparison is exact.
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' ||
            line.back() == ' '  || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

// Reads the single-line `variant` file that installPluginFile writes into
// every installed module directory, recording which variant was physically
// extracted there. Returns empty if the file is absent (e.g. embedded or
// legacy installs) — callers must treat that as "unknown", not a mismatch.
static std::string readInstalledVariant(const fs::path& moduleDir)
{
    return readSidecarLine(moduleDir, "variant");
}

// Enumerate all manifests under the given embedded + user dir lists,
// deduping by package name with user-dir entries winning over embedded ones.
// `types` filter selects by manifest.type; empty = no filter.
static std::map<std::string, ScanEntry> enumerateManifests(
    const std::vector<std::string>& embeddedDirs,
    const std::vector<std::string>& userDirs,
    const std::vector<std::string>& types)
{
    std::map<std::string, ScanEntry> byName;
    auto variants = PackageManagerLib::platformVariantsToTry();

    auto scanOne = [&](const std::string& dirPath, InstallType installType) {
        std::error_code dirEc;
        if (!fs::is_directory(dirPath, dirEc) || dirEc)
            return;

        for (const auto& entry : fs::directory_iterator(
                 dirPath,
                 fs::directory_options::skip_permission_denied,
                 dirEc)) {
            if (dirEc) break;
            std::error_code entryEc;
            if (!entry.is_directory(entryEc) || entryEc)
                continue;

            // lgpm's own staging and retired trees are dot-prefixed siblings of
            // the real install. One can outlive a crash mid-swap, and it holds a
            // valid manifest.json, so it would shadow the module it was made
            // from. isValidModuleName keeps packages out of that namespace.
            const std::string dirName = entry.path().filename().string();
            if (!dirName.empty() && dirName.front() == '.')
                continue;

            fs::path manifestPath = entry.path() / "manifest.json";
            // Slurped, not streamed into the parser: the RAW bytes are the
            // signed message, and parsing from the string keeps both views.
            std::optional<std::string> manifestBytes = readFileBytes(manifestPath);
            if (!manifestBytes)
                continue;

            json manifest;
            try {
                manifest = json::parse(*manifestBytes);
            } catch (...) {
                continue;
            }

            if (!manifest.is_object())
                continue;

            std::string name = manifest.value("name", "");
            if (name.empty())
                continue;

            std::string moduleType = manifest.value("type", "");
            if (!types.empty()) {
                bool matches = false;
                for (const auto& t : types) {
                    if (moduleType == t) { matches = true; break; }
                }
                if (!matches) continue;
            }

            // User always wins — later scans for the same name clobber earlier ones.
            // Since we scan embedded first, then user, the map ends with the correct entry.
            ScanEntry scan;
            scan.name = name;
            scan.type = moduleType;
            scan.version = manifest.value("version", "");
            scan.installDir = entry.path().string();
            scan.installType = installType;
            // WHAT WAS EXTRACTED HERE IS WHAT `main` RESOLVES AGAINST.
            //
            // A manifest's `main` is a per-variant map, and an installed module
            // directory holds exactly ONE variant's files -- the one
            // installPluginFile unpacked, which it records in the `variant`
            // sidecar beside the manifest. Resolving against this host's native
            // list instead answers "no main" for every package installed for
            // anything else, and a package with no main is DROPPED by the
            // callers that matter: liblogos' module discovery skips a row whose
            // mainFilePath is empty, so the module is invisible with no error
            // anywhere.
            //
            // That is not a hypothetical mismatch. A Store shell installs the
            // `web` variant it declared (setInstallVariants, below) precisely
            // because it is NOT the variant this host loads natively -- a phone
            // may not download native code (ADR 0003) -- and the Web container
            // that runs it is a core container, so the core has to find it in a
            // modules directory. Measured on an iPad simulator, 2026-09-13: the
            // install succeeded, the files landed in the directory the core
            // scans, and the module never appeared.
            //
            // The native list stays the fallback, for embedded and legacy
            // installs that record no variant at all.
            //
            // Resolved before manifestBytes is moved from, below. Reported
            // against entry.path(), NOT the absolute path lgx_resolve_main
            // returns: installDir inherits the caller's form, and the two must
            // agree so a relative --modules-dir does not yield one of each.
            const std::string installedVariant = readInstalledVariant(entry.path());
            {
                std::vector<std::string> mainVariants = variants;
                if (!installedVariant.empty()) {
                    // The recorded name AND its build-suffix-free form: the
                    // sidecar records what was unpacked, and a `-dev` build
                    // writes a directory the published manifest spells without
                    // the suffix.
                    mainVariants = { installedVariant };
                    const std::string bare = variantWithoutDevSuffix(installedVariant);
                    if (bare != installedVariant)
                        mainVariants.push_back(bare);
                }
                const ResolvedMain m = resolveMain(entry.path(), *manifestBytes, mainVariants);
                if (m.state == LGX_MAIN_RESOLVED)
                    scan.mainFilePath = (entry.path() / m.declaredPath).string();
            }
            // Both halves verbatim — see ScanEntry::manifestBytes.
            scan.manifestBytes   = std::move(*manifestBytes);
            scan.manifestSigJson = readFileBytes(entry.path() / "manifest.sig");
            // Display only — a self-check; identity is decided by the pin block
            // in resolveDependencies.
            scan.signerDid = selfAssertedSignerDid(scan.manifestBytes, scan.manifestSigJson);

            // Surface the variant-mismatch silent-drop (logos-basecamp#191): a
            // module installed for a variant this build does not accept is
            // unloadable on this platform but otherwise scans clean. The
            // installed `variant` file records exactly what was extracted here,
            // so compare it against the supported list and emit one line per
            // affected module (naming the installed variant, the supported list,
            // and the directory) so the cause is greppable. Modules without a
            // variant file (embedded / legacy installs) are left untouched.
            // `web` is exempt, and for the reason the block above exists: it is
            // not a native image and this library cannot say whether it runs.
            // Whether it does depends on the host having registered a Web
            // container, which is a RUNTIME fact no compiled-in platform list
            // knows -- and installPluginFile routes a `web` variant into the
            // modules directory exactly so a core container can find it. Warning
            // there would tell a Store shell its one installable variant will
            // not load, every time it scans.
            if (!installedVariant.empty() &&
                variantWithoutDevSuffix(installedVariant) != "web" &&
                std::find(variants.begin(), variants.end(), installedVariant) == variants.end()) {
                std::cerr << "Warning: module '" << name << "' in " << entry.path().string()
                          << " was installed for variant '" << installedVariant
                          << "' which is not supported on this platform and will not be loadable: "
                          << "supported variants [" << joinStrings(variants, ", ") << "].\n";
            }

            // A `dependencies[]` entry is either a plain string or an object
            // carrying the same name plus the constraints an installer resolves
            // by (see the LGX spec's "Dependency entries", and lgx::Dependency
            // in logos-package). Both forms declare THE SAME EDGE.
            //
            // This used to accept only `d.is_string()` with no else branch, so
            // an object entry was skipped entirely — the graph lost the edge,
            // not merely its constraint. resolveDependencies under-reported,
            // resolveDependents under-reported (so an uninstall never warned
            // about a dependent that declared its need in object form), and
            // basecamp's missing-dependency marker went green on a module whose
            // dependency was not installed at all. No manifest in the workspace
            // uses the object form today, which is exactly why it went unseen.
            // One reader for both dependency arrays: they accept identical entry forms,
            // and a second copy is how they would come to disagree about what an
            // entry means.
            auto readDeps = [&](const char* field, std::vector<PackageDependency>& out) {
                if (!manifest.contains(field) || !manifest[field].is_array())
                    return;
                for (const auto& d : manifest[field]) {
                    PackageDependency dep;
                    if (d.is_string()) {
                        dep.name = d.get<std::string>();
                    } else if (d.is_object() && d.contains("name") && d["name"].is_string()) {
                        dep.name = d["name"].get<std::string>();
                        // PRESENT BUT NOT A STRING IS MALFORMED, NOT ABSENT.
                        // Gating the read on is_string() turns `{"signer": 42}`
                        // into "no pin" — fail-OPEN on the two fields whose job
                        // is to narrow an edge. The raw text parses as neither a
                        // semver range nor a did:jwk, so carrying it fails
                        // CLOSED and shows the operator the offending value.
                        if (d.contains("version")) {
                            dep.version = d["version"].is_string()
                                        ? d["version"].get<std::string>()
                                        : d["version"].dump();
                        }
                        if (d.contains("signer")) {
                            dep.signer = d["signer"].is_string()
                                       ? d["signer"].get<std::string>()
                                       : d["signer"].dump();
                        }
                    }
                    if (dep.name.empty()) {
                        std::cerr << "Warning: package '" << name << "' in "
                                  << entry.path().string()
                                  << " has a malformed " << field << "[] entry " << d.dump()
                                  << " (expected a non-empty name string, or an object with a "
                                  << "string 'name'); the dependency edge is ignored.\n";
                        continue;
                    }
                    out.push_back(std::move(dep));
                }
            };

            readDeps("dependencies", scan.dependencies);
            readDeps("optional_dependencies", scan.optionalDependencies);

            // Names only: an interface names no package, so there is nothing here to
            // resolve — it is carried for display.
            if (manifest.contains("interface_dependencies")
                && manifest["interface_dependencies"].is_array()) {
                for (const auto& i : manifest["interface_dependencies"])
                    if (i.is_string() && !i.get<std::string>().empty())
                        scan.interfaceDependencies.push_back(i.get<std::string>());
            }

            scan.manifest = std::move(manifest);

            byName[name] = std::move(scan);
        }
    };

    for (const auto& d : embeddedDirs) scanOne(d, InstallType::Embedded);
    for (const auto& d : userDirs)     scanOne(d, InstallType::User);

    return byName;
}

// Materialise a struct from a scan. Reads the manifest fields that
// downstream consumers actually use (field-use audit: name, version,
// description, type, category, author, license, icon, view, dependencies,
// hashes.root); the synthesised installDir / mainFilePath / installType
// come straight from the scan result. Missing fields default to empty —
// the manifest is already partially validated by enumerateManifests so
// name / type / version are present in any well-formed entry.
static InstalledPackage scanToInstalledPackage(const ScanEntry& scan)
{
    InstalledPackage p;
    p.name        = scan.name;
    p.version     = scan.version;
    p.type        = scan.type;
    p.installType = scan.installType;
    p.installDir  = scan.installDir;
    p.mainFilePath = scan.mainFilePath;
    p.signerDid = scan.signerDid;
    // Split the scanned entries into the name list every consumer already
    // reads and the constrained subset. Built here, in one place, so the two
    // views cannot drift.
    p.dependencies.reserve(scan.dependencies.size());
    for (const auto& d : scan.dependencies) {
        p.dependencies.push_back(d.name);
        if (!d.isSimple())
            p.dependencyConstraints.push_back(d);
    }
    // Carried whole rather than split into names: nothing walks a name-only
    // view of these, because they are not part of any graph the installer
    // completes.
    p.optionalDependencies = scan.optionalDependencies;
    p.interfaceDependencies = scan.interfaceDependencies;

    const json& m = scan.manifest;
    p.displayName = m.value("display_name", "");
    p.description = m.value("description", "");
    p.category    = m.value("category", "");
    p.author      = m.value("author", "");
    p.license     = m.value("license", "");
    p.icon        = m.value("icon", "");
    p.view        = m.value("view", "");
    p.manifestVersion = m.value("manifestVersion", "");

    if (m.contains("hashes") && m["hashes"].is_object()) {
        p.hashes.root = m["hashes"].value("root", "");
    }

    return p;
}

static std::vector<InstalledPackage> scansToInstalledPackages(
    const std::map<std::string, ScanEntry>& byName)
{
    std::vector<InstalledPackage> out;
    out.reserve(byName.size());
    for (const auto& [name, scan] : byName) {
        out.push_back(scanToInstalledPackage(scan));
    }
    return out;
}

} // namespace

std::vector<InstalledPackage> PackageManagerLib::getInstalledModules()
{
    auto byName = enumerateManifests(m_embeddedModulesDirs,
                                     m_userModulesDir.empty() ? std::vector<std::string>{}
                                                              : std::vector<std::string>{m_userModulesDir},
                                     {"core"});
    return scansToInstalledPackages(byName);
}

std::vector<InstalledPackage> PackageManagerLib::getInstalledUiPlugins()
{
    auto byName = enumerateManifests(m_embeddedUiPluginsDirs,
                                     m_userUiPluginsDir.empty() ? std::vector<std::string>{}
                                                                : std::vector<std::string>{m_userUiPluginsDir},
                                     {"ui", "ui_qml"});
    return scansToInstalledPackages(byName);
}

std::vector<InstalledPackage> PackageManagerLib::getInstalledPackages()
{
    std::vector<std::string> embeddedDirs = m_embeddedModulesDirs;
    embeddedDirs.insert(embeddedDirs.end(),
                        m_embeddedUiPluginsDirs.begin(), m_embeddedUiPluginsDirs.end());
    std::vector<std::string> userDirs;
    if (!m_userModulesDir.empty())   userDirs.push_back(m_userModulesDir);
    if (!m_userUiPluginsDir.empty()) userDirs.push_back(m_userUiPluginsDir);

    auto byName = enumerateManifests(embeddedDirs, userDirs, {});
    return scansToInstalledPackages(byName);
}

// Helper — enumerate every installed package across all four directory
// categories. Used for cross-category dependency resolution.
static std::map<std::string, ScanEntry> enumerateAllForResolve(
    const std::vector<std::string>& embeddedModulesDirs,
    const std::string& userModulesDir,
    const std::vector<std::string>& embeddedUiPluginsDirs,
    const std::string& userUiPluginsDir)
{
    std::vector<std::string> embedded = embeddedModulesDirs;
    embedded.insert(embedded.end(), embeddedUiPluginsDirs.begin(), embeddedUiPluginsDirs.end());
    std::vector<std::string> users;
    if (!userModulesDir.empty())   users.push_back(userModulesDir);
    if (!userUiPluginsDir.empty()) users.push_back(userUiPluginsDir);
    return enumerateManifests(embedded, users, {});
}

std::optional<DependencyTreeNode> PackageManagerLib::resolveDependencies(const std::string& packageName)
{
    auto byName = enumerateAllForResolve(m_embeddedModulesDirs, m_userModulesDir,
                                         m_embeddedUiPluginsDirs, m_userUiPluginsDir);

    if (byName.find(packageName) == byName.end())
        return std::nullopt;

    // Recursive walk. Visited-on-path set for cycle detection; the tree is
    // expanded across different branches (diamond shapes) but a cycle
    // through a single branch produces a Cycle leaf to stop descent.
    //
    // Takes the whole PackageDependency, not the name: the range and the signer
    // live on the EDGE, so they must travel with the recursion to be evaluated
    // at the node they constrain.
    std::set<std::string> path;
    std::function<DependencyTreeNode(const PackageDependency&, bool)> build =
        [&](const PackageDependency& dep, bool optional) -> DependencyTreeNode {
        DependencyTreeNode node;
        node.name = dep.name;
        // Recorded before any early return, so a constraint is visible even on
        // an edge whose status is decided without consulting it.
        node.requiredVersion = dep.version;
        node.requiredSigner  = dep.signer;
        node.optional        = optional;

        if (path.count(dep.name)) {
            node.status = DependencyStatus::Cycle;
            return node;
        }

        auto it = byName.find(dep.name);
        if (it == byName.end()) {
            // ABSENCE OUTRANKS MISMATCH: a range can only be judged against a
            // version we actually have, and "install it" is the fix either way.
            // The declared range still rides along on `requiredVersion`.
            node.status = DependencyStatus::NotInstalled;
            return node;
        }

        const auto& scan = it->second;
        node.version        = scan.version;
        node.installType    = scan.installType;
        node.signerDid      = scan.signerDid;
        // No range = no constraint. A range that does not PARSE counts as
        // unsatisfied rather than ignored: dropping a typo'd range fails open,
        // and `lgx verify` already rejects the syntax upstream
        // (logos::semver::valid_range), so one reaching here bypassed that gate.
        node.status = (!dep.version || logos::semver::satisfies(scan.version, *dep.version))
                          ? DependencyStatus::Installed
                          : DependencyStatus::VersionMismatch;

        // The signer pin: IS THIS THE SAME PACKAGE? THE PIN SUPPLIES THE KEY —
        // verify the installed manifest.sig over the installed manifest.json
        // bytes with the Ed25519 key the PINNED did:jwk embeds. The DID inside
        // the signature is deliberately NOT consulted: checked against the DID
        // sitting beside it, a signature only proves the file agrees with
        // itself, and an attacker swaps both. Nor is the keyring — that
        // answers ANCHORING, at install time.
        //
        // Not also a tamper check: ~450us and CONSTANT here where re-hashing is
        // linear (~47ms for a 50 MB module), on a path basecamp refreshes
        // constantly, and the hash tree is over TAR ENTRY PATHS while the
        // install tree is flattened and added to. That is `lgpm verify`.
        if (dep.signer) {
            DependencyStatus signerVerdict = DependencyStatus::Installed;
            if (!scan.manifestSigJson) {
                // Nothing proved and nothing disproved — see
                // UnknownSignerPolicy for why the default does not block.
                signerVerdict = (m_unknownSignerPolicy == UnknownSignerPolicy::Strict)
                                    ? DependencyStatus::SignerMismatch
                                    : DependencyStatus::SignerUnknown;
            } else {
                switch (lgx_check_manifest_signature(
                            scan.manifestBytes.data(), scan.manifestBytes.size(),
                            scan.manifestSigJson->c_str(), dep.signer->c_str())) {
                case LGX_SIG_CHECK_OK:
                    break;                       // the pinned key signed this
                case LGX_SIG_CHECK_MISMATCH:
                    signerVerdict = DependencyStatus::SignerMismatch;
                    break;
                case LGX_SIG_CHECK_BAD_DID:
                    // The PIN itself is not a did:jwk. It must not read as
                    // satisfied, and must not read as "unknown" either: the
                    // default policy tolerates unknown, so a typo'd pin would
                    // be waved through — the fail-open this exists to prevent.
                    std::cerr << "Warning: dependency '" << dep.name << "' is pinned to '"
                              << *dep.signer << "', which is not a did:jwk carrying an "
                              << "Ed25519 key; the pin cannot be satisfied by anything.\n";
                    signerVerdict = DependencyStatus::SignerMismatch;
                    break;
                case LGX_SIG_CHECK_UNUSABLE:
                    // Present but not a usable signature document. It refutes
                    // nothing, so it ranks with absence, not with a refusal.
                    signerVerdict = (m_unknownSignerPolicy == UnknownSignerPolicy::Strict)
                                        ? DependencyStatus::SignerMismatch
                                        : DependencyStatus::SignerUnknown;
                    break;
                }
            }
            // Max, not assignment: one edge can fail both constraints, and a
            // satisfied pin must never IMPROVE a status.
            if (edgeVerdictSeverity(signerVerdict) > edgeVerdictSeverity(node.status))
                node.status = signerVerdict;
        }

        // Warn once, where it is decided: SignerMismatch is definitive and its
        // remedy (this is somebody else's package) is not guessable from a bare
        // load failure. SignerUnknown deliberately does NOT warn — it is the
        // expected state for every embedded and every unsigned package, and it
        // travels on the status and on `lgpm info` instead.
        if (node.status == DependencyStatus::SignerMismatch && dep.signer) {
            std::cerr << "Warning: dependency '" << dep.name << "' is pinned to signer "
                      << *dep.signer << " but the installed package in " << scan.installDir
                      << " was not signed by that key (it is signed by "
                      << (scan.signerDid ? *scan.signerDid
                                         : std::string("no key whose signature verifies"))
                      << "); it is a different package under the same name.\n";
        }

        path.insert(dep.name);
        node.children.reserve(scan.dependencies.size() + scan.optionalDependencies.size());
        // A required child INHERITS this node's optionality: a package you only
        // need because you installed something you did not have to install is
        // not itself required, so an absent one below an optional edge must not
        // read as a broken install either.
        for (const auto& child : scan.dependencies) {
            node.children.push_back(build(child, optional));
        }
        // Optional edges appear in the tree so a UI can offer them, marked so
        // nothing reads an absent one as broken.
        for (const auto& child : scan.optionalDependencies) {
            node.children.push_back(build(child, /*optional=*/true));
        }
        path.erase(dep.name);
        return node;
    };

    // The root is not pointed at by any edge, so it carries no constraint and
    // no optionality.
    return build(PackageDependency(packageName), /*optional=*/false);
}

std::optional<DependentTreeNode> PackageManagerLib::resolveDependents(const std::string& packageName)
{
    auto byName = enumerateAllForResolve(m_embeddedModulesDirs, m_userModulesDir,
                                         m_embeddedUiPluginsDirs, m_userUiPluginsDir);

    // Only installed packages get rooted trees — matches resolveDependencies.
    auto rootIt = byName.find(packageName);
    if (rootIt == byName.end())
        return std::nullopt;

    // Reverse adjacency: name → [names that depend on it].
    // Every manifest's dependencies[] entry must reference the canonical
    // manifest.name of its target exactly — no normalization. Packages
    // with incorrect dependency declarations are bugs in those packages.
    std::unordered_map<std::string, std::vector<std::string>> reverseDeps;
    for (const auto& [name, scan] : byName) {
        for (const auto& d : scan.dependencies) {
            reverseDeps[d.name].push_back(name);
        }
    }

    // Fill a node from its ScanEntry. Reverse edges are synthesised only
    // from installed manifests, so every node reached during the walk is
    // present in byName; no NotInstalled/Cycle status distinction needed.
    auto fillFromScan = [](DependentTreeNode& node, const ScanEntry& scan) {
        node.name        = scan.name;
        node.version     = scan.version;
        node.type        = scan.type;
        node.installType = scan.installType;
        node.installDir  = scan.installDir;
    };

    // Recursive build with on-path cycle detection. A cycle ends the
    // descent silently (node without children) — there's no "Cycle"
    // status to emit because the forward-tree's tri-state status field
    // doesn't apply to reverse trees.
    std::set<std::string> path;
    std::function<DependentTreeNode(const std::string&)> build = [&](const std::string& name) -> DependentTreeNode {
        DependentTreeNode node;
        auto it = byName.find(name);
        if (it != byName.end()) fillFromScan(node, it->second);
        else                    node.name = name;   // unreachable in practice

        if (path.count(name))
            return node;

        auto rit = reverseDeps.find(name);
        if (rit == reverseDeps.end())
            return node;

        path.insert(name);
        node.children.reserve(rit->second.size());
        for (const auto& depender : rit->second) {
            node.children.push_back(build(depender));
        }
        path.erase(name);
        return node;
    };

    return build(packageName);
}

// ---------------------------------------------------------------------------
// DependencyTreeNode::flatten / DependentTreeNode::flatten
// ---------------------------------------------------------------------------
// Both flavours share the same BFS-dedup shape. Kept as two hand-rolled
// copies rather than a template — they walk different node types but are
// each a ~15 line function, and templating would either require putting
// the body in a header or a shared private helper whose generality
// doesn't pay off at two call sites. Children vectors on the returned
// copies are cleared so the flat output is canonical (no hidden
// substructure, no double-counting under iteration).

std::vector<DependencyTreeNode> DependencyTreeNode::flatten() const
{
    std::vector<DependencyTreeNode> out;
    std::unordered_map<std::string, std::size_t> indexByName;
    std::deque<const DependencyTreeNode*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty()) {
        const DependencyTreeNode* n = queue.front();
        queue.pop_front();
        auto [slot, inserted] = indexByName.emplace(n->name, out.size());
        if (!inserted) {
            // Already reported under this name. One row per package stays the
            // contract, so this appends nothing — but the recorded row came
            // from whichever edge BFS reached FIRST (the shallowest), and a
            // package satisfies its dependants only if it satisfies ALL of
            // them. So a later edge that judges it MORE HARSHLY promotes the
            // row, carrying its constraint so the report can name what failed.
            // Ranked by edgeVerdictSeverity so a new status is placed in one
            // ranking instead of a chain of pairwise ifs; the -1 statuses
            // (NotInstalled, Cycle) neither promote nor are promoted.
            //
            // `signerDid` is NOT carried across — it belongs to the installed
            // package and is identical on every edge, whereas requiredVersion /
            // requiredSigner belong to the promoting edge.
            DependencyTreeNode& recorded = out[slot->second];
            const int recordedRank = edgeVerdictSeverity(recorded.status);
            const int candidateRank = edgeVerdictSeverity(n->status);
            if (recordedRank >= 0 && candidateRank > recordedRank) {
                recorded.status          = n->status;
                recorded.requiredVersion = n->requiredVersion;
                recorded.requiredSigner  = n->requiredSigner;
            }
            // Optionality collapses the same way, and in the CONSERVATIVE
            // direction: reachable by any required edge means required, however
            // many optional edges also reach it. Unlike the status promotion
            // this is not ranked — one required edge settles it — and it is
            // deliberately independent of that ranking, since a row can be
            // promoted to required by an edge whose status is no worse.
            recorded.optional = recorded.optional && n->optional;
            continue;
        }
        DependencyTreeNode copy;
        copy.name            = n->name;
        copy.status          = n->status;
        copy.version         = n->version;
        copy.installType     = n->installType;
        copy.requiredVersion = n->requiredVersion;
        copy.requiredSigner  = n->requiredSigner;
        copy.signerDid       = n->signerDid;
        // Must travel: dropping it here hands a flat-list consumer an unmarked
        // NotInstalled node for a package nothing requires.
        copy.optional        = n->optional;
        out.push_back(std::move(copy));
        for (const auto& c : n->children) queue.push_back(&c);
    }
    return out;
}

std::vector<DependentTreeNode> DependentTreeNode::flatten() const
{
    std::vector<DependentTreeNode> out;
    std::unordered_set<std::string> seen;
    std::deque<const DependentTreeNode*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty()) {
        const DependentTreeNode* n = queue.front();
        queue.pop_front();
        if (!seen.insert(n->name).second) continue;
        DependentTreeNode copy;
        copy.name        = n->name;
        copy.version     = n->version;
        copy.type        = n->type;
        copy.installType = n->installType;
        copy.installDir  = n->installDir;
        out.push_back(std::move(copy));
        for (const auto& c : n->children) queue.push_back(&c);
    }
    return out;
}

UninstallResult PackageManagerLib::uninstallPackage(const std::string& packageName)
{
    UninstallResult result;

    auto byName = enumerateAllForResolve(m_embeddedModulesDirs, m_userModulesDir,
                                         m_embeddedUiPluginsDirs, m_userUiPluginsDir);
    auto it = byName.find(packageName);
    if (it == byName.end()) {
        result.errorMsg = "Package not found: " + packageName;
        return result;
    }

    const auto& scan = it->second;
    if (scan.installType == InstallType::Embedded) {
        result.errorMsg = "Cannot uninstall embedded package: " + packageName;
        return result;
    }
    if (scan.installDir.empty()) {
        result.errorMsg = "Package has no install directory: " + packageName;
        return result;
    }

    std::error_code ec;
    // Guard against anything weird happening with the resolved path: require
    // that the directory lives under one of the configured user directories.
    fs::path toDelete = fs::absolute(scan.installDir, ec);
    if (ec || !fs::exists(toDelete, ec)) {
        result.errorMsg = "Install directory missing: " + scan.installDir;
        return result;
    }
    bool insideUserDir = false;
    for (const auto* d : { &m_userModulesDir, &m_userUiPluginsDir }) {
        if (d->empty()) continue;
        std::error_code pec;
        fs::path userRoot = fs::absolute(*d, pec);
        if (pec) continue;
        auto rel = fs::relative(toDelete, userRoot, pec);
        if (pec) continue;
        std::string rs = rel.string();
        if (!rs.empty() && rs.rfind("..", 0) != 0) {
            insideUserDir = true;
            break;
        }
    }
    if (!insideUserDir) {
        result.errorMsg = "Refusing to uninstall path outside user directories: " + toDelete.string();
        return result;
    }

    // Same read-only trap as the temp cleanup above, but here the failure is
    // load-bearing rather than cosmetic: an installed package whose icon came
    // out of the Nix store carries FILE_ATTRIBUTE_READONLY, so on Windows every
    // uninstall and every upgrade failed with
    //   "Failed to remove install directory: Access is denied"
    // while the same call succeeds on POSIX, which needs only the parent
    // directory's write bit. Clear the attributes and retry before reporting.
    std::uintmax_t removed = fs::remove_all(toDelete, ec);
    if (ec) {
        clearReadOnlyRecursive(toDelete);
        ec.clear();
        removed = fs::remove_all(toDelete, ec);
    }
    if (ec) {
        result.errorMsg = "Failed to remove install directory: " + ec.message();
        return result;
    }

    result.success = true;
    result.removedFiles.push_back(toDelete.string());
    (void)removed;
    return result;
}

namespace {
// Deliberately a plain global rather than something inferred from the
// environment: a silent platform switch would defeat the fail-closed check that
// stops a Windows package being installed as a Linux one.
std::string g_platformVariantOverride;
}

void PackageManagerLib::setPlatformVariantOverride(const std::string& variant)
{
    g_platformVariantOverride = variant;
}

std::string PackageManagerLib::platformVariantOverride()
{
    return g_platformVariantOverride;
}

std::string PackageManagerLib::currentPlatformVariant()
{
    if (!g_platformVariantOverride.empty()) {
        return g_platformVariantOverride;
    }
    // Compile-time, and logos-package's to compute: it writes the variant names
    // into the signed hash tree, so it owns their vocabulary.
    return lgx_host_variant();
}

std::vector<std::string> PackageManagerLib::platformVariantsToTry()
{
    std::vector<std::string> variants;
    // The host's own spelling always leads: it is what diagnostics print as
    // "this machine", and what errorMsg names as the platform that was wanted.
    const char** spellings = lgx_variant_spellings(currentPlatformVariant().c_str());
    if (spellings) {
        for (const char** s = spellings; *s != nullptr; ++s)
            variants.emplace_back(*s);
        lgx_free_string_array(spellings);
    }

    // "-dev" is a contract of THIS binary's build variant, which liblgx cannot
    // know. Expanded spellings first, then suffixed: "darwin-arm64-dev" aliases
    // nothing.
#ifndef LGPM_PORTABLE_BUILD
    for (auto& variant : variants) {
        variant += "-dev";
    }
#endif

    return variants;
}

void PackageManagerLib::setInstallVariants(const std::vector<std::string>& variants)
{
    m_installVariants = variants;
}

std::vector<std::string> PackageManagerLib::installVariantsToTry() const
{
    // No declaration: this host installs what it loads, which is every caller
    // that is not a Store shell.
    if (m_installVariants.empty())
        return platformVariantsToTry();

    // Expanded through liblgx's spellings for the same reason the loader's list
    // is: the vocabulary belongs to logos-package, which writes these names into
    // the signed hash tree, and a second table here would be a second answer.
    // A variant liblgx has no aliases for still names itself, so nothing is
    // lost by a declaration it has never heard of.
    std::vector<std::string> variants;
    for (const std::string& declared : m_installVariants) {
        const char** spellings = lgx_variant_spellings(declared.c_str());
        if (!spellings) {
            variants.push_back(declared);
            continue;
        }
        for (const char** sp = spellings; *sp != nullptr; ++sp)
            variants.emplace_back(*sp);
        lgx_free_string_array(spellings);
    }
    return variants;
}

bool PackageManagerLib::isValidModuleName(const std::string& name)
{
    // A module name becomes a single directory component under the install
    // directory, so anything that isn't a plain leaf segment is rejected. This
    // blocks the path-traversal vectors that std::filesystem::operator/ would
    // otherwise honor: ".." escapes upward, an absolute path resets the join,
    // and embedded separators create nested or sibling directories.
    if (name.empty() || name == "." || name == "..")
        return false;

    // A leading '.' is lgpm's own namespace -- the staging and retired trees a
    // swap creates are dot-prefixed siblings, and enumerateManifests skips the
    // whole prefix. A package claiming it would install and then never be
    // listed.
    if (name.front() == '.')
        return false;

    for (char c : name) {
        // '/' and '\\' are path separators; ':' is a Windows drive separator
        // ("C:foo" is drive-relative); '\0' truncates the path at the C ABI.
        if (c == '/' || c == '\\' || c == ':' || c == '\0')
            return false;
    }

    return true;
}

bool PackageManagerLib::copyDirectoryContents(const std::string& srcDir, const std::string& destDir, std::string& errorMsg)
{
    if (!fs::is_directory(srcDir)) {
        errorMsg = "Source directory does not exist: " + srcDir;
        return false;
    }

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        errorMsg = "Failed to create destination directory: " + destDir;
        return false;
    }

    std::error_code iterEc;
    for (const auto& entry : fs::directory_iterator(srcDir, fs::directory_options::skip_permission_denied, iterEc)) {
        if (iterEc) {
            errorMsg = "Failed to iterate source directory: " + srcDir;
            return false;
        }
        fs::path destPath = fs::path(destDir) / entry.path().filename();

        std::error_code statusEc;
        if (entry.is_directory(statusEc) && !statusEc) {
            if (!copyDirectoryContents(entry.path().string(), destPath.string(), errorMsg)) {
                return false;
            }
        } else if (!statusEc) {
            if (fs::exists(destPath, statusEc) && !statusEc) {
                fs::remove(destPath, ec);
                if (ec) {
                    errorMsg = "Failed to remove existing file: " + destPath.string();
                    return false;
                }
            }
            fs::copy_file(entry.path(), destPath, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                errorMsg = "Failed to copy file from " + entry.path().string() + " to " + destPath.string();
                return false;
            }
        }
    }

    return true;
}

bool PackageManagerLib::extractLgxPackage(const std::string& lgxPath, const std::string& outputDir, std::string& errorMsg)
{
    lgx_package_t pkg = lgx_load(lgxPath.c_str());
    if (!pkg) {
        errorMsg = std::string("Failed to load LGX package: ") + lgx_get_last_error();
        return false;
    }

    // The INSTALL list: this is the gate a Store shell's `web` variant has to
    // pass, and it is the FIRST one -- reached before anything is extracted, so
    // a declaration honoured only below here would never see a directory.
    auto variants = installVariantsToTry();
    std::string matchedVariant;

    for (const auto& v : variants) {
        if (lgx_has_variant(pkg, v.c_str())) {
            matchedVariant = v;
            break;
        }
    }

    if (matchedVariant.empty()) {
        // Collect the variants the package actually provides so the failure is
        // self-explanatory. The common cause is a dev/portable mismatch: a
        // package built for "<host>-dev" installed by a portable build that only
        // accepts "<host>" (or vice versa). Naming both lists turns a silent
        // "Package does not contain variant" into an actionable line.
        std::vector<std::string> available;
        const char** pkgVariants = lgx_get_variants(pkg);
        if (pkgVariants) {
            for (size_t i = 0; pkgVariants[i] != nullptr; ++i)
                available.push_back(pkgVariants[i]);
            lgx_free_string_array(pkgVariants);
        }
        std::string availableList = available.empty() ? "none" : joinStrings(available, ", ");
        std::cerr << "Warning: package '" << lgxPath
                  << "' has no variant matching this platform: tried ["
                  << joinStrings(variants, ", ") << "], package provides ["
                  << availableList << "].\n";
        errorMsg = "Package does not contain variant for platform: " + variants.front()
                 + " (package provides: " + availableList + ")";
        lgx_free_package(pkg);
        return false;
    }

    lgx_result_t result = lgx_extract(pkg, matchedVariant.c_str(), outputDir.c_str());

    if (!result.success) {
        errorMsg = std::string("Failed to extract variant: ") + (result.error ? result.error : "unknown error");
        lgx_free_package(pkg);
        return false;
    }

    // Write the full root manifest.json from the LGX package into the extracted variant directory
    fs::path variantOutputDir = fs::path(outputDir) / matchedVariant;
    fs::path manifestPath = variantOutputDir / "manifest.json";

    const char* manifestJson = lgx_get_manifest_json(pkg);
    if (!manifestJson) {
        errorMsg = std::string("Failed to get manifest JSON from LGX package: ") + lgx_get_last_error();
        lgx_free_package(pkg);
        return false;
    }

    {
        // BINARY, and it is load-bearing: these bytes are the signed message.
        // A text-mode stream translates '\n' to "\r\n" on Windows and
        // readFileBytes() reads BINARY, so the translation is never undone and
        // the signature can never verify — silently, and Windows-only, as a
        // SignerMismatch on a package the pinned key really did sign.
        std::ofstream mf(manifestPath, std::ios::binary);
        if (!mf.is_open()) {
            errorMsg = "Failed to write manifest.json to: " + manifestPath.string();
            lgx_free_package(pkg);
            return false;
        }
        mf << manifestJson;
        if (!mf.good()) {
            errorMsg = "Failed to write data to manifest.json at: " + manifestPath.string();
            lgx_free_package(pkg);
            return false;
        }
    }

    // Carry the signature to where the signed bytes are going. manifest.json
    // above comes from lgx_get_manifest_json(), the SAME expression
    // Package::signPackage signs, so the installed manifest is byte-identical
    // to what the signature covers and a detached check stays meaningful long
    // after the .lgx is deleted. A package that ships no signature leaves
    // nothing here and reads as "nobody knows", exactly as before.
    //
    // Written BEFORE the payload copy deliberately: Package::extractVariant
    // re-adds owner_write to every file it writes, so a package shipping its
    // own read-only variants/<v>/manifest.sig cannot block the real one.
    if (const char* sigJson = lgx_get_manifest_sig_json(pkg)) {
        fs::path sigPath = variantOutputDir / "manifest.sig";
        std::ofstream sf(sigPath, std::ios::binary | std::ios::trunc);
        if (!sf.is_open() || !(sf << sigJson) || !sf.good()) {
            // Not fatal — the install only NEEDS manifest.json, and a missing
            // signature is already a defined, safe state. But say it: the copy
            // would otherwise read as unsigned forever.
            std::cerr << "Warning: package '" << lgxPath << "' is signed, but its signature "
                      << "could not be written to " << sigPath.string()
                      << "; the installed copy will read as having no known publisher.\n";
        }
    }

    // Write a "variant" text file containing the installed variant name
    {
        fs::path variantFilePath = variantOutputDir / "variant";
        std::ofstream vf(variantFilePath);
        if (vf.is_open()) {
            vf << matchedVariant;
        }
    }

    lgx_free_package(pkg);
    return true;
}

void PackageManagerLib::setSignaturePolicy(SignaturePolicy policy)
{
    m_signaturePolicy = policy;
}

void PackageManagerLib::setKeyringDirectory(const std::string& dir)
{
    m_keyringDir = dir;
}

void PackageManagerLib::setUnknownSignerPolicy(UnknownSignerPolicy policy)
{
    m_unknownSignerPolicy = policy;
}

SignatureVerificationResult PackageManagerLib::verifyPackageSignature(const std::string& lgxPath)
{
    SignatureVerificationResult result;
    const char* krDir = m_keyringDir.empty() ? nullptr : m_keyringDir.c_str();
    lgx_signature_info_t info = lgx_verify_signature(lgxPath.c_str(), krDir);

    result.is_signed = info.is_signed;
    result.signature_valid = info.signature_valid;
    result.package_valid = info.package_valid;
    if (info.signer_did) result.signer_did = info.signer_did;
    if (info.signer_name) result.signer_name = info.signer_name;
    if (info.signer_url) result.signer_url = info.signer_url;
    if (info.trusted_as) result.trusted_as = info.trusted_as;
    if (info.error) result.error = info.error;

    lgx_free_signature_info(info);
    return result;
}

bool PackageManagerLib::copyLibraryFromExtracted(const std::string& extractedDir, const std::string& targetDir,
                                                  bool /* isCoreModule */, std::string& outModuleName, std::string& errorMsg)
{
    auto variants = installVariantsToTry();
    std::string variantDir;

    for (const auto& v : variants) {
        fs::path candidate = fs::path(extractedDir) / v;
        if (fs::is_directory(candidate)) {
            variantDir = candidate.string();
            break;
        }
    }

    if (variantDir.empty()) {
        std::string variantList;
        for (size_t i = 0; i < variants.size(); ++i) {
            if (i > 0) variantList += ", ";
            variantList += variants[i];
        }
        errorMsg = "Extracted variant directory not found for: " + variantList;
        return false;
    }

    // Determine the module name from manifest.json "name" field
    std::string moduleName;
    fs::path manifestPath = fs::path(variantDir) / "manifest.json";
    if (fs::exists(manifestPath)) {
        std::ifstream mf(manifestPath);
        if (mf.is_open()) {
            try {
                json doc = json::parse(mf);
                moduleName = doc.value("name", "");
            } catch (...) {}
        }
    }

    // Fall back to the first library file's base name if manifest name is unavailable
    if (moduleName.empty()) {
        std::vector<std::string> extensions;
#if defined(__APPLE__)
        extensions.push_back(".dylib");
#elif defined(_WIN32)
        extensions.push_back(".dll");
#else
        extensions.push_back(".so");
#endif
        for (const auto& entry : fs::directory_iterator(variantDir)) {
            if (!entry.is_regular_file())
                continue;
            for (const auto& ext : extensions) {
                if (entry.path().extension() == ext) {
                    moduleName = entry.path().stem().string();
                    break;
                }
            }
            if (!moduleName.empty())
                break;
        }
        if (moduleName.empty()) {
            errorMsg = "No library files found and no name in manifest for: " + variantDir;
            return false;
        }
    }

    // The module name (whether from the manifest or the library-file fallback)
    // is about to become a directory component under targetDir. Reject anything
    // that isn't a single plain segment before the join, or a crafted manifest
    // "name" like "../../x" or "/abs" would let the copy escape targetDir and
    // plant files (e.g. an auto-loaded plugin) anywhere on disk — F-006.
    if (!isValidModuleName(moduleName)) {
        errorMsg = "Invalid module name in manifest: " + moduleName;
        return false;
    }

    outModuleName = moduleName;
    std::string moduleSubDir = (fs::path(targetDir) / moduleName).string();

    // Defense in depth: even with the name validated, confirm the resolved
    // destination is lexically contained within targetDir before any write
    // happens. weakly_canonical resolves "."/".." without requiring the path to
    // exist, so this runs ahead of copyDirectoryContents.
    {
        fs::path canonicalTarget = fs::weakly_canonical(fs::path(targetDir));
        fs::path canonicalSub = fs::weakly_canonical(fs::path(moduleSubDir));
        auto rel = fs::relative(canonicalSub, canonicalTarget);
        // Compare the first COMPONENT rather than doing a string prefix test.
        // fs::path::native() is std::wstring on Windows, so rfind("..", 0) does
        // not even compile there -- and the prefix test was imprecise anyway: it
        // also matched a legitimate directory literally named "..foo", rejecting
        // a path that does not actually escape. Component comparison is portable
        // and exact. (Short-circuits, so *rel.begin() is only reached when rel
        // is non-empty.)
        if (rel.empty() || *rel.begin() == fs::path("..")) {
            errorMsg = "Resolved install path escapes target directory: " + moduleSubDir;
            return false;
        }
    }

    // Build the new install beside the destination and swap it in, rather than
    // copying over the old tree. Copying in place MERGED the two: a copy that
    // failed midway left a mixture that was neither install (measured at six of
    // eight payload files, with no manifest.json and no variant), and an
    // upgrade that dropped a file left the old one behind forever. Staging is a
    // sibling so the swap is a rename on the same filesystem.
    //
    // It also sidesteps the wedge this class of bug is named for: a destination
    // left mode 0555 -- what `cp -R` out of `nix build .#install` produces,
    // since the Nix store is 0555/0444 and cp without -p keeps both -- refuses
    // every NEW file, so the old in-place copy could never overwrite it.
    fs::path stagingDir = fs::path(targetDir) / reservedSiblingName("staging", moduleName);

    if (!copyDirectoryContents(variantDir, stagingDir.string(), errorMsg)) {
        removeTreeQuietly(stagingDir);
        return false;
    }

    // Nothing is carried over from the previous install, manifest.sig included.
    // The merge used to leave it standing over a manifest it did not sign, so
    // an unsigned reinstall was refused as SignerMismatch -- blocking, but for
    // the wrong reason. What is installed is now exactly what the package
    // shipped; an unsigned one reads as SignerUnknown, and closing that is the
    // unknown-signer POLICY's job, not the copier's
    // (A2_AnUnsignedReinstallLeavesNoSignatureBehind).

    // The payload carries whatever modes the archive recorded. Windows refuses
    // to delete a read-only file, so leaving them breaks the next uninstall.
    clearReadOnlyRecursive(stagingDir);

    if (!swapIntoPlace(stagingDir, fs::path(moduleSubDir), errorMsg)) {
        removeTreeQuietly(stagingDir);
        return false;
    }

    return true;
}
