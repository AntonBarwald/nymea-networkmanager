/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                                                                               *
 * Copyright (C) 2016 Simon Stürz <simon.stuerz@guh.io>                          *
 *                                                                               *
 * This file is part of loopd.                                                   *
 *                                                                               *
 * Loopd can not be copied and/or distributed without the express                *
 * permission of guh GmbH.                                                       *
 *                                                                               *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "networkmanager.h"
#include "networkconnection.h"

#include <QUuid>
#include <QDebug>
#include <QMetaEnum>

/*! Constructs a new \l{NetworkManager} object with the given \a parent. */
NetworkManager::NetworkManager(QObject *parent) :
    QObject(parent),
    m_networkManagerInterface(nullptr),
    m_available(false),
    m_state(NetworkManagerStateUnknown),
    m_connectivityState(NetworkManagerConnectivityStateUnknown),
    m_networkingEnabled(false),
    m_wirelessEnabled(false)
{
    // Check DBus connection
    if (!QDBusConnection::systemBus().isConnected()) {
        qCWarning(dcNetworkManager()) << "System DBus not connected. NetworkManagre not available.";
        return;
    }

    // Create interface
    m_networkManagerInterface = new QDBusInterface(NetworkManagerUtils::networkManagerServiceString(), NetworkManagerUtils::networkManagerPathString(), NetworkManagerUtils::networkManagerServiceString(), QDBusConnection::systemBus(), this);
    if(!m_networkManagerInterface->isValid()) {
        qCWarning(dcNetworkManager()) << "Invalid DBus network manager interface. NetworkManagre not available.";
        return;
    }

    m_available = true;

    // Read properties
    setVersion(m_networkManagerInterface->property("Version").toString());
    setState((NetworkManagerState)m_networkManagerInterface->property("State").toUInt());
    setConnectivityState((NetworkManagerConnectivityState)m_networkManagerInterface->property("Connectivity").toUInt());
    setNetworkingEnabled(m_networkManagerInterface->property("NetworkingEnabled").toBool());
    setWirelessEnabled(m_networkManagerInterface->property("WirelessEnabled").toBool());

    loadDevices();

    // Connect signals
    QDBusConnection::systemBus().connect(NetworkManagerUtils::networkManagerServiceString(), NetworkManagerUtils::networkManagerPathString(), NetworkManagerUtils::networkManagerServiceString(), "StateChanged", this, SLOT(onStateChanged(uint)));
    QDBusConnection::systemBus().connect(NetworkManagerUtils::networkManagerServiceString(), NetworkManagerUtils::networkManagerPathString(), NetworkManagerUtils::networkManagerServiceString(), "DeviceAdded", this, SLOT(onDeviceAdded(QDBusObjectPath)));
    QDBusConnection::systemBus().connect(NetworkManagerUtils::networkManagerServiceString(), NetworkManagerUtils::networkManagerPathString(), NetworkManagerUtils::networkManagerServiceString(), "DeviceRemoved", this, SLOT(onDeviceRemoved(QDBusObjectPath)));
    QDBusConnection::systemBus().connect(NetworkManagerUtils::networkManagerServiceString(), NetworkManagerUtils::networkManagerPathString(), NetworkManagerUtils::networkManagerServiceString(), "PropertiesChanged", this, SLOT(onPropertiesChanged(QVariantMap)));

    // Create settings
    m_networkSettings = new NetworkSettings(this);
}

NetworkManager::~NetworkManager()
{
    qCDebug(dcNetworkManager()) << "Destroy network manager.";
}

/*! Returns true if the network-manager is available on this system. */
bool NetworkManager::available()
{
    return m_available;
}

/*! Returns true if wifi is available on this system. */
bool NetworkManager::wirelessAvailable()
{
    return !m_wirelessNetworkDevices.isEmpty();
}

/*! Returns the list of \l{NetworkDevice}{NetworkDevices} from this \l{NetworkManager}. */
QList<NetworkDevice *> NetworkManager::networkDevices() const
{
    return m_networkDevices.values();
}

QList<WirelessNetworkDevice *> NetworkManager::wirelessNetworkDevices() const
{
    return m_wirelessNetworkDevices.values();
}

/*! Returns the list of \l{WiredNetworkDevice}{WiredNetworkDevices} from this \l{NetworkManager}. */
QList<WiredNetworkDevice *> NetworkManager::wiredNetworkDevices() const
{
    return m_wiredNetworkDevices.values();
}

