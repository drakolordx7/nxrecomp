// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/services/: real host sockets behind bsd and the [net] hosts / dns name map behind sfdnsres ([net] sockets = host).
// network.cpp builds the offline command tables; when sockets = host it lets this module replace the working set of commands.
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/ipc.h"

namespace nxrt::services::nethost {

// [net] sockets = host (default off: every socket call answers "network down", as before).
bool sockets_host();

// Replaces the working set of nn::socket::sf::IClient commands on `s` with Winsock-backed handlers (RegisterClient, StartMonitoring,
// Socket ... DuplicateSocket). Commands it does not touch keep their offline rows.
void install_bsd(ipc::Service& s);

// Resolves `name` through [net] hosts (exact names and "*.suffix" wildcards), dotted IPv4 literals and "localhost", then, when
// [net] dns = host, through the PC's getaddrinfo. `ip` gets the IPv4 address in network byte order (127.0.0.1 = {127, 0, 0, 1}).
// `how` says where the answer came from ("hosts", "literal", "dns"). False: not found.
bool resolve_ipv4(const std::string& name, uint8_t ip[4], const char** how = nullptr);

void reset();

}
