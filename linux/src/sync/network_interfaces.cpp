#include "network_interfaces.h"

#include <algorithm>

namespace SyncNetwork {

bool isPrivateIPv4(const QHostAddress &address) {
    if (address.protocol() != QAbstractSocket::IPv4Protocol) return false;
    const quint32 ip = address.toIPv4Address();
    return (ip >> 24) == 10                       // 10.0.0.0/8
        || (ip >> 20) == ((172u << 4) | 1)        // 172.16.0.0/12
        || (ip >> 16) == ((192u << 8) | 168);     // 192.168.0.0/16
}

bool looksVirtualOrVpn(const QString &name) {
    static const char *markers[] = {
        "vethernet", "hyper-v", "vmware", "virtualbox", "vbox", "docker", "wsl", "vpn", "tap", "tun",
        "wireguard", "wg", "tailscale", "zerotier", "veth", "virbr", "br-", "utun", "loopback", "npcap",
    };
    const QString lower = name.toLower();
    for (const char *marker : markers) {
        const QString needle = QLatin1String(marker);
        // Short markers ("wg", "tap", "tun") must start the name or a word.
        if (needle.size() <= 3) {
            if (lower.startsWith(needle) || lower.contains(QLatin1Char(' ') + needle)) return true;
        } else if (lower.contains(needle)) {
            return true;
        }
    }
    return false;
}

QVector<Interface> eligibleInterfaces() {
    QVector<Interface> result;
    for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
        const auto flags = networkInterface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || !(flags & QNetworkInterface::CanMulticast) || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        if (networkInterface.type() == QNetworkInterface::Virtual
            || looksVirtualOrVpn(networkInterface.name())
            || looksVirtualOrVpn(networkInterface.humanReadableName())) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
            if (!isPrivateIPv4(entry.ip())) continue;
            Interface candidate;
            candidate.networkInterface = networkInterface;
            candidate.address = entry.ip();
            candidate.netmask = entry.netmask();
            candidate.wired = networkInterface.type() == QNetworkInterface::Ethernet;
            result.append(candidate);
        }
    }
    return result;
}

bool onSameSubnet(const QHostAddress &address, const QVector<Interface> &interfaces) {
    const quint32 ip = address.toIPv4Address();
    for (const Interface &candidate : interfaces) {
        const quint32 mask = candidate.netmask.toIPv4Address();
        if (mask && (ip & mask) == (candidate.address.toIPv4Address() & mask)) return true;
    }
    return false;
}

QStringList rankAddresses(const QStringList &addresses, const QVector<Interface> &interfaces) {
    auto score = [&interfaces](const QString &entry) {
        const QHostAddress address(entry.section(QLatin1Char(':'), 0, 0));
        int value = 0;
        for (const Interface &candidate : interfaces) {
            const quint32 mask = candidate.netmask.toIPv4Address();
            if (mask && (address.toIPv4Address() & mask) == (candidate.address.toIPv4Address() & mask))
                value = std::max(value, candidate.wired ? 3 : 2);
        }
        return value;
    };
    QStringList ranked = addresses;
    std::stable_sort(ranked.begin(), ranked.end(),
                     [&score](const QString &a, const QString &b) { return score(a) > score(b); });
    return ranked;
}

} // namespace SyncNetwork