/*! Returns the \l{NetworkDevice} with the given \a interface from this \l{NetworkManager}. If there is no such \a interface returns Q_NULLPTR. */
NetworkDevice *NetworkManager::getNetworkDevice(const QString &interface)
{
    foreach (NetworkDevice *device, m_networkDevices.values()) {
        if (device->interface() == interface)
            return device;
    }
    return Q_NULLPTR;
}

/*! Returns the version of the running \l{NetworkManager}. */
QString NetworkManager::version() const
{
    return m_version;
}

/*! Returns the state of this \l{NetworkManager}. \sa NetworkManagerState, */
NetworkManager::NetworkManagerState NetworkManager::state() const
{
    return m_state;
}

/*! Returns the human readable string of the current state of this \l{NetworkManager}. \sa NetworkManagerState, */
QString NetworkManager::stateString() const
{
    return networkManagerStateToString(m_state);
}

/*! Returns the current connectivity state of this \l{NetworkManager}. \sa NetworkManagerConnectivityState, */
NetworkManager::NetworkManagerConnectivityState NetworkManager::connectivityState() const
{
    return m_connectivityState;
}

/*! Connect the given \a interface to a wifi network with the given \a ssid and \a password. Returns the \l{NetworkManagerError} to inform about the result. \sa NetworkManagerError, */
NetworkManager::NetworkManagerError NetworkManager::connectWifi(const QString &interface, const QString &ssid, const QString &password, const bool &hidden)
{
    // Check interface
    if (!getNetworkDevice(interface))
        return NetworkManagerErrorNetworkInterfaceNotFound;

    // Get wirelessNetworkDevice
    WirelessNetworkDevice *wirelessNetworkDevice = 0;
    foreach (WirelessNetworkDevice *networkDevice, wirelessNetworkDevices()) {
        if (networkDevice->interface() == interface)
            wirelessNetworkDevice = networkDevice;
    }

    if (!wirelessNetworkDevice)
        return NetworkManagerErrorInvalidNetworkDeviceType;

    // Get the access point object path
    WirelessAccessPoint *accessPoint = wirelessNetworkDevice->getAccessPoint(ssid);
    if (!accessPoint)
        return NetworkManagerErrorAccessPointNotFound;

    // Note: https://developer.gnome.org/NetworkManager/stable/ref-settings.html

    QVariantMap connectionSettings;
    connectionSettings.insert("autoconnect", true);
    connectionSettings.insert("id", ssid + " (loop)");
    connectionSettings.insert("uuid", QUuid::createUuid().toString().remove("{").remove("}"));
    connectionSettings.insert("type", "802-11-wireless");

    QVariantMap wirelessSettings;
    wirelessSettings.insert("ssid", ssid.toUtf8());
    wirelessSettings.insert("mode", "infrastructure");

    // Note: disable power save mode
    wirelessSettings.insert("powersave", 2);

    if (hidden)
        wirelessSettings.insert("hidden", true);

    QVariantMap wirelessSecuritySettings;
    wirelessSecuritySettings.insert("auth-alg", "open");
    wirelessSecuritySettings.insert("key-mgmt", "wpa-psk");
    wirelessSecuritySettings.insert("psk", password);

    QVariantMap ipv4Settings;
    ipv4Settings.insert("method", "auto");

    QVariantMap ipv6Settings;
    ipv6Settings.insert("method", "auto");

    // Build connection object
    ConnectionSettings settings;
    settings.insert("connection", connectionSettings);
    settings.insert("802-11-wireless", wirelessSettings);
    settings.insert("ipv4", ipv4Settings);
    settings.insert("ipv6", ipv6Settings);
    settings.insert("802-11-wireless-security", wirelessSecuritySettings);

    // Remove old configuration (if there is any)
    foreach (NetworkConnection *connection, m_networkSettings->connections()) {
        if (connection->id() == connectionSettings.value("id")) {
            connection->deleteConnection();
        }
    }

    // Add connection
    QDBusObjectPath connectionObjectPath = m_networkSettings->addConnection(settings);
    if (connectionObjectPath.path().isEmpty())
        return NetworkManagerErrorWirelessConnectionFailed;

    // Activate connection
    QDBusMessage query = m_networkManagerInterface->call("ActivateConnection", QVariant::fromValue(connectionObjectPath), QVariant::fromValue(wirelessNetworkDevice->objectPath()), QVariant::fromValue(accessPoint->objectPath()));
    if(query.type() != QDBusMessage::ReplyMessage) {
        qCWarning(dcNetworkManager()) << query.errorName() << query.errorMessage();
        return NetworkManagerErrorWirelessConnectionFailed;
    }

    return NetworkManagerErrorNoError;
}

