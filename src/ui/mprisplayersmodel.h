#ifndef SAILFISHCONNECT_MPRISPLAYERSMODEL_H
#define SAILFISHCONNECT_MPRISPLAYERSMODEL_H

#include <QAbstractListModel>

namespace SailfishConnect {

class MprisRemotePlugin;

class MprisPlayersModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId)

public:
    explicit MprisPlayersModel(QObject *parent = nullptr);

    enum ExtraRoles {
        PlayerNameRole = Qt::UserRole,
        IsPlayingRole,
        CurrentSongRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        AlbumArtUrlRole,
        VolumeRole,
        LengthRole,
        PositionRole,
        PlayAllowedRole,
        PauseAllowedRole,
        GoNextAllowedRole,
        GoPreviousAllowedRole,
        SeekAllowedRole,
        SetVolumeAllowedRole,
    };

    // Basic functionality:
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    QHash<int, QByteArray> roleNames() const override;

    QString deviceId() const { return m_deviceId; }
    void setDeviceId(const QString& value);

private:
    QStringList m_players;
    MprisRemotePlugin* m_plugin = nullptr;
    QString m_deviceId;

    void playerAdded(const QString& name);
    void playerRemoved(const QString& name);
    void playerUpdated();

    void setPlugin(MprisRemotePlugin* plugin);

    void connectPlayer(const QString& name);
};

} // namespace SailfishConnect

#endif // SAILFISHCONNECT_MPRISPLAYERSMODEL_H
