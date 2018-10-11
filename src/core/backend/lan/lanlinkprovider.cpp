/**
 * Copyright 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lanlinkprovider.h"

#include <algorithm>

#include <QHostInfo>
#include <QTcpServer>
#include <QNetworkProxy>
#include <QUdpSocket>
#include <QNetworkSession>
#include <QNetworkConfigurationManager>
#include <QSslCipher>
#include <QSslConfiguration>

#include "../../corelogging.h"
#include "../../kdeconnectconfig.h"
#include "../../daemon.h"
#include "landevicelink.h"
#include "lanpairinghandler.h"

#define MIN_VERSION_WITH_SSL_SUPPORT 6

using namespace SailfishConnect;

LanLinkProvider::LanLinkProvider(bool testMode)
    : m_testMode(testMode)
{
    m_tcpPort = 0;

    // increase this if waiting a single event-loop iteration is not enough
    m_combineBroadcastsTimer.setInterval(0);
    m_combineBroadcastsTimer.setSingleShot(true);
    connect(&m_combineBroadcastsTimer, &QTimer::timeout,
            this, &LanLinkProvider::broadcastToNetwork);

    connect(&m_udpSocket, &QIODevice::readyRead,
            this, &LanLinkProvider::newUdpConnection);

    m_server = new Server(this);    
    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection,
            this, &LanLinkProvider::newConnection);

    m_udpSocket.setProxy(QNetworkProxy::NoProxy);

    // Detect when a network interface changes status, so we announce ourelves
    // in the new network
    connect(
        &m_networkManager, &QNetworkConfigurationManager::configurationChanged,
        this, &LanLinkProvider::onNetworkConfigurationChanged);
}

void LanLinkProvider::onNetworkConfigurationChanged(const QNetworkConfiguration& config)
{
    if (m_lastConfig != config && config.state() == QNetworkConfiguration::Active) {
        m_lastConfig = config;
        qCInfo(coreLogger)
                << "Network configuration changed"
                << "QNetworkConfiguration("
                << "name:" << config.name()
                << "state:" << config.state()
                << "id:" << config.identifier()
                << "type:" << config.type()
                << "bearerType:" << config.bearerType()
                << "bearerTypeFamily:" << config.bearerTypeFamily()
                << "bearerTypeName:" << config.bearerTypeName()
                << "purpose:" << config.purpose()
                << ")";

        qCDebug(coreLogger)
                << "old network config was"
                << "QNetworkConfiguration("
                << "name:" << config.name()
                << "state:" << config.state()
                << "id:" << config.identifier()
                << "type:" << config.type()
                << "bearerType:" << config.bearerType()
                << "bearerTypeFamily:" << config.bearerTypeFamily()
                << "bearerTypeName:" << config.bearerTypeName()
                << "purpose:" << config.purpose()
                << ")";

        onNetworkChange("Network configuration changed");
    }
}

LanLinkProvider::~LanLinkProvider()
{
}

void LanLinkProvider::onStart()
{
    const QHostAddress bindAddress = m_testMode? QHostAddress::LocalHost : QHostAddress::Any;

    // TODO: only bind to WLAN, Ethernet and Bluetooth networks
    bool success = m_udpSocket.bind(bindAddress, UDP_PORT, QUdpSocket::ShareAddress);
    Q_ASSERT(success);

    qCDebug(coreLogger) << "onStart";

    m_tcpPort = MIN_TCP_PORT;
    while (!m_server->listen(bindAddress, m_tcpPort)) {
        m_tcpPort++;
        if (m_tcpPort > MAX_TCP_PORT) { //No ports available?
            qCritical(coreLogger) << "Error opening a port in range" << MIN_TCP_PORT << "-" << MAX_TCP_PORT;
            m_tcpPort = 0;
            return;
        }
    }

    onNetworkChange("starting up");
}

void LanLinkProvider::onStop()
{
    qCDebug(coreLogger) << "onStop";
    m_udpSocket.close();
    m_server->close();
}

void LanLinkProvider::onNetworkChange(const QString& reason)
{
    qCInfo(coreLogger) << "Trying to send broadcast:" << reason;
    if (m_combineBroadcastsTimer.isActive()) {
        qCDebug(coreLogger()) << "Preventing duplicate broadcasts";
        return;
    }
    m_combineBroadcastsTimer.start();
}

//I'm in a new network, let's be polite and introduce myself
void LanLinkProvider::broadcastToNetwork()
{
    if (!m_server->isListening()) {
        // Not started
        return;
    }

    Q_ASSERT(m_tcpPort != 0);

    qCDebug(coreLogger()) << "Broadcasting identity packet";

    NetworkPackage np(QLatin1String(""));
    NetworkPackage::createIdentityPackage(&np);
    np.set(QStringLiteral("tcpPort"), m_tcpPort);

    if (m_testMode) {
        m_udpSocket.writeDatagram(np.serialize(), QHostAddress::LocalHost, UDP_PORT);
        return;
    }

    auto interfaces = LanLinkProvider::getUsefulNetworkInterfaces();
    if (!interfaces.empty()) {
        // TODO: support IPv6 with multicast FF02::1
        m_udpSocket.writeDatagram(
                    np.serialize(), QHostAddress::Broadcast, UDP_PORT);
    }
}

void LanLinkProvider::error(QAbstractSocket::SocketError error)
{
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());
    qCDebug(coreLogger) << socket << "TCP Error" << socket->errorString();
}

QList<QNetworkInterface> LanLinkProvider::getUsefulNetworkInterfaces() {
    auto configs = m_networkManager.allConfigurations(
                QNetworkConfiguration::Active);

    // only use Ethernet, WLAN or Bluetooth network
    configs.erase(std::remove_if(configs.begin(), configs.end(), [](const QNetworkConfiguration& config) {
        auto bearerTypeFamily = config.bearerTypeFamily();
        return bearerTypeFamily != QNetworkConfiguration::BearerEthernet
            && bearerTypeFamily != QNetworkConfiguration::BearerWLAN
            && bearerTypeFamily != QNetworkConfiguration::BearerBluetooth
        ;
    }), configs.end());

    QList<QNetworkInterface> result;
    for (auto& config : configs) {
        result.push_back(QNetworkSession(config).interface());
    }
    return result;
}

//I'm the existing device, a new device is kindly introducing itself.
//I will create a TcpSocket and try to connect. This can result in either connected() or connectError().
void LanLinkProvider::newUdpConnection() //udpBroadcastReceived
{
    QByteArray datagram;

    while (m_udpSocket.hasPendingDatagrams()) {
        datagram.resize(m_udpSocket.pendingDatagramSize());
        QHostAddress sender;

        m_udpSocket.readDatagram(datagram.data(), datagram.size(), &sender);

        if (sender.isLoopback() && !m_testMode)
            continue;

        NetworkPackage* receivedPackage = new NetworkPackage(QLatin1String(""));
        bool success = NetworkPackage::unserialize(datagram, receivedPackage);

        auto deviceId = receivedPackage->get<QString>(
                    QStringLiteral("deviceId"));
        qCDebug(coreLogger) << "UDP connection from" << deviceId;
        // qCDebug(coreLogger) << "UDP datagram" << datagram.data();

        if (
                !success
                || receivedPackage->type() != PACKAGE_TYPE_IDENTITY
                || deviceId.isEmpty())
        {
            delete receivedPackage;
            continue;
        }

        if (deviceId == KdeConnectConfig::instance()->deviceId()) {
            qCDebug(coreLogger) << "Ignoring my own broadcast";
            delete receivedPackage;
            continue;
        }

        if (m_links.contains(deviceId)) {
            qCInfo(coreLogger) << "Ignoring broadcast of linked device"
                                << deviceId;
            delete receivedPackage;
            continue;
        }

        int tcpPort = receivedPackage->get<int>(QStringLiteral("tcpPort"));

        qCDebug(coreLogger)
                << "Received UDP identity package from" << sender
                << "asking for a tcp connection on port" << tcpPort;

        QSslSocket* socket = new QSslSocket(this);
        socket->setProxy(QNetworkProxy::NoProxy);
        m_receivedIdentityPackages[socket].np = receivedPackage;
        m_receivedIdentityPackages[socket].sender = sender;
        connect(socket, &QAbstractSocket::connected, this, &LanLinkProvider::connected);
        connect(socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(connectError()));
        socket->connectToHost(sender, tcpPort);
    }
}

void LanLinkProvider::connectError()
{
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());
    if (!socket) return;
    disconnect(socket, &QAbstractSocket::connected, this, &LanLinkProvider::connected);
    disconnect(socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(connectError()));

    qCDebug(coreLogger) << "Fallback (1), try reverse connection (send udp packet)" << socket->errorString();
    NetworkPackage np(QLatin1String(""));
    NetworkPackage::createIdentityPackage(&np);
    np.set(QStringLiteral("tcpPort"), m_tcpPort);
    m_udpSocket.writeDatagram(np.serialize(), m_receivedIdentityPackages[socket].sender, UDP_PORT);

    //The socket we created didn't work, and we didn't manage
    //to create a LanDeviceLink from it, deleting everything.
    delete m_receivedIdentityPackages.take(socket).np;
    socket->deleteLater();
}

//We received a UDP package and answered by connecting to them by TCP. This gets called on a succesful connection.
void LanLinkProvider::connected()
{
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());

    if (!socket) return;
    disconnect(socket, &QAbstractSocket::connected, this, &LanLinkProvider::connected);
    disconnect(socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(connectError()));

    configureSocket(socket);

    // If socket disconnects due to any reason after connection, link on ssl faliure
    connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

    NetworkPackage* receivedPackage = m_receivedIdentityPackages[socket].np;
    const QString& deviceId = receivedPackage->get<QString>(QStringLiteral("deviceId"));
    qCDebug(coreLogger) << "Connected" << socket << socket->isWritable();

    // If network is on ssl, do not believe when they are connected, believe when handshake is completed
    NetworkPackage np2(QLatin1String(""));
    NetworkPackage::createIdentityPackage(&np2);
    socket->write(np2.serialize());
    bool success = socket->waitForBytesWritten();

    if (success) {

        qCDebug(coreLogger) << socket << "TCP connection done (i'm the existing device)";

        // if ssl supported
        if (receivedPackage->get<int>(QStringLiteral("protocolVersion")) >= MIN_VERSION_WITH_SSL_SUPPORT) {

            bool isDeviceTrusted = KdeConnectConfig::instance()->trustedDevices().contains(deviceId);
            configureSslSocket(socket, deviceId, isDeviceTrusted);

            qCDebug(coreLogger) << socket << "Starting server ssl (I'm the client TCP socket)";

            connect(socket, &QSslSocket::encrypted, this, &LanLinkProvider::encrypted);

            if (isDeviceTrusted) {
                connect(socket, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrors(QList<QSslError>)));
            }

            socket->startServerEncryption();

            return; // Return statement prevents from deleting received package, needed in slot "encrypted"
        } else {
            qWarning() << receivedPackage->get<QString>(QStringLiteral("deviceName")) << "uses an old protocol version, this won't work";
            //addLink(deviceId, socket, receivedPackage, LanDeviceLink::Remotely);
        }

    } else {
        //I think this will never happen, but if it happens the deviceLink
        //(or the socket that is now inside it) might not be valid. Delete them.
        qCDebug(coreLogger) << "Fallback (2), try reverse connection (send udp packet)";
        m_udpSocket.writeDatagram(np2.serialize(), m_receivedIdentityPackages[socket].sender, UDP_PORT);
    }

    delete m_receivedIdentityPackages.take(socket).np;
    //We don't delete the socket because now it's owned by the LanDeviceLink
}

void LanLinkProvider::encrypted()
{
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());
    if (!socket) return;

    qCDebug(coreLogger) << "Socket successfully stablished an SSL connection" << socket;

    disconnect(socket, &QSslSocket::encrypted, this, &LanLinkProvider::encrypted);
    disconnect(socket, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrors(QList<QSslError>)));

    Q_ASSERT(socket->mode() != QSslSocket::UnencryptedMode);
    LanDeviceLink::ConnectionStarted connectionOrigin = (socket->mode() == QSslSocket::SslClientMode)? LanDeviceLink::Locally : LanDeviceLink::Remotely;

    NetworkPackage* receivedPackage = m_receivedIdentityPackages[socket].np;
    const QString& deviceId = receivedPackage->get<QString>(QStringLiteral("deviceId"));

    addLink(deviceId, socket, receivedPackage, connectionOrigin);

    // Copied from connected slot, now delete received package
    delete m_receivedIdentityPackages.take(socket).np;
}

void LanLinkProvider::sslErrors(const QList<QSslError>& errors)
{
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());
    if (!socket) return;

    disconnect(socket, &QSslSocket::encrypted, this, &LanLinkProvider::encrypted);
    disconnect(socket, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrors(QList<QSslError>)));

    qCDebug(coreLogger) << "Failing due to " << errors;
    Device* device = Daemon::instance()->getDevice(socket->peerVerifyName());
    if (device) {
        device->unpair();
    }

    delete m_receivedIdentityPackages.take(socket).np;
    // Socket disconnects itself on ssl error and will be deleted by deleteLater slot, no need to delete manually
}

//I'm the new device and this is the answer to my UDP identity package (no data received yet). They are connecting to us through TCP, and they should send an identity.
void LanLinkProvider::newConnection()
{
    qCDebug(coreLogger) << "LanLinkProvider newConnection";

    while (m_server->hasPendingConnections()) {
        QSslSocket* socket = m_server->nextPendingConnection();
        configureSocket(socket);
        //This socket is still managed by us (and child of the QTcpServer), if
        //it disconnects before we manage to pass it to a LanDeviceLink, it's
        //our responsibility to delete it. We do so with this connection.
        connect(socket, &QAbstractSocket::disconnected,
                socket, &QObject::deleteLater);
        connect(socket, &QIODevice::readyRead,
                this, &LanLinkProvider::dataReceived);

    }
}

//I'm the new device and this is the answer to my UDP identity package (data received)
void LanLinkProvider::dataReceived()
{
    QSslSocket* socket = qobject_cast<QSslSocket*>(sender());

    const QByteArray data = socket->readLine();

    qCDebug(coreLogger) << "LanLinkProvider received reply:" << data;

    NetworkPackage* np = new NetworkPackage(QLatin1String(""));
    bool success = NetworkPackage::unserialize(data, np);

    if (!success) {
        delete np;
        return;
    }

    if (np->type() != PACKAGE_TYPE_IDENTITY) {
        qCWarning(coreLogger) << "LanLinkProvider/newConnection: Expected identity, received " << np->type();
        delete np;
        return;
    }

    // Needed in "encrypted" if ssl is used, similar to "connected"
    m_receivedIdentityPackages[socket].np = np;

    const QString& deviceId = np->get<QString>(QStringLiteral("deviceId"));
    qCDebug(coreLogger) << "Handshaking done (i'm the new device)" << deviceId;

    //This socket will now be owned by the LanDeviceLink or we don't want more data to be received, forget about it
    disconnect(socket, &QIODevice::readyRead, this, &LanLinkProvider::dataReceived);

    if (np->get<int>(QStringLiteral("protocolVersion")) >= MIN_VERSION_WITH_SSL_SUPPORT) {

        bool isDeviceTrusted = KdeConnectConfig::instance()->trustedDevices().contains(deviceId);
        configureSslSocket(socket, deviceId, isDeviceTrusted);

        qCDebug(coreLogger) << "Starting client ssl (but I'm the server TCP socket)" << socket;

        connect(socket, &QSslSocket::encrypted, this, &LanLinkProvider::encrypted);

        if (isDeviceTrusted) {
            connect(socket, SIGNAL(sslErrors(QList<QSslError>)),
                    this, SLOT(sslErrors(QList<QSslError>)));

            connect(socket, SIGNAL(error(QAbstractSocket::SocketError)),
                    this, SLOT(error(QAbstractSocket::SocketError)));
        }

        socket->startClientEncryption();

    } else {
        qWarning() << np->get<QString>(QStringLiteral("deviceName"))
                   << "uses an old protocol version, this won't work";
        delete m_receivedIdentityPackages.take(socket).np;
    }
}

void LanLinkProvider::deviceLinkDestroyed(QObject* destroyedDeviceLink)
{
    const QString id = destroyedDeviceLink->property("deviceId").toString();
    qCDebug(coreLogger) << "deviceLinkDestroyed" << id;
    Q_ASSERT(m_links.key(static_cast<LanDeviceLink*>(destroyedDeviceLink)) == id);
    auto linkIterator = m_links.find(id);
    if (linkIterator != m_links.end()) {
        Q_ASSERT(linkIterator.value() == destroyedDeviceLink);
        m_links.erase(linkIterator);
        LanPairingHandler* handler = m_pairingHandlers.take(id);
        if (handler)
            handler->deleteLater();
    }
}

void LanLinkProvider::configureSslSocket(QSslSocket* socket, const QString& deviceId, bool isDeviceTrusted)
{
    // Setting supported ciphers manually
    // Top 3 ciphers are for new Android devices, botton two are for old Android devices
    // FIXME : These cipher suites should be checked whether they are supported or not on device
    QList<QSslCipher> socketCiphers;
    socketCiphers.append(QSslCipher(QStringLiteral("ECDHE-ECDSA-AES256-GCM-SHA384")));
    socketCiphers.append(QSslCipher(QStringLiteral("ECDHE-ECDSA-AES128-GCM-SHA256")));
    socketCiphers.append(QSslCipher(QStringLiteral("ECDHE-RSA-AES128-SHA")));
    socketCiphers.append(QSslCipher(QStringLiteral("RC4-SHA")));
    socketCiphers.append(QSslCipher(QStringLiteral("RC4-MD5")));
    socketCiphers.append(QSslCipher(QStringLiteral("DHE-RSA-AES256-SHA")));

    // Configure for ssl
    QSslConfiguration sslConfig;
    sslConfig.setCiphers(socketCiphers);
    sslConfig.setProtocol(QSsl::TlsV1_0);

    socket->setSslConfiguration(sslConfig);
    socket->setLocalCertificate(KdeConnectConfig::instance()->certificate());
    socket->setPrivateKey(KdeConnectConfig::instance()->privateKeyPath());
    socket->setPeerVerifyName(deviceId);

    if (isDeviceTrusted) {
        QString certString = KdeConnectConfig::instance()->getDeviceProperty(deviceId, QStringLiteral("certificate"), QString());
        socket->addCaCertificate(QSslCertificate(certString.toLatin1()));
        socket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    } else {
        socket->setPeerVerifyMode(QSslSocket::QueryPeer);
    }

    //Usually SSL errors are only bad for trusted devices. Uncomment this section to log errors in any case, for debugging.
    QObject::connect(socket, static_cast<void (QSslSocket::*)(const QList<QSslError>&)>(&QSslSocket::sslErrors), [socket](const QList<QSslError>& errors)
    {
        Q_FOREACH (const QSslError& error, errors) {
            qCDebug(coreLogger) << socket << "SSL Error:" << error.errorString();
        }
    });
}

void LanLinkProvider::configureSocket(QSslSocket* socket) {

    socket->setProxy(QNetworkProxy::NoProxy);

    socket->setSocketOption(QAbstractSocket::KeepAliveOption, QVariant(1));

    #ifdef TCP_KEEPIDLE
        // time to start sending keepalive packets (seconds)
        int maxIdle = 30;
        setsockopt(socket->socketDescriptor(), IPPROTO_TCP, TCP_KEEPIDLE, &maxIdle, sizeof(maxIdle));
    #endif

    #ifdef TCP_KEEPINTVL
        // interval between keepalive packets after the initial period (seconds)
        int interval = 30;
        setsockopt(socket->socketDescriptor(), IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    #endif

    #ifdef TCP_KEEPCNT
        // number of missed keepalive packets before disconnecting
        int count = 2;
        setsockopt(socket->socketDescriptor(), IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    #endif

}

void LanLinkProvider::addLink(const QString& deviceId, QSslSocket* socket, NetworkPackage* receivedPackage, LanDeviceLink::ConnectionStarted connectionOrigin)
{
    // Socket disconnection will now be handled by LanDeviceLink
    disconnect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

    LanDeviceLink* deviceLink;
    //Do we have a link for this device already?
    auto linkIterator = m_links.find(deviceId);
    if (linkIterator != m_links.end()) {
        deviceLink = linkIterator.value();
        deviceLink->reset(socket, connectionOrigin);
    } else {
        deviceLink = new LanDeviceLink(deviceId, this, socket, connectionOrigin);
        connect(deviceLink, &QObject::destroyed, this, &LanLinkProvider::deviceLinkDestroyed);
        m_links[deviceId] = deviceLink;
        if (m_pairingHandlers.contains(deviceId)) {
            //We shouldn't have a pairinghandler if we didn't have a link.
            //Crash if debug, recover if release (by setting the new devicelink to the old pairinghandler)
            m_pairingHandlers[deviceId]->setDeviceLink(deviceLink);
        }
    }
    Q_EMIT onConnectionReceived(*receivedPackage, deviceLink);
}

LanPairingHandler* LanLinkProvider::createPairingHandler(DeviceLink* link)
{
    LanPairingHandler* ph = m_pairingHandlers.value(link->deviceId());
    if (!ph) {
        ph = new LanPairingHandler(link);
        qCDebug(coreLogger) << "creating pairing handler for" << link->deviceId();
        connect (ph, &LanPairingHandler::pairingError, link, &DeviceLink::pairingError);
        m_pairingHandlers[link->deviceId()] = ph;
    }
    return ph;
}

void LanLinkProvider::userRequestsPair(const QString& deviceId)
{
    LanPairingHandler* ph = createPairingHandler(m_links.value(deviceId));
    ph->requestPairing();
}

void LanLinkProvider::userRequestsUnpair(const QString& deviceId)
{
    LanPairingHandler* ph = createPairingHandler(m_links.value(deviceId));
    ph->unpair();
}

void LanLinkProvider::incomingPairPackage(DeviceLink* deviceLink, const NetworkPackage& np)
{
    LanPairingHandler* ph = createPairingHandler(deviceLink);
    ph->packageReceived(np);
}