/*! Returns true if the networking of this \l{NetworkManager} is enabled. */
bool NetworkManager::networkingEnabled() const
{
    return m_networkingEnabled;
}

/*! Returns true if the networking of this \l{NetworkManager} could be \a enabled. */
bool NetworkManager::enableNetworking(const bool &enabled)
{
    if (m_networkingEnabled == enabled)
        return true;

    QDBusMessage query = m_networkManagerInterface->call("Enable", enabled);
    if(query.type() != QDBusMessage::ReplyMessage) {
        qCWarning(dcNetworkManager()) << query.errorName() << query.errorMessage();
        return false;
    }
    return true;
}

/*! Sets the networking of this \l{NetworkManager} to \a enabled. */
void NetworkManager::setNetworkingEnabled(const bool &enabled)
{
    qCDebug(dcNetworkManager()) << "Networking" << (enabled ? "enabled" : "disabled");
    m_networkingEnabled = enabled;
    emit networkingEnabledChanged();
}

/*! Returns true if the wireless networking of this \l{NetworkManager} is enabled. */
bool NetworkManager::wirelessEnabled() const
{
    return m_wirelessEnabled;
}

/*! Returns true if the wireless networking of this \l{NetworkManager} could be set to \a enabled. */
bool NetworkManager::enableWireless(const bool &enabled)
{
    if (m_wirelessEnabled == enabled)
        return true;

    return m_networkManagerInterface->setProperty("WirelessEnabled", enabled);
}

void NetworkManager::loadDevices()
{
    // Get network devices
    QDBusMessage query = m_networkManagerInterface->call("GetDevices");
    if(query.type() != QDBusMessage::ReplyMessage) {
        qCWarning(dcNetworkManager()) << query.errorName() << query.errorMessage();
        return;
    }

    if (query.arguments().isEmpty())
        return;

    const QDBusArgument &argument = query.arguments().at(0).value<QDBusArgument>();
    argument.beginArray();
    while(!argument.atEnd()) {
        QDBusObjectPath deviceObjectPath = qdbus_cast<QDBusObjectPath>(argument);
        onDeviceAdded(deviceObjectPath);
    }
    argument.endArray();
}

QString NetworkManager::networkManagerStateToString(const NetworkManager::NetworkManagerState &state)
{
    QMetaObject metaObject = NetworkManager::staticMetaObject;
    int enumIndex = metaObject.indexOfEnumerator(QString("NetworkManagerState").toLatin1().data());
    QMetaEnum metaEnum = metaObject.enumerator(enumIndex);
    return QString(metaEnum.valueToKey(state));
}

QString NetworkManager::networkManagerConnectivityStateToString(const NetworkManager::NetworkManagerConnectivityState &state)
{
    QMetaObject metaObject = NetworkManager::staticMetaObject;
    int enumIndex = metaObject.indexOfEnumerator(QString("NetworkManagerConnectivityState").toLatin1().data());
    QMetaEnum metaEnum = metaObject.enumerator(enumIndex);
    return QString(metaEnum.valueToKey(state)).remove("NetworkManagerConnectivityState");
}

void NetworkManager::setVersion(const QString &version)
{
    qCDebug(dcNetworkManager()) << "Version:" << version;
    m_version = version;
    emit versionChanged();
}

void NetworkManager::setWirelessEnabled(const bool &enabled)
{
    qCDebug(dcNetworkManager()) << "Wireless networking" << (enabled ? "enabled" : "disabled");
    m_wirelessEnabled = enabled;
    emit wirelessEnabledChanged();
}

void NetworkManager::setConnectivityState(const NetworkManager::NetworkManagerConnectivityState &connectivityState)
{
    qCDebug(dcNetworkManager()) << "Connectivity state changed:" << networkManagerConnectivityStateToString(connectivityState);
    m_connectivityState = connectivityState;
    emit connectivityStateChanged();
}

void NetworkManager::setState(const NetworkManager::NetworkManagerState &state)
{
    qCDebug(dcNetworkManager()) << "State changed:" << networkManagerStateToString(state);
    m_state = state;
    emit stateChanged();
}

