#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <lgx.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

/**
 * RAII for the process-wide platform override.
 *
 * platformVariantOverride() is a plain global, so a test that pretends to be
 * another host has to put it back or it leaks into every later test in this
 * binary. Restoring the PREVIOUS value rather than "" keeps these tests
 * composable with a caller that set one.
 */
class ScopedPlatformOverride {
public:
    explicit ScopedPlatformOverride(const std::string& variant)
        : m_previous(PackageManagerLib::platformVariantOverride())
    {
        PackageManagerLib::setPlatformVariantOverride(variant);
    }
    ~ScopedPlatformOverride() { PackageManagerLib::setPlatformVariantOverride(m_previous); }

private:
    std::string m_previous;
};

/**
 * A non-portable build appends "-dev" to every accepted variant and a portable
 * one does not. LGPM_PORTABLE_BUILD is a PRIVATE compile definition of
 * package_manager_lib, so the test binary cannot read it -- accept either
 * spelling instead of guessing which build this is.
 */
bool accepts(const std::vector<std::string>& variants, const std::string& bare)
{
    return std::find(variants.begin(), variants.end(), bare) != variants.end()
        || std::find(variants.begin(), variants.end(), bare + "-dev") != variants.end();
}

std::string join(const std::vector<std::string>& v)
{
    std::string out;
    for (const auto& s : v) {
        if (!out.empty()) out += ", ";
        out += s;
    }
    return "[" + out + "]";
}

} // namespace

// =============================================================================
// Shape of the host's own variant
// =============================================================================

TEST(VariantTest, CurrentPlatformVariantNotEmpty) {
    std::string variant = PackageManagerLib::currentPlatformVariant();
    EXPECT_FALSE(variant.empty());
    EXPECT_NE(variant, "unknown");
}

TEST(VariantTest, CurrentPlatformVariantFormat) {
    std::string variant = PackageManagerLib::currentPlatformVariant();
    // Should contain a dash separating OS and arch
    EXPECT_NE(variant.find('-'), std::string::npos);
}

TEST(VariantTest, PlatformVariantsToTryNotEmpty) {
    auto variants = PackageManagerLib::platformVariantsToTry();
    EXPECT_FALSE(variants.empty());
}

TEST(VariantTest, HostOwnVariantIsAlwaysFirst) {
    // The head of the list is what diagnostics print as "this machine", and
    // what test helpers elsewhere in this suite use to build a package the
    // host will accept. Aliasing must widen the tail, never displace the head.
    const std::string primary = PackageManagerLib::currentPlatformVariant();
    auto variants = PackageManagerLib::platformVariantsToTry();
    ASSERT_FALSE(variants.empty());
    EXPECT_EQ(variants.front().rfind(primary, 0), 0u)
        << "first entry " << variants.front() << " does not start with " << primary;
}

// =============================================================================
// Producer/consumer spelling agreement
//
// This is a TABLE over every target this ecosystem ships, not a probe of the
// host, because the defect being pinned is invisible from any single host: an
// arm64 Mac never computes darwin-x86_64 and so never meets it.
//
// Left column is what a host computes for itself (currentPlatformVariant);
// right column is a spelling some producer actually writes into a .lgx, so it
// has to resolve on that host. The producers disagree with EACH OTHER --
//
//   nix-bundle-lgx/flake.nix:52                  darwin-amd64  linux-amd64
//                                                darwin-arm64  linux-arm64
//   nix-bundle-logos-module-install/flake.nix:49 darwin-x86_64 linux-x86_64
//                                                darwin-arm64  linux-arm64
//
// -- and the second CONSUMES the first, calling lgpm --platform with its own
// spelling on a package the first named. So both columns must resolve here.
// =============================================================================

