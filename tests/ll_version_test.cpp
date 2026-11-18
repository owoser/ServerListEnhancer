// Link with <LL_SOURCE>/src/ll/core/mod/ModLoadPlanner.cpp and add <LL_SOURCE>/src,
// src-client, and the resolved LL package include directories. Compile both files
// with /FI"ll/api/Global.h", matching LL's PCH contract.
// Run from the project root, or pass the template / packed client manifest path.
// The version boundary calls the exact production load guard and LL parser.
// The load-planner regression intentionally has NO synthetic LeviLamina native mod:
// ModRegistrar.cpp loadManifest skips the real preload-native loader manifest.
#include "LoaderVersion.h"
#include "ll/core/mod/ModLoadPlanner.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using ll::mod::Manifest;
using ll::mod::ModLoadIssueKind;
using ll::mod::ModLoadPlanner;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string{message});
}

Manifest readClientManifest(char const* manifestPath) {
    std::ifstream input{manifestPath};
    check(input.is_open(), "Cannot open client manifest");
    auto document = nlohmann::json::parse(input);
    check(document.at("platform") == "client", "Client manifest must target client");
    check(document.at("type") == "native", "Client manifest must target native mod manager");
    check(document.at("extraInfo").at("minimumLeviLaminaVersion").get<std::string>() == sle::client::kMinimumLoaderVersion,
          "Descriptive manifest minimum must agree with the production guard");
    // There is no normal LeviLamina native-mod registration to satisfy such a dependency.
    check(!document.contains("dependencies") || document.at("dependencies").empty(),
          "Standalone client must not require the preload-native loader as a regular mod");

    // Only packaging placeholders are substituted here; versions are parsed by real LL.
    Manifest client;
    client.name = document.at("name").get<std::string>();
    if (client.name == "${modName}") client.name = "ServerListEnhancer";
    client.entry = document.at("entry").get<std::string>();
    if (client.entry == "${modFile}") client.entry = "ServerListEnhancer.dll";
    client.type = document.at("type").get<std::string>();
    client.platform = document.at("platform").get<std::string>();
    auto version = document.at("version").get<std::string>();
    if (version == "${modVersion}") version = "1.0.0";
    client.version = ll::data::Version{version};
    return client;
}

void testProductionGuard(std::string_view version, bool allowed) {
    check(sle::client::isSupportedLoaderVersion(ll::data::Version{version}) == allowed,
          "Production LL load guard differs from the required minimum-version boundary");
}

void testActualLoadPlanner(Manifest const& client) {
    ll::DenseMap<std::string, Manifest> manifests;
    manifests.emplace(client.name, client);
    ll::DenseSet<std::string> roots{client.name};
    auto plan = ModLoadPlanner::resolve(manifests, roots, true);
    check(plan.rejected.empty(), "Client rejected without a synthetic normal loader mod");
    check(plan.order.size() == 1 && plan.order.front() == client.name,
          "Actual load planner must admit the standalone client");

    // Reproduce the rejected real-world manifest mistake, rather than inventing a loader
    // registration that hides it: the planner must report MissingDependency in this case.
    auto brokenClient = client;
    brokenClient.dependencies.emplace().emplace(
        ll::mod::Dependency{"LeviLamina", ll::data::VersionRequirement{
            ">=" + std::string{sle::client::kMinimumLoaderVersion}}}
    );
    manifests.at(client.name) = std::move(brokenClient);
    auto brokenPlan = ModLoadPlanner::resolve(manifests, roots, true);
    check(brokenPlan.rejected.contains(client.name), "Broken required-loader dependency must be rejected");
    auto const& issue = brokenPlan.rejected.at(client.name);
    check(issue.kind == ModLoadIssueKind::MissingDependency && issue.related == "LeviLamina",
          "Required-loader regression must fail with the real MissingDependency issue");
}

} // namespace

int main(int argc, char** argv) try {
    auto client = readClientManifest(argc > 1 ? argv[1] : "manifest-client.json");
    testProductionGuard("26.60.7", false);
    testProductionGuard("26.60.8", true);
    testProductionGuard("26.60.9", true);
    testProductionGuard("27.0.0", true); // Minimum floor, no upper bound.
    testProductionGuard("26.60.8-beta", false); // LL stable-range semantics.
    testActualLoadPlanner(client);
    std::cout << "Production LL guard passed: 26.60.7 rejected, 26.60.8/26.60.9 allowed; "
                 "actual load planner accepts client without a synthetic loader dependency\n";
    return 0;
} catch (std::exception const& error) {
    std::cerr << "LL minimum-version test failed: " << error.what() << '\n';
    return 1;
}
