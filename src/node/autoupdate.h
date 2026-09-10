// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_AUTOUPDATE_H
#define BITCOIN_NODE_AUTOUPDATE_H

#include <util/chaintype.h>
#include <util/fs.h>
#include <util/threadinterrupt.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class ArgsManager;

namespace node {

static constexpr std::string_view DEFAULT_AUTOUPDATE_MANIFEST_URL{""};
static constexpr std::string_view DEFAULT_AUTOUPDATE_TRUSTED_ORIGIN{""};
// QTC has no release channel yet: auto-update is OFF by default on every chain and no
// release key, manifest URL or trusted origin is compiled in. To enable it, an operator
// must pass -autoupdate=1 together with -autoupdatemanifesturl, -autoupdatetrustedorigin
// and -autoupdatepubkey (+ -autoupdatepubkeyalgo). Bake a QTC release key here only once a
// signing ceremony has produced one; never inherit another chain's key. Even with a baked key,
// auto-update stays off unless -autoupdate=1 is given explicitly (mainnet included).
static constexpr std::string_view DEFAULT_AUTOUPDATE_RELEASE_PUBKEY{""};
// Release-signature scheme for the manifest. On a post-quantum chain the update
// channel (which can ship code to every node) must itself be quantum-safe, so the
// default is the PQ ML-DSA-44 scheme rather than classical secp256k1. Supported
// values: "ml-dsa-44", "slh-dsa-128s", "secp256k1". Operators may override the
// baked release key/scheme with -autoupdatepubkey / -autoupdatepubkeyalgo.
static constexpr std::string_view DEFAULT_AUTOUPDATE_RELEASE_PUBKEY_ALGO{"ml-dsa-44"};
static constexpr int64_t DEFAULT_AUTOUPDATE_INTERVAL_SECONDS{30 * 60};
static constexpr int64_t DEFAULT_AUTOUPDATE_INITIAL_DELAY_SECONDS{30};
static constexpr int64_t DEFAULT_AUTOUPDATE_INITIAL_JITTER_SECONDS{60};
static constexpr int64_t DEFAULT_AUTOUPDATE_RETRY_SECONDS{5 * 60};

struct AutoUpdateUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
};

struct AutoUpdateConfig {
    bool enabled{false};
    bool seamless{true};
    bool dev_origin{false};
    bool telemetry{true};
    bool telemetry_client_id_enabled{false};
    std::string manifest_url{std::string{DEFAULT_AUTOUPDATE_MANIFEST_URL}};
    std::string trusted_origin{std::string{DEFAULT_AUTOUPDATE_TRUSTED_ORIGIN}};
    std::string release_pubkey{std::string{DEFAULT_AUTOUPDATE_RELEASE_PUBKEY}};
    std::string release_pubkey_algo{std::string{DEFAULT_AUTOUPDATE_RELEASE_PUBKEY_ALGO}};
    std::string telemetry_client_id;
    std::string python_command{"python3"};
    int64_t interval_seconds{DEFAULT_AUTOUPDATE_INTERVAL_SECONDS};
    int64_t initial_delay_seconds{DEFAULT_AUTOUPDATE_INITIAL_DELAY_SECONDS};
    int64_t initial_jitter_seconds{DEFAULT_AUTOUPDATE_INITIAL_JITTER_SECONDS};
    int64_t retry_seconds{DEFAULT_AUTOUPDATE_RETRY_SECONDS};
    int64_t daemon_pid{0};
    fs::path datadir;
    // Optional explicit rollout cohort in [0, 100). When unset the cohort is derived stably from the
    // datadir, so each node keeps the same cohort across restarts. Set it to pin a node into an
    // early canary band (e.g. 0) or a late band for testing staged rollouts.
    std::optional<int> rollout_cohort;
};

struct AutoUpdateManifest {
    std::string version;
    std::string script_url;
    std::string sig_url;
    std::string script_sha256;
    // Staged/canary rollout: the signed percentage of the fleet (0-100) eligible to apply THIS
    // release yet. A node applies the update only if its stable cohort falls under this value, so a
    // bad release reaches a fraction of nodes first. Defaults to 100 (full rollout) when the
    // manifest omits it, preserving prior behavior. Because it lives in the signed manifest body it
    // cannot be tampered with to widen a rollout.
    int rollout_percent{100};
};

struct AutoUpdateFetchResult {
    bool ok{false};
    int status{0};
    std::string final_url;
    std::vector<unsigned char> body;
    std::string error;
};

enum class AutoUpdateStatus {
    DISABLED,
    INVALID_CONFIG,
    FETCH_FAILED,
    BAD_MANIFEST,
    UNSIGNED_MANIFEST,
    BAD_SIGNATURE,
    NOT_NEWER,
    DOWNGRADE_REJECTED,
    ROLLOUT_DEFERRED,
    SCRIPT_ORIGIN_REJECTED,
    SCRIPT_HASH_MISSING,
    SCRIPT_FETCH_FAILED,
    SCRIPT_HASH_MISMATCH,
    UPDATE_AVAILABLE,
    LAUNCH_FAILED,
    LAUNCHED,
};