namespace {
struct VariantAliasCase {
    const char* host;          // what currentPlatformVariant() would return
    const char* alsoAccepted;  // a producer spelling that must still resolve
};

const VariantAliasCase kAliasCases[] = {
    // linux: bridged before this change
    { "linux-x86_64",   "linux-amd64"     },
    { "linux-amd64",    "linux-x86_64"    },
    { "linux-arm64",    "linux-aarch64"   },
    { "linux-aarch64",  "linux-arm64"     },
    // darwin: NOT bridged before this change. The first row is the reported
    // Intel-Mac failure -- nix-bundle-lgx writes darwin-amd64, lgpm computes
    // darwin-x86_64, and install refuses.
    { "darwin-x86_64",  "darwin-amd64"    },
    { "darwin-amd64",   "darwin-x86_64"   },
    { "darwin-arm64",   "darwin-aarch64"  },
    { "darwin-aarch64", "darwin-arm64"    },
    // windows: NOT bridged before this change, and its producer spelling is
    // the odd one out (windows-x86_64 where the same producer writes
    // linux-amd64), which is exactly the per-OS exception that caused this.
    { "windows-x86_64", "windows-amd64"   },
    { "windows-amd64",  "windows-x86_64"  },
    { "windows-arm64",  "windows-aarch64" },
    { "windows-aarch64","windows-arm64"   },
};
} // namespace

TEST(VariantTest, EveryTargetAcceptsBothArchSpellings) {
    for (const auto& c : kAliasCases) {
        ScopedPlatformOverride guard(c.host);
        auto variants = PackageManagerLib::platformVariantsToTry();
        EXPECT_TRUE(accepts(variants, c.host))
            << "host " << c.host << " does not accept its own spelling; got " << join(variants);
        EXPECT_TRUE(accepts(variants, c.alsoAccepted))
            << "host " << c.host << " does not accept producer spelling "
            << c.alsoAccepted << "; got " << join(variants);
    }
}

// =============================================================================
// Fail-closed guarantees. Aliasing must WIDEN within one target and never
// across targets: a Windows package on a Mac, or an arm64 package on an x86_64
// host, must still be refused.
// =============================================================================

TEST(VariantTest, NeverAcceptsAnotherOperatingSystem) {
    const char* const hosts[] = { "darwin-x86_64", "darwin-arm64", "linux-x86_64",
                                  "linux-arm64", "windows-x86_64" };
    const char* const foreign[] = { "darwin-x86_64", "darwin-amd64", "darwin-arm64",
                                    "darwin-aarch64", "linux-x86_64", "linux-amd64",
                                    "linux-arm64", "linux-aarch64", "windows-x86_64",
                                    "windows-amd64", "windows-arm64", "windows-aarch64" };
    for (const char* host : hosts) {
        ScopedPlatformOverride guard(host);
        auto variants = PackageManagerLib::platformVariantsToTry();
        const std::string hostOs = std::string(host).substr(0, std::string(host).rfind('-'));
        for (const char* f : foreign) {
            const std::string fOs = std::string(f).substr(0, std::string(f).rfind('-'));
            if (fOs == hostOs) continue;
            EXPECT_FALSE(accepts(variants, f))
                << "host " << host << " accepted foreign-OS variant " << f
                << "; got " << join(variants);
        }
    }
}

TEST(VariantTest, NeverAcceptsAnotherArchitecture) {
    struct { const char* host; const char* foreignArch; } cases[] = {
        { "darwin-x86_64",  "darwin-arm64"    },
        { "darwin-x86_64",  "darwin-aarch64"  },
        { "darwin-arm64",   "darwin-x86_64"   },
        { "darwin-arm64",   "darwin-amd64"    },
        { "linux-x86_64",   "linux-arm64"     },
        { "linux-arm64",    "linux-amd64"     },
        { "windows-x86_64", "windows-arm64"   },
    };
    for (const auto& c : cases) {
        ScopedPlatformOverride guard(c.host);
        auto variants = PackageManagerLib::platformVariantsToTry();
        EXPECT_FALSE(accepts(variants, c.foreignArch))
            << "host " << c.host << " accepted foreign-arch variant " << c.foreignArch
            << "; got " << join(variants);
    }
}

TEST(VariantTest, UnknownArchitectureGetsNoAliasesAndStillNamesItself) {
    // A spelling no table row knows must degrade to "itself only" rather than
    // throwing away the host's own variant.
    ScopedPlatformOverride guard("linux-riscv64");
    auto variants = PackageManagerLib::platformVariantsToTry();
    EXPECT_TRUE(accepts(variants, "linux-riscv64")) << join(variants);
    EXPECT_EQ(variants.size(), 1u) << join(variants);
}

TEST(VariantTest, OverrideIsRestoredAfterScopedUse) {
    const std::string before = PackageManagerLib::currentPlatformVariant();
    {
        ScopedPlatformOverride guard("windows-arm64");
        EXPECT_EQ(PackageManagerLib::currentPlatformVariant(), "windows-arm64");
    }
    EXPECT_EQ(PackageManagerLib::currentPlatformVariant(), before);
}