void NetworkManager::onDeviceAdded(const QDBusObjectPath &deviceObjectPath)
{
    if (m_networkDevices.keys().contains(deviceObjectPath)) {
        qCWarning(dcNetworkManager()) << "Device" << deviceObjectPath.path() << "already added.";
        return;
    }

    // Get device Type
    QDBusInterface networkDeviceInterface(NetworkManagerUtils::networkManagerServiceString(), deviceObjectPath.path(), NetworkManagerUtils::deviceInterfaceString(), QDBusConnection::systemBus());
    if(!networkDeviceInterface.isValid()) {
        qCWarning(dcNetworkManager()) << "NetworkDevice: Invalid DBus device interface" << deviceObjectPath.path();
        return;
    }

    // Create object
    NetworkDevice::NetworkDeviceType deviceType = NetworkDevice::NetworkDeviceType(networkDeviceInterface.property("DeviceType").toUInt());
    switch (deviceType) {
    case NetworkDevice::NetworkDeviceTypeWifi: {
        WirelessNetworkDevice *wirelessNetworkDevice = new WirelessNetworkDevice(deviceObjectPath, this);
        qCDebug(dcNetworkManager()) << "[+]" << wirelessNetworkDevice;
        m_networkDevices.insert(deviceObjectPath, wirelessNetworkDevice);
        m_wirelessNetworkDevices.insert(deviceObjectPath, wirelessNetworkDevice);
        connect(wirelessNetworkDevice, &WirelessNetworkDevice::deviceChanged, this, &NetworkManager::onWirelessDeviceChanged);
        emit wirelessDeviceAdded(wirelessNetworkDevice);
        break;
    }
    case NetworkDevice::NetworkDeviceTypeEthernet: {
        WiredNetworkDevice *wiredNetworkDevice = new WiredNetworkDevice(deviceObjectPath, this);
        qCDebug(dcNetworkManager()) << "[+]" << wiredNetworkDevice;
        m_networkDevices.insert(deviceObjectPath, wiredNetworkDevice);
        m_wiredNetworkDevices.insert(deviceObjectPath, wiredNetworkDevice);

        connect(wiredNetworkDevice, &WiredNetworkDevice::deviceChanged, this, &NetworkManager::onWiredDeviceChanged);
        emit wiredDeviceAdded(wiredNetworkDevice);
        break;
    }
    default:
        NetworkDevice *networkDevice = new NetworkDevice(deviceObjectPath, this);
        qCDebug(dcNetworkManager()) << "[+]" << networkDevice;
        m_networkDevices.insert(deviceObjectPath, networkDevice);
        break;
    }
}

void NetworkManager::onDeviceRemoved(const QDBusObjectPath &deviceObjectPath)
{
    if (!m_networkDevices.keys().contains(deviceObjectPath)) {
        qCWarning(dcNetworkManager()) << "Unknown network device removed:" << deviceObjectPath.path();
        return;
    }

    NetworkDevice *networkDevice = m_networkDevices.take(deviceObjectPath);

    if (m_wiredNetworkDevices.contains(deviceObjectPath)) {
        qCDebug(dcNetworkManager()) << "[-]" << m_wiredNetworkDevices.value(deviceObjectPath);
        m_wiredNetworkDevices.remove(deviceObjectPath);
        emit wiredDeviceRemoved(networkDevice->interface());
    } else if (m_wirelessNetworkDevices.contains(deviceObjectPath)) {
        qCDebug(dcNetworkManager()) << "[-]" << m_wirelessNetworkDevices.value(deviceObjectPath);
        m_wirelessNetworkDevices.remove(deviceObjectPath);
        if (!wirelessAvailable())
            emit wirelessAvailableChanged();

        emit wirelessDeviceRemoved(networkDevice->interface());
    } else {
        qCDebug(dcNetworkManager()) << "[-]" << networkDevice;
    }

    networkDevice->deleteLater();
}

void NetworkManager::onPropertiesChanged(const QVariantMap &properties)
{
    if (properties.contains("Version"))
        setVersion(properties.value("Version").toString());

    if (properties.contains("State"))
        setState((NetworkManagerState)properties.value("State").toUInt());

    if (properties.contains("Connectivity"))
        setConnectivityState(NetworkManagerConnectivityState(properties.value("Connectivity").toUInt()));

    if (properties.contains("NetworkingEnabled"))
        setNetworkingEnabled(properties.value("NetworkingEnabled").toBool());

    if (properties.contains("WirelessEnabled"))
        setWirelessEnabled(properties.value("WirelessEnabled").toBool());

}

void NetworkManager::onWirelessDeviceChanged()
{
    WirelessNetworkDevice *networkDevice = static_cast<WirelessNetworkDevice *>(sender());
    emit wirelessDeviceChanged(networkDevice);
}

void NetworkManager::onWiredDeviceChanged()
{
    WiredNetworkDevice *networkDevice = static_cast<WiredNetworkDevice *>(sender());
    emit wiredDeviceChanged(networkDevice);
}

