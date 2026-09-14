// Copyright (c) 2026 The QTC developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Launch-network seed infrastructure compiled into the binary: DNS seed names,
// BIP155 fixed seeds (src/chainparamsseeds.h) and the mining-guard peer mesh.

#include <chainparams.h>
#include <netaddress.h>
#include <netbase.h>
#include <node/mining_guard.h>
#include <protocol.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {

// Decode a fixed-seed blob the same way CConnman::ConvertSeeds does.
std::vector<std::string> DecodeFixedSeeds(const std::vector<uint8_t>& blob)
{
    std::vector<std::string> out;
    ParamsStream s{DataStream{blob}, CAddress::V2_NETWORK};
    while (!s.eof()) {
        CService endpoint;
        s >> endpoint;
        out.push_back(endpoint.ToStringAddrPort());
    }
    return out;
}

const std::vector<std::string> kLaunchHosts{"157.230.194.146", "167.99.181.131"};

} // namespace

BOOST_FIXTURE_TEST_SUITE(qtc_seed_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(mainnet_dns_seeds)
{
    const auto params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const std::vector<std::string> expected{"seed.qtc.gold.", "seed.qtc.exchange."};
    BOOST_CHECK_EQUAL_COLLECTIONS(params->DNSSeeds().begin(), params->DNSSeeds().end(),
                                  expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(testnet_dns_seeds)
{
    const auto params = CreateChainParams(*m_node.args, ChainType::TESTNET);
    const std::vector<std::string> expected{"testnet-seed.qtc.gold.", "testnet-seed.qtc.exchange."};
    BOOST_CHECK_EQUAL_COLLECTIONS(params->DNSSeeds().begin(), params->DNSSeeds().end(),
                                  expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(mainnet_fixed_seeds_decode_to_launch_nodes)
{
    const auto params = CreateChainParams(*m_node.args, ChainType::MAIN);
    const std::vector<std::string> expected{"157.230.194.146:19755", "167.99.181.131:19755"};
    const auto decoded = DecodeFixedSeeds(params->FixedSeeds());
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.begin(), decoded.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(testnet_fixed_seeds_decode_to_launch_nodes)
{
    const auto params = CreateChainParams(*m_node.args, ChainType::TESTNET);
    const std::vector<std::string> expected{"157.230.194.146:29755", "167.99.181.131:29755"};
    const auto decoded = DecodeFixedSeeds(params->FixedSeeds());
    BOOST_CHECK_EQUAL_COLLECTIONS(decoded.begin(), decoded.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(no_seeds_on_chains_without_public_network)
{
    for (const auto chain : {ChainType::TESTNET4, ChainType::REGTEST}) {
        const auto params = CreateChainParams(*m_node.args, chain);
        BOOST_CHECK(params->FixedSeeds().empty());
    }
    BOOST_CHECK(CreateChainParams(*m_node.args, ChainType::TESTNET4)->DNSSeeds().empty());
}

BOOST_AUTO_TEST_CASE(default_mining_peer_mesh_is_launch_nodes_without_ports)
{
    // Bare hosts: CConnman::AddNode applies the active chain's default P2P port,
    // so the same mesh is valid on mainnet (19755) and testnet (29755).
    const auto& mesh = node::DefaultMiningPeerMesh();
    BOOST_CHECK_EQUAL_COLLECTIONS(mesh.begin(), mesh.end(), kLaunchHosts.begin(), kLaunchHosts.end());
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET}) {
        const uint16_t port = CreateChainParams(*m_node.args, chain)->GetDefaultPort();
        for (const auto& host : mesh) {
            BOOST_CHECK(host.find(':') == std::string::npos);
            const CService resolved = LookupNumeric(host, port);
            BOOST_CHECK(resolved.IsValid());
            BOOST_CHECK_EQUAL(resolved.GetPort(), port);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