// =============================================================================
// End-to-end: a real .lgx, built with a producer's spelling, installed by a
// host that computes the other spelling.
//
// The unit assertions above pin the accept LIST; these pin what the list is
// for. They are also the backward-compatibility proof: every variant name
// exercised here is one that already exists in published packages
// (darwin-arm64, linux-amd64, linux-arm64) or that the standard bundler emits
// on a target it has never been able to ship (darwin-amd64).
// =============================================================================

class VariantInstallTest : public ::testing::Test {
protected:
    fs::path tempDir;
    fs::path modulesDir;
    fs::path uiPluginsDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path()
                / ("lgpm_variant_test_" + std::to_string(::testing::UnitTest::GetInstance()
                                                             ->random_seed())
                   + "_" + std::to_string(std::rand()));
        modulesDir = tempDir / "modules";
        uiPluginsDir = tempDir / "ui_plugins";
        fs::create_directories(modulesDir);
        fs::create_directories(uiPluginsDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    /**
     * Build a real .lgx whose only variant is the one the given PRODUCER host
     * would name for itself -- i.e. platformVariantsToTry().front() evaluated
     * under that host, which carries the build's own -dev/portable suffix.
     */
    fs::path createPackageAsProducedBy(const std::string& name,
                                       const std::string& producerHost) {
        std::string variant;
        {
            ScopedPlatformOverride guard(producerHost);
            auto v = PackageManagerLib::platformVariantsToTry();
            if (v.empty()) return {};
            variant = v.front();
        }

        fs::path lgxPath = tempDir / (name + ".lgx");
        fs::path contentDir = tempDir / (name + "_content");
        fs::create_directories(contentDir);

#if defined(__APPLE__)
        std::string libName = name + "_plugin.dylib";
#elif defined(_WIN32)
        std::string libName = name + "_plugin.dll";
#else
        std::string libName = name + "_plugin.so";
#endif
        { std::ofstream f(contentDir / libName); f << "fake library content"; }
        {
            std::ofstream mf(contentDir / "manifest.json");
            mf << "{\n  \"name\": \"" << name << "\",\n"
               << "  \"version\": \"1.0.0\",\n"
               << "  \"type\": \"core\",\n"
               << "  \"category\": \"test\"\n}";
        }

        lgx_result_t res = lgx_create(lgxPath.string().c_str(), name.c_str());
        if (!res.success) return {};
        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        res = lgx_add_variant(pkg, variant.c_str(), contentDir.string().c_str(),
                              libName.c_str());
        if (!res.success) { lgx_free_package(pkg); return {}; }
        res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);
        if (!res.success) return {};
        return lgxPath;
    }

    PackageManagerLib createPM() {
        PackageManagerLib pm;
        pm.setUserModulesDirectory(modulesDir.string());
        pm.setUserUiPluginsDirectory(uiPluginsDir.string());
        pm.setSignaturePolicy(SignaturePolicy::WARN);
        return pm;
    }
};

TEST_F(VariantInstallTest, IntelMacInstallsAPackageTheStandardBundlerProduced) {
    // nix-bundle-lgx names an x86_64-darwin build "darwin-amd64"; an Intel Mac
    // computes "darwin-x86_64". This is the reported failure, end to end.
    auto lgxPath = createPackageAsProducedBy("bundler_pkg", "darwin-amd64");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("darwin-x86_64");
    auto pm = createPM();
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
    ASSERT_FALSE(result.empty()) << errorMsg;
    // installPluginFile returns the directory it installed INTO; the package's
    // own manifest decides whether that is the module or the ui-plugin dir.
    EXPECT_TRUE(fs::exists(fs::path(result) / "bundler_pkg")) << result;
}