struct AutoUpdateCheckResult {
    AutoUpdateStatus status{AutoUpdateStatus::DISABLED};
    std::string detail;
    std::string remote_version;
    std::string script_url;
};

class AutoUpdateFetcher
{
public:
    virtual ~AutoUpdateFetcher() = default;
    virtual AutoUpdateFetchResult Fetch(std::string_view url, size_t max_bytes) = 0;
};

class AutoUpdateSignatureVerifier
{
public:
    virtual ~AutoUpdateSignatureVerifier() = default;
    virtual bool Verify(std::string_view pubkey_hex,
                        const std::vector<unsigned char>& message,
                        const std::vector<unsigned char>& signature) = 0;
};

class AutoUpdateCommandRunner
{
public:
    virtual ~AutoUpdateCommandRunner() = default;
    virtual bool LaunchInstaller(const AutoUpdateConfig& config,
                                 const AutoUpdateManifest& manifest,
                                 const std::vector<unsigned char>& script) = 0;
};

class AutoUpdateManager
{
public:
    AutoUpdateManager(AutoUpdateConfig config,
                      std::unique_ptr<AutoUpdateFetcher> fetcher,
                      std::unique_ptr<AutoUpdateSignatureVerifier> verifier,
                      std::unique_ptr<AutoUpdateCommandRunner> runner);
    ~AutoUpdateManager();

    AutoUpdateManager(const AutoUpdateManager&) = delete;
    AutoUpdateManager& operator=(const AutoUpdateManager&) = delete;

    void Start();
    void Interrupt();
    void Stop();

private:
    void ThreadLoop();

    AutoUpdateConfig m_config;
    std::unique_ptr<AutoUpdateFetcher> m_fetcher;
    std::unique_ptr<AutoUpdateSignatureVerifier> m_verifier;
    std::unique_ptr<AutoUpdateCommandRunner> m_runner;
    std::thread m_thread;
    std::unique_ptr<CThreadInterrupt> m_interrupt;
};

std::optional<AutoUpdateUrl> ParseAutoUpdateUrl(std::string_view url);
bool AutoUpdateUrlMatchesTrustedOrigin(std::string_view url, std::string_view trusted_origin, bool dev_origin);
int CompareAutoUpdateVersion(std::string_view remote_version);
// Three-way compare of two "major.minor.build" version strings (-1/0/1); 0 if either is unparsable.
int CompareAutoUpdateVersions(std::string_view lhs, std::string_view rhs);
// Downgrade/replay protection: the highest release version this node has launched a verified
// installer for, persisted at <datadir>/autoupdate/last_applied_version. Manifests whose version
// is not newer than this are refused even if validly signed.
std::optional<std::string> ReadLastAppliedAutoUpdateVersion(const fs::path& datadir);
bool WriteLastAppliedAutoUpdateVersion(const fs::path& datadir, std::string_view version);
std::string AutoUpdateStatusString(AutoUpdateStatus status);
std::string AutoUpdateTelemetryQuery(const AutoUpdateConfig& config);
std::string AutoUpdateTrackedUrl(std::string_view url, const AutoUpdateConfig& config);

// This node's stable rollout cohort in [0, 100): config.rollout_cohort if set, else derived from a
// hash of the datadir so it is stable across restarts but varies across the fleet. An update with
// manifest rollout_percent P is applied only when cohort < P.
int AutoUpdateRolloutCohort(const AutoUpdateConfig& config);

AutoUpdateCheckResult CheckForAutoUpdate(const AutoUpdateConfig& config,
                                         AutoUpdateFetcher& fetcher,
                                         AutoUpdateSignatureVerifier& verifier,
                                         AutoUpdateCommandRunner& runner);

// Build a verifier for the named release-signature scheme ("ml-dsa-44",
// "slh-dsa-128s", or "secp256k1"). Returns nullptr for an unknown scheme.
std::unique_ptr<AutoUpdateSignatureVerifier> MakeAutoUpdateSignatureVerifier(
    std::string_view algo = DEFAULT_AUTOUPDATE_RELEASE_PUBKEY_ALGO);

// Expected hex-string length of a release public key for the given scheme, or
// std::nullopt if the scheme is unknown. Used for early -autoupdatepubkey validation.
std::optional<size_t> AutoUpdateReleasePubkeyHexLength(std::string_view algo);
std::unique_ptr<AutoUpdateManager> MakeAutoUpdateManager(const ArgsManager& args, ChainType chain);

} // namespace node

#endif // BITCOIN_NODE_AUTOUPDATE_H
