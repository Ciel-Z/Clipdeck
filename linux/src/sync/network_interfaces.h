#pragma once

#include <QHostAddress>
#include <QNetworkInterface>
#include <QString>
#include <QVector>

// Which network adapters LAN sync uses (docs/lan-sync-design.md §5): up,
// multicast-capable, with a private IPv4 address, and not loopback, VPN or a
// virtual adapter (Hyper-V, VMware, VirtualBox, Docker, WSL…).
namespace SyncNetwork {

struct Interface {
    QNetworkInterface networkInterface;
    QHostAddress address;
    QHostAddress netmask;
    bool wired = false;
};

QVector<Interface> eligibleInterfaces();
bool isPrivateIPv4(const QHostAddress &address);
bool looksVirtualOrVpn(const QString &name);
// True when `address` is on the same subnet as one of `interfaces`.
bool onSameSubnet(const QHostAddress &address, const QVector<Interface> &interfaces);
// Preferred first: same subnet as this device, then wired, then the rest.
QStringList rankAddresses(const QStringList &addresses, const QVector<Interface> &interfaces);

} // namespace SyncNetwork