TEST_F(VariantInstallTest, PublishedSpellingsStillInstall) {
    // Every variant name live in the registry today, each installed by the
    // host that computes the OTHER spelling for the same target. Nothing here
    // may regress: 15 published packages / 146 manifest rows use these names.
    struct { const char* pkg; const char* producer; const char* host; } cases[] = {
        { "pub_darwin_arm64", "darwin-arm64", "darwin-aarch64" },
        { "pub_linux_amd64",  "linux-amd64",  "linux-x86_64"   },
        { "pub_linux_arm64",  "linux-arm64",  "linux-aarch64"  },
    };
    for (const auto& c : cases) {
        auto lgxPath = createPackageAsProducedBy(c.pkg, c.producer);
        ASSERT_FALSE(lgxPath.empty()) << c.pkg;

        ScopedPlatformOverride host(c.host);
        auto pm = createPM();
        std::string errorMsg;
        std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
        EXPECT_FALSE(result.empty())
            << c.producer << " package refused by host " << c.host << ": " << errorMsg;
        if (result.empty()) continue;   // report every row, don't stop at the first
        EXPECT_TRUE(fs::exists(fs::path(result) / c.pkg)) << result;
    }
}

TEST_F(VariantInstallTest, SameSpellingStillInstalls) {
    // The null-change control: producer and host agree, which worked before
    // this change and must keep working.
    struct { const char* pkg; const char* both; } cases[] = {
        { "same_darwin_arm64", "darwin-arm64"  },
        { "same_linux_amd64",  "linux-amd64"   },
        { "same_darwin_x8664", "darwin-x86_64" },
    };
    for (const auto& c : cases) {
        auto lgxPath = createPackageAsProducedBy(c.pkg, c.both);
        ASSERT_FALSE(lgxPath.empty()) << c.pkg;

        ScopedPlatformOverride host(c.both);
        auto pm = createPM();
        std::string errorMsg;
        std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
        EXPECT_FALSE(result.empty()) << c.both << ": " << errorMsg;
        if (result.empty()) continue;   // report every row, don't stop at the first
        EXPECT_TRUE(fs::exists(fs::path(result) / c.pkg)) << result;
    }
}

TEST_F(VariantInstallTest, ForeignTargetPackagesAreStillRefused) {
    // Widening the accept set must not have opened a cross-target hole.
    struct { const char* pkg; const char* producer; const char* host; } cases[] = {
        { "foreign_os",   "windows-x86_64", "darwin-x86_64" },
        { "foreign_arch", "darwin-arm64",   "darwin-x86_64" },
        { "foreign_both", "linux-amd64",    "darwin-arm64"  },
    };
    for (const auto& c : cases) {
        auto lgxPath = createPackageAsProducedBy(c.pkg, c.producer);
        ASSERT_FALSE(lgxPath.empty()) << c.pkg;

        ScopedPlatformOverride host(c.host);
        auto pm = createPM();
        std::string errorMsg;
        std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
        EXPECT_TRUE(result.empty())
            << c.producer << " package was installed on host " << c.host;
        EXPECT_NE(errorMsg.find("variant"), std::string::npos) << errorMsg;
        EXPECT_FALSE(fs::exists(modulesDir / c.pkg));
        EXPECT_FALSE(fs::exists(uiPluginsDir / c.pkg));
    }
}

// =============================================================================
// Mobile and web hosts
//
// Same table, three more targets. The store shell is where the "one vocabulary"
// claim gets tested: lgpm computes nothing itself -- currentPlatformVariant()
// is liblgx's answer for the build -- so these rows pin that the accept list a
// phone builds is the one the package format promises, and that widening it
// for mobile did not open a hole between targets.
// =============================================================================

namespace {
const VariantAliasCase kMobileAliasCases[] = {
    { "android-arm64",   "android-aarch64" },
    { "android-x86_64",  "android-amd64"   },
    { "ios-arm64",       "ios-aarch64"     },
    { "ios-sim-arm64",   "ios-sim-aarch64" },
};
} // namespace

TEST(VariantTest, EveryMobileTargetAcceptsBothArchSpellings) {
    for (const auto& c : kMobileAliasCases) {
        ScopedPlatformOverride guard(c.host);
        auto variants = PackageManagerLib::platformVariantsToTry();
        EXPECT_TRUE(accepts(variants, c.host))
            << "host " << c.host << " does not accept its own spelling; got " << join(variants);
        EXPECT_TRUE(accepts(variants, c.alsoAccepted))
            << "host " << c.host << " does not accept producer spelling "
            << c.alsoAccepted << "; got " << join(variants);
    }
}

