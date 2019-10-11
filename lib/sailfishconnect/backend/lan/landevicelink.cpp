/*
 * Copyright 2013 Albert Vaca <albertvaka@gmail.com>
 * Copyright 2019 Richard Liebscher <richard.liebscher@gmail.com>
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

#include "landevicelink.h"

#include <QTimer>

#include "../../kdeconnectconfig.h"
#include "../linkprovider.h"
#include "socketlinereader.h"
#include "lanlinkprovider.h"
#include "../../corelogging.h"
#include "lanuploadjob.h"
#include <sailfishconnect/device.h>
#include <sailfishconnect/helper/cpphelper.h>
#include <KJobTrackerInterface>

using namespace SailfishConnect;

LanDeviceLink::LanDeviceLink(const QString& deviceId, LanLinkProvider *parent, QSslSocket* socket, ConnectionStarted connectionSource)
    : DeviceLink(deviceId, parent)
    , m_socketLineReader(nullptr)
    , m_debounceTimer(new QTimer(this))
{
    reset(socket, connectionSource);

    m_debounceTimer->setInterval(100);
    m_debounceTimer->setSingleShot(true);
    connect(m_debounceTimer, &QTimer::timeout,
            this, &LanDeviceLink::socketDisconnected);
}

LanDeviceLink::~LanDeviceLink() = default;

void LanDeviceLink::reset(QSslSocket* socket, ConnectionStarted connectionSource)
{
    Q_ASSERT(socket->state() != QAbstractSocket::UnconnectedState);
    qCDebug(coreLogger) << "reseting device link";

    m_socketLineReader.reset(new SocketLineReader(socket, this));

    connect(socket, &QAbstractSocket::disconnected,
            m_debounceTimer, Overload<>::of(&QTimer::start));
    connect(m_socketLineReader.data(), &SocketLineReader::readyRead,
            this, &LanDeviceLink::dataReceived);

    //We take ownership of the socket.
    //When the link provider destroys us,
    //the socket (and the reader) will be
    //destroyed as well
    socket->setParent(m_socketLineReader.data());

    QString certString = config()->getDeviceProperty(deviceId(), QStringLiteral("certificate"));
    DeviceLink::setPairStatus(certString.isEmpty()? PairStatus::NotPaired : PairStatus::Paired);
}

QHostAddress LanDeviceLink::hostAddress() const
{
    if (!m_socketLineReader) {
        return QHostAddress::Null;
    }
    QHostAddress addr = m_socketLineReader->m_socket->peerAddress();
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        bool success;
        QHostAddress convertedAddr = QHostAddress(addr.toIPv4Address(&success));
        if (success) {
            qCDebug(coreLogger) << "Converting IPv6" << addr << "to IPv4" << convertedAddr;
            addr = convertedAddr;
        }
    }
    return addr;
}

QString LanDeviceLink::name()
{
    return QStringLiteral("LanLink"); // Should be same in both android and kde version
}

bool LanDeviceLink::sendPacket(NetworkPacket& np, KJobTrackerInterface* jobMgr)
{
    if (np.hasPayload()) {
        auto* uploadJob = sendPayload(np, jobMgr);
        if (uploadJob->isOkay())
            np.setPayloadTransferInfo(uploadJob->transferInfo());
    }

    int written = m_socketLineReader->write(np.serialize());

    //Actually we can't detect if a packet is received or not. We keep TCP
    //"ESTABLISHED" connections that look legit (return true when we use them),
    //but that are actually broken (until keepalive detects that they are down).
    return (written != -1);
}

LanUploadJob* LanDeviceLink::sendPayload(const NetworkPacket& np, KJobTrackerInterface* jobMgr)
{
    LanUploadJob* job = new LanUploadJob(np, deviceId(), provider(), this);
    job->start();
    if (jobMgr) {
        jobMgr->registerJob(job);
    }

    return job;
}

void LanDeviceLink::dataReceived()
{
    if (m_socketLineReader->bytesAvailable() == 0) return;

    const QByteArray serializedPacket = m_socketLineReader->readLine();
    NetworkPacket packet;
    NetworkPacket::unserialize(serializedPacket, &packet);

    qCDebug(coreLogger).noquote()
            << "LanDeviceLink dataReceived" << serializedPacket;

    if (packet.type() == PACKET_TYPE_PAIR) {
        //TODO: Handle pair/unpair requests and forward them (to the pairing handler?)
        provider()->incomingPairPacket(this, packet);
        return;
    }

    if (packet.hasPayloadTransferInfo()) {
        if (packet.payloadSize() < -1) {
            qCWarning(coreLogger)
                    << "Ignore packet because of invalid payload size";
            return;
        }

        //qCDebug(coreLogger) << "HasPayloadTransferInfo";
        const QVariantMap transferInfo = packet.payloadTransferInfo();

        QSharedPointer<QSslSocket> socket(new QSslSocket());

        provider()->configureSslSocket(socket.data(), deviceId(), true);

#if QT_VERSION < QT_VERSION_CHECK(5, 9, 2)
        // emit readChannelFinished when the socket gets disconnected. This seems to be a bug in upstream QSslSocket.
        // Needs investigation and upstreaming of the fix. QTBUG-62257
        connect(socket.data(), &QAbstractSocket::disconnected, socket.data(), &QAbstractSocket::readChannelFinished);
#endif

        const QString address = m_socketLineReader->peerAddress().toString();
        const quint16 port = transferInfo[QStringLiteral("port")].toInt();
        socket->connectToHostEncrypted(address, port, QIODevice::ReadWrite);
        packet.setPayload(socket, packet.payloadSize());
    }

    Q_EMIT receivedPacket(packet);

    if (m_socketLineReader->bytesAvailable() > 0) {
        QMetaObject::invokeMethod(this, "dataReceived", Qt::QueuedConnection);
    }
}

void LanDeviceLink::socketDisconnected()
{
    // Maybe LanDeviceLink::reset was called
    qCDebug(coreLogger) << QObject::sender() << "has disconnected";
    if (m_socketLineReader->m_socket->state()
            == QAbstractSocket::UnconnectedState) {
        delete this;
    }
}

LanLinkProvider *LanDeviceLink::provider() {
    return static_cast<LanLinkProvider*>(DeviceLink::provider());
}

KdeConnectConfig* LanDeviceLink::config() {
    return provider()->config();
}

void LanDeviceLink::userRequestsPair()
{
    if (m_socketLineReader->peerCertificate().isNull()) {
        Q_EMIT pairingError(tr("This device cannot be paired because it is running an old version of KDE Connect."));
    } else {
        provider()->userRequestsPair(deviceId());
    }
}

void LanDeviceLink::userRequestsUnpair()
{
    provider()->userRequestsUnpair(deviceId());
}

void LanDeviceLink::setPairStatus(PairStatus status)
{
    if (status == Paired) {
        QSslCertificate cert = m_socketLineReader->peerCertificate();
        if (cert.isNull()) {
            Q_EMIT pairingError(tr("This device cannot be paired because it is "
                                   "running an old version of KDE Connect."));
            return;
        }

        // check for common name that is used for peer verify
        // it is the unsanitized device id which is used at restart to set the
        // right peer verify name
        QStringList commonName = cert.issuerInfo(QSslCertificate::CommonName);
        if (commonName.length() != 1
                || Device::sanitizeDeviceId(commonName.first()) != deviceId()) {
            Q_EMIT pairingError(tr("This device cannot be paired because it "
                                   "sends a strange ssl certificate."));
            return;
        }
    }

    DeviceLink::setPairStatus(status);
    if (status == Paired) {
        Q_ASSERT(config()->trustedDevices().contains(deviceId()));
        Q_ASSERT(!m_socketLineReader->peerCertificate().isNull());
        config()->setDeviceProperty(
                    deviceId(), QStringLiteral("certificate"), m_socketLineReader->peerCertificate().toPem());
    }
}

bool LanDeviceLink::linkShouldBeKeptAlive() {

    return true;     //FIXME: Current implementation is broken, so for now we will keep links always established

    //We keep the remotely initiated connections, since the remotes require them if they want to request
    //pairing to us, or connections that are already paired. TODO: Keep connections in the process of pairing
    //return (mConnectionSource == ConnectionStarted::Remotely || pairStatus() == Paired);

}