TEST(VariantTest, AnIosHostSelectsItsOwnVariantAndNothingAdjacent) {
    // The acceptance criterion, as a list: a host reporting ios-arm64 leads
    // with ios-arm64, tolerates the aarch64 spelling of it, and reaches for
    // neither the simulator slice nor the Mac one nor a web payload.
    ScopedPlatformOverride guard("ios-arm64");
    auto variants = PackageManagerLib::platformVariantsToTry();
    ASSERT_FALSE(variants.empty());
    EXPECT_EQ(variants.front().rfind("ios-arm64", 0), 0u) << join(variants);
    EXPECT_TRUE(accepts(variants, "ios-aarch64")) << join(variants);
    for (const char* foreign : { "ios-sim-arm64", "darwin-arm64", "android-arm64", "web" }) {
        EXPECT_FALSE(accepts(variants, foreign))
            << "ios-arm64 host accepted " << foreign << "; got " << join(variants);
    }
}

TEST(VariantTest, NoNativeHostReachesForTheWebVariant) {
    // A web payload needs the Web container. A host without one selecting it
    // would install JavaScript where it loads a plugin.
    for (const char* host : { "darwin-arm64", "linux-x86_64", "windows-x86_64",
                              "android-arm64", "ios-arm64", "ios-sim-arm64" }) {
        ScopedPlatformOverride guard(host);
        auto variants = PackageManagerLib::platformVariantsToTry();
        EXPECT_FALSE(accepts(variants, "web")) << host << " -> " << join(variants);
    }
}

TEST(VariantTest, AndroidIsNotLinuxAndTheSimulatorIsNotTheDevice) {
    struct { const char* host; const char* foreign; } cases[] = {
        { "android-arm64",  "linux-arm64"    },
        { "android-arm64",  "linux-aarch64"  },
        { "linux-arm64",    "android-arm64"  },
        { "ios-arm64",      "ios-sim-arm64"  },
        { "ios-sim-arm64",  "ios-arm64"      },
        { "ios-sim-arm64",  "darwin-arm64"   },
        { "darwin-arm64",   "ios-arm64"      },
    };
    for (const auto& c : cases) {
        ScopedPlatformOverride guard(c.host);
        auto variants = PackageManagerLib::platformVariantsToTry();
        EXPECT_FALSE(accepts(variants, c.foreign))
            << "host " << c.host << " accepted " << c.foreign << "; got " << join(variants);
    }
}

TEST_F(VariantInstallTest, AnIosHostInstallsTheIosPackage) {
    auto lgxPath = createPackageAsProducedBy("ios_pkg", "ios-arm64");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("ios-arm64");
    auto pm = createPM();
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
    ASSERT_FALSE(result.empty()) << errorMsg;
    EXPECT_TRUE(fs::exists(fs::path(result) / "ios_pkg")) << result;
}

TEST_F(VariantInstallTest, AMobileHostRefusesAForeignTargetAndNamesWhatThePackageHas) {
    // The refusal has to be actionable on a device, where nobody can open the
    // .lgx to see what is inside: the message names the variants the package
    // actually provides, not just the one that was wanted.
    struct { const char* pkg; const char* producer; const char* host; } cases[] = {
        { "sim_on_device",  "ios-sim-arm64", "ios-arm64"     },
        { "device_on_sim",  "ios-arm64",     "ios-sim-arm64" },
        { "mac_on_device",  "darwin-arm64",  "ios-arm64"     },
        { "linux_on_droid", "linux-arm64",   "android-arm64" },
        { "web_on_device",  "web",           "ios-arm64"     },
    };
    for (const auto& c : cases) {
        auto lgxPath = createPackageAsProducedBy(c.pkg, c.producer);
        ASSERT_FALSE(lgxPath.empty()) << c.pkg;

        ScopedPlatformOverride host(c.host);
        auto pm = createPM();
        std::string errorMsg;
        std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
        EXPECT_TRUE(result.empty())
            << c.producer << " package was installed on host " << c.host;
        EXPECT_NE(errorMsg.find("variant"), std::string::npos) << errorMsg;
        // The variant the package carries, named in the failure.
        EXPECT_NE(errorMsg.find(c.producer), std::string::npos)
            << "error does not name the variant the package provides: " << errorMsg;
        EXPECT_FALSE(fs::exists(modulesDir / c.pkg));
        EXPECT_FALSE(fs::exists(uiPluginsDir / c.pkg));
    }
}

// =============================================================================
// A Store shell installs the CONTAINER's variant, not the loader's
//
// Everything above is about the variant a host LOADS: one native image, chosen
// by the platform it is running on, and `NoNativeHostReachesForTheWebVariant`
// pins that no host reaches past it on its own. That rule has to stay -- a host
// that silently selected a web payload would install JavaScript where it opens
// a plugin.
//
// A Store shell is the case it does not cover. Its native variant arrived in
// the app image at BUILD time and a phone may not download native code (ADR
// 0003), so the only thing it installs at run time is a `web` variant, into the
// Web container. That is a DECLARATION the embedder makes -- setInstallVariants
// -- and never an inference, which is the same shape the cross-build override
// already has next door.
// =============================================================================

class WebVariantInstallTest : public VariantInstallTest {
protected:
    /**
     * A real .lgx whose only variant is `web`: the directory
     * logos-module-builder emits for a ui_qml module's web variant -- a
     * manifest, an entry document and the files beside it, and NO library at
     * all. The bytes are a stand-in; the layout is the contract.
     */
    fs::path createWebPackage(const std::string& name) {
        fs::path lgxPath = tempDir / (name + ".lgx");
        fs::path contentDir = tempDir / (name + "_web_content");
        fs::create_directories(contentDir / "view");
        { std::ofstream f(contentDir / "index.html");  f << "<!doctype html><title>ui</title>"; }
        { std::ofstream f(contentDir / "host.html");   f << "<!doctype html><title>headless</title>"; }
        { std::ofstream f(contentDir / "view" / "Counter.qml"); f << "import QtQuick\nItem {}\n"; }
        {
            std::ofstream mf(contentDir / "manifest.json");
            mf << "{\n  \"name\": \"" << name << "\",\n"
               << "  \"version\": \"1.0.0\",\n"
               << "  \"type\": \"ui_qml\",\n"
               << "  \"logos_web_runtime\": \"qml\",\n"
               << "  \"main\": \"index.html\",\n"
               << "  \"category\": \"test\"\n}";
        }

        lgx_result_t res = lgx_create(lgxPath.string().c_str(), name.c_str());
        if (!res.success) return {};
        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        res = lgx_add_variant(pkg, "web", contentDir.string().c_str(), "index.html");
        if (!res.success) { lgx_free_package(pkg); return {}; }
        res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);
        if (!res.success) return {};
        return lgxPath;
    }
};

TEST_F(WebVariantInstallTest, AStoreShellInstallsTheWebVariantItDeclared) {
    auto lgxPath = createWebPackage("web_counter");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("ios-sim-arm64");
    auto pm = createPM();
    pm.setInstallVariants({ "web" });

    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
    ASSERT_FALSE(result.empty()) << errorMsg;
    EXPECT_TRUE(fs::exists(modulesDir / "web_counter" / "manifest.json")) << result;
    EXPECT_TRUE(fs::exists(modulesDir / "web_counter" / "index.html")) << result;
    EXPECT_TRUE(fs::exists(modulesDir / "web_counter" / "view" / "Counter.qml")) << result;
}

TEST_F(WebVariantInstallTest, AWebVariantIsTheCoresToDiscoverWhateverItsType) {
    // `type: ui_qml` routes a DESKTOP package to the ui-plugins directory,
    // where a Qt plugin is loaded by the shell. A `web` variant is not that
    // package: the Web container is a CORE container, so the core discovers it
    // in a modules directory or it is never loaded at all. Routing it by type
    // would install it somewhere nothing scans and report success.
    auto lgxPath = createWebPackage("web_counter_typed");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("ios-sim-arm64");
    auto pm = createPM();
    pm.setInstallVariants({ "web" });

    std::string errorMsg;
    bool isCoreModule = false;
    std::string installedPath;
    std::string result =
        pm.installPluginFile(lgxPath.string(), errorMsg, false, &installedPath, &isCoreModule);
    ASSERT_FALSE(result.empty()) << errorMsg;
    EXPECT_TRUE(isCoreModule);
    EXPECT_EQ(result, modulesDir.string());
    EXPECT_FALSE(fs::exists(uiPluginsDir / "web_counter_typed"));
    // ...and what it reports is inside what it installed. The exact leaf is the
    // ROOT manifest's business (see installedDirOf in test_signer_identity.cpp:
    // the C API has no lgx_set_type, so a fixture's root manifest is thinner
    // than a published package's), and this is about WHERE, not which file.
    ASSERT_FALSE(installedPath.empty());
    EXPECT_EQ(installedPath.rfind((modulesDir / "web_counter_typed").string(), 0), 0u)
        << installedPath;
}

TEST_F(WebVariantInstallTest, AnInstalledWebVariantIsFoundByTheScanWithItsEntryPoint) {
    // THE OTHER HALF OF THE INSTALL, and the one that decides whether anything
    // ever runs. installPluginFile puts a `web` variant in the modules
    // directory so the core can DISCOVER it -- and discovery is
    // getInstalledPackages(), which resolves each manifest's `main` before it
    // reports the package at all: a row with no main file is dropped by the
    // caller (liblogos' ModuleRegistry skips `mainFilePath.empty()`).
    //
    // `main` is a per-variant map, and this host's native variant is
    // ios-sim-arm64. Resolved against THAT the web package answers "no main",
    // so the install succeeded, the files landed in the directory the core
    // scans, and the module was invisible -- with no error anywhere. Measured
    // on an iPad simulator, 2026-09-13.
    //
    // The variant that was EXTRACTED here is the one `main` has to resolve
    // against, and installPluginFile already records it in the `variant`
    // sidecar beside the manifest.
    auto lgxPath = createWebPackage("web_scanned");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("ios-sim-arm64");
    auto pm = createPM();
    pm.setInstallVariants({ "web" });

    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;

    // A SECOND instance, with no declaration at all -- which is what the core's
    // own PackageManagerLib is. Nothing tells it the shell installs `web`
    // variants, and it must find the package all the same.
    PackageManagerLib scanner;
    scanner.setUserModulesDirectory(modulesDir.string());
    bool found = false;
    for (const auto& pkg : scanner.getInstalledPackages()) {
        if (pkg.name != "web_scanned") continue;
        found = true;
        EXPECT_EQ(pkg.mainFilePath, (modulesDir / "web_scanned" / "index.html").string());
    }
    EXPECT_TRUE(found) << "the scan did not report the installed web package at all";
}

TEST_F(WebVariantInstallTest, WithoutTheDeclarationAWebPackageIsStillRefused) {
    // The default is unchanged and stays unchanged: a host that did not say it
    // has a container does not get one.
    auto lgxPath = createWebPackage("web_undeclared");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("ios-sim-arm64");
    auto pm = createPM();

    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
    EXPECT_TRUE(result.empty()) << "a web payload installed on a host with no container";
    EXPECT_NE(errorMsg.find("variant"), std::string::npos) << errorMsg;
    EXPECT_FALSE(fs::exists(modulesDir / "web_undeclared"));
    EXPECT_FALSE(fs::exists(uiPluginsDir / "web_undeclared"));
}

TEST_F(WebVariantInstallTest, DeclaringWebDoesNotAdmitANativePackage) {
    // The declaration REPLACES the accept list rather than widening it. A Store
    // shell that installed a darwin-arm64 payload because it happened to be on
    // a Mac would be downloading native code, which is the one thing ADR 0003
    // says it may not do.
    auto lgxPath = createPackageAsProducedBy("native_on_shell", "darwin-arm64");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("darwin-arm64");
    auto pm = createPM();
    pm.setInstallVariants({ "web" });

    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
    EXPECT_TRUE(result.empty()) << "a native payload installed into a web-only shell";
    EXPECT_NE(errorMsg.find("variant"), std::string::npos) << errorMsg;
    EXPECT_FALSE(fs::exists(modulesDir / "native_on_shell"));
}

TEST_F(WebVariantInstallTest, AnEmptyDeclarationRestoresTheHostsOwnVariant) {
    // Symmetrical with setPlatformVariantOverride(""): the declaration is
    // revocable, and revoking it is not the same as declaring nothing is
    // installable.
    auto lgxPath = createPackageAsProducedBy("back_to_native", "darwin-arm64");
    ASSERT_FALSE(lgxPath.empty());

    ScopedPlatformOverride host("darwin-arm64");
    auto pm = createPM();
    pm.setInstallVariants({ "web" });
    pm.setInstallVariants({ });

    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
    ASSERT_FALSE(result.empty()) << errorMsg;
    // installPluginFile returns the root it installed INTO; which of the two
    // that is depends on the fixture's root manifest, not on this test.
    EXPECT_TRUE(fs::exists(fs::path(result) / "back_to_native")) << result;
}
