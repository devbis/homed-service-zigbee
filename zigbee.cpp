#include <QtEndian>
#include <QFile>
#include <QRandomGenerator>
#include "ezsp.h"
#include "gpio.h"
#include "logger.h"
#include "zcl.h"
#include "zigbee.h"
#include "zstack.h"

ZigBee::ZigBee(QSettings *config, QObject *parent) : QObject(parent), m_config(config), m_requestTimer(new QTimer(this)), m_neignborsTimer(new QTimer(this)), m_statusLedTimer(new QTimer(this)), m_devices(new DeviceList(m_config)), m_adapter(nullptr), m_requestId(0)
{
    m_statusLedPin = m_config->value("gpio/status", "-1").toString();
    m_blinkLedPin = m_config->value("gpio/blink", "-1").toString();

    connect(m_devices, &DeviceList::pollRequest, this, &ZigBee::pollRequest);
    connect(m_devices, &DeviceList::statusUpdated, this, &ZigBee::statusUpdated);
    connect(m_statusLedTimer, &QTimer::timeout, this, &ZigBee::updateStatusLed);

    GPIO::direction(m_statusLedPin, GPIO::Output);
    GPIO::setStatus(m_statusLedPin, m_statusLedPin != m_blinkLedPin);

    if (m_statusLedPin == m_blinkLedPin)
        return;

    GPIO::direction(m_blinkLedPin, GPIO::Output);
    GPIO::setStatus(m_blinkLedPin, false);
}

void ZigBee::init(void)
{
    QList <QString> list = {"ezsp", "znp"};
    QString adapterType = m_config->value("zigbee/adapter", "znp").toString();

    switch (list.indexOf(adapterType))
    {
        case 0:  m_adapter = new EZSP(m_config, this); break;
        case 1:  m_adapter = new ZStack(m_config, this); break;
        default: logWarning << "Unrecognized adapter type" << adapterType; return;
    }

    connect(m_adapter, &Adapter::coordinatorReady, this, &ZigBee::coordinatorReady);
    connect(m_adapter, &Adapter::permitJoinUpdated, this, &ZigBee::permitJoinUpdated);
    connect(m_adapter, &Adapter::requestFinished, this, &ZigBee::requestFinished);

    m_devices->init();
    m_adapter->init();
}

void ZigBee::setPermitJoin(bool enabled)
{
    if (!m_adapter)
        return;

    m_adapter->setPermitJoin(enabled);
}

void ZigBee::setDeviceName(const QString &deviceName, const QString &newName, bool store)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    device->setName(newName);

    if (!store)
        return;

    m_devices->storeDatabase();
}

void ZigBee::removeDevice(const QString &deviceName, bool force)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    if (!force)
    {
        enqueueDeviceRequest(device, RequestType::Remove);
        return;
    }

    logInfo << "Device" << device->name() << "removed (force)";
    m_devices->removeDevice(device);
    m_devices->storeDatabase();
}

void ZigBee::updateDevice(const QString &deviceName, bool reportings)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    m_devices->setupDevice(device);

    if (!reportings)
    {
        logInfo << "Device" << device->name() << "configuration updated without reportings";
        return;
    }

    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        for (int i = 0; i < it.value()->reportings().count(); i++)
            configureReporting(it.value(), it.value()->reportings().at(i));

    logInfo << "Device" << device->name() << "configuration updated";
}

void ZigBee::updateReporting(const QString &deviceName, quint8 endpointId, const QString &reportingName, quint16 minInterval, quint16 maxInterval, quint16 valueChange)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        if (endpointId && it.value()->id() != endpointId)
            continue;

        for (int i = 0; i < it.value()->reportings().count(); i++)
        {
            const Reporting &reporting = it.value()->reportings().at(i);

            if (!reportingName.isEmpty() && reporting->name() != reportingName)
                continue;

            if (minInterval)
                reporting->setMinInterval(minInterval);

            if (maxInterval)
                reporting->setMaxInterval(maxInterval);

            if (valueChange)
                reporting->setValueChange(valueChange);

            configureReporting(it.value(), reporting);
        }
    }
}

void ZigBee::bindingControl(const QString &deviceName, quint8 endpointId, quint16 clusterId, const QVariant &dstName, quint8 dstEndpointId, bool unbind)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    switch (dstName.type())
    {
        case QVariant::LongLong:
        {
            quint16 value = qToLittleEndian <quint16> (dstName.toInt());

            if (value)
                enqueueBindingRequest(device, endpointId, clusterId, QByteArray(reinterpret_cast <char*> (&value), sizeof(value)), 0xFF, unbind);

            break;
        }

        case QVariant::String:
        {
            Device destination = m_devices->byName(deviceName);

            if (!destination.isNull() && !destination->removed())
                enqueueBindingRequest(device, endpointId, clusterId, destination->ieeeAddress(), dstEndpointId, unbind);

            break;
        }

        default:
            break;
    }
}

void ZigBee::groupControl(const QString &deviceName, quint8 endpointId, quint16 groupId, bool remove)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    groupId = qFromLittleEndian(groupId);
    enqueueDataRequest(device, endpointId ? endpointId : 1, CLUSTER_GROUPS, zclHeader(FC_CLUSTER_SPECIFIC, m_requestId, remove ? 0x03 : 0x00).append(reinterpret_cast <char*> (&groupId), sizeof(groupId)).append(remove ? 0 : 1, 0x00));
}

void ZigBee::removeAllGroups(const QString &deviceName, quint8 endpointId)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    enqueueDataRequest(device, endpointId ? endpointId : 1, CLUSTER_GROUPS, zclHeader(FC_CLUSTER_SPECIFIC, m_requestId, 0x04), QString("remove all groups request"));
}

void ZigBee::otaUpgrade(const QString &deviceName, quint8 endpointId, const QString &fileName)
{
    Device device = m_devices->byName(deviceName);
    otaImageNotifyStruct payload;

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator || fileName.isEmpty() || !QFile::exists(fileName))
        return;

    m_otaUpgradeFile = fileName;

    payload.type = 0x00;
    payload.jitter = 0x64; // TODO: check this

    enqueueDataRequest(device, endpointId ? endpointId : 1, CLUSTER_OTA_UPGRADE, zclHeader(FC_CLUSTER_SPECIFIC | FC_SERVER_TO_CLIENT, m_requestId, 0x00).append(reinterpret_cast <char*> (&payload), sizeof(payload)));
}

void ZigBee::touchLinkRequest(const QByteArray &ieeeAddress, quint8 channel, bool reset)
{
    if (m_adapter->setInterPanEndpointId(0x0C))
    {
        if (reset)
            touchLinkReset(ieeeAddress, channel);
        else
            touchLinkScan();

        m_adapter->resetInterPan();
    }
}

void ZigBee::deviceAction(const QString &deviceName, quint8 endpointId, const QString &actionName, const QVariant &actionData)
{
    Device device = m_devices->byName(deviceName);

    if (device.isNull() || device->removed() || device->logicalType() == LogicalType::Coordinator)
        return;

    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        if (endpointId && it.value()->id() != endpointId)
            continue;

        for (int i = 0; i < it.value()->actions().count(); i++)
        {
            const Action &action = it.value()->actions().at(i);

            if (action->name() == actionName)
            {
                QByteArray data = action->request(actionData);

                if (!data.isEmpty())
                    enqueueDataRequest(device, it.value()->id(), action->clusterId(), data, QString("%1 action").arg(action->name()));

                if (action->poll())
                    enqueueDataRequest(device, it.value()->id(), action->clusterId(), attributesRequest(m_requestId, {action->attributeId()}, action->manufacturerCode()));

                break;
            }
        }
    }
}

void ZigBee::groupAction(quint16 groupId, const QString &actionName, const QVariant &actionData)
{
    int type = QMetaType::type(QString(actionName).append("Action").toUtf8());

    if (type)
    {
        Action action(reinterpret_cast <ActionObject*> (QMetaType::create(type)));
        QByteArray data = action->request(actionData);

        if (data.isEmpty())
            return;

        m_adapter->extendedDataRequest(m_requestId, groupId, 0xFF, 0x0000, 0x01, action->clusterId(), data, true);
    }
}

void ZigBee::enqueueBindingRequest(const Device &device, quint8 endpointId, quint16 clusterId, const QByteArray &dstAddress, quint8 dstEndpointId, bool unbind)
{
    BindingRequest request(new BindingRequestObject(device, endpointId, clusterId, dstAddress, dstEndpointId, unbind));

    if (!m_requestTimer->isActive())
        m_requestTimer->start();

    m_requests.insert(m_requestId++, Request(new RequestObject(QVariant::fromValue(request), RequestType::Binding)));
}

void ZigBee::enqueueDataRequest(const Device &device, quint8 endpointId, quint16 clusterId, const QByteArray &data, const QString &name)
{
    DataRequest request(new DataRequestObject(device, endpointId, clusterId, data, name));

    if (!m_requestTimer->isActive())
        m_requestTimer->start();

    m_requests.insert(m_requestId++, Request(new RequestObject(QVariant::fromValue(request), RequestType::Data)));
}

void ZigBee::enqueueDeviceRequest(const Device &device, RequestType type)
{
    if (!m_requestTimer->isActive())
        m_requestTimer->start();

    m_requests.insert(m_requestId++, Request(new RequestObject(QVariant::fromValue(device), type)));
}

QByteArray ZigBee::attributesRequest(quint8 id, QList <quint16> attributes, quint16 manufacturerCode)
{
    QByteArray request = zclHeader(0x00, id, CMD_READ_ATTRIBUTES, manufacturerCode);

    for (int i = 0; i < attributes.count(); i++)
    {
        quint16 attributeId = qToLittleEndian(attributes.at(i));
        request.append(reinterpret_cast <char*> (&attributeId), sizeof(attributeId));
    }

    return request;
}

bool ZigBee::interviewRequest(quint8 id, const Device &device)
{
    if (device->manufacturerName().isEmpty() || device->modelName().isEmpty())
    {
        if (!device->descriptorReceived())
        {
            if (m_adapter->nodeDescriptorRequest(id, device->networkAddress()))
                return true;

            interviewError(device, "node descriptor request failed");
            return false;
        }

        if (!device->endpointsReceived())
        {
            if (m_adapter->activeEndpointsRequest(id, device->networkAddress()))
                return true;

            interviewError(device, "active endpoints request failed");
            return false;
        }

        for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        {
            if (!it.value()->descriptorReceived())
            {
                device->setInterviewEndpointId(it.value()->id());

                if (m_adapter->simpleDescriptorRequest(id, device->networkAddress(), it.key()))
                    return true;

                interviewError(device, QString::asprintf("endpoint 0x%02X simple descriptor request failed", it.key()));
                return false;
            }
        }

        for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        {
            if (it.value()->inClusters().contains(CLUSTER_BASIC))
            {
                if (m_adapter->dataRequest(id, device->networkAddress(), it.key(), CLUSTER_BASIC, attributesRequest(id, {0x0001, 0x0004, 0x0005, 0x0007})))
                    return true;

                interviewError(device, "read basic attributes request failed");
                return false;
            }
        }

        interviewError(device, "device has empty manufacturer name or model name");
        return false;
    }

    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
    {
        if (!it.value()->inClusters().contains(CLUSTER_IAS_ZONE))
            continue;

        switch (it.value()->zoneStatus())
        {
            case ZoneStatus::Unknown:
            {
                if (m_adapter->dataRequest(id, device->networkAddress(), it.key(), CLUSTER_IAS_ZONE, attributesRequest(id, {0x0000, 0x0010})))
                    return true;

                interviewError(device, "read current IAS zone status request failed");
                return false;
            }

            case ZoneStatus::SetAddress:
            {
                writeArrtibutesStruct payload;
                quint64 ieeeAddress = m_adapter->ieeeAddress();

                payload.attributeId = qToLittleEndian <quint16> (0x0010);
                payload.dataType = DATA_TYPE_IEEE_ADDRESS;

                if (m_adapter->dataRequest(id, device->networkAddress(), it.value()->id(), CLUSTER_IAS_ZONE, zclHeader(FC_DISABLE_DEFAULT_RESPONSE, id, CMD_WRITE_ATTRIBUTES).append(reinterpret_cast <char*> (&payload), sizeof(payload)).append(reinterpret_cast <char*> (&ieeeAddress), sizeof(ieeeAddress))))
                    return true;

                interviewError(device, "write IAS zone CIE address request failed");
                return false;
            }

            case ZoneStatus::Enroll:
            {
                iasZoneEnrollResponseStruct payload;

                payload.responseCode = 0x00;
                payload.zoneId = 0x42;

                if (m_adapter->dataRequest(id, device->networkAddress(), it.value()->id(), CLUSTER_IAS_ZONE, zclHeader(FC_CLUSTER_SPECIFIC | FC_DISABLE_DEFAULT_RESPONSE, id, 0x00).append(reinterpret_cast <char*> (&payload), sizeof(payload))) && m_adapter->dataRequest(id, device->networkAddress(), it.key(), CLUSTER_IAS_ZONE, attributesRequest(id, {0x0000, 0x0010})))
                    return true;

                interviewError(device, "enroll IAS zone request failed");
                return false;
            }

            case ZoneStatus::Enrolled:
            {
                logInfo << "Device" << device->name() << "endpoint" << QString::asprintf("0x%02X", it.value()->id()) << "IAS zone enrolled";
                break;
            }
        }
    }

    interviewFinished(device);
    return true;
}

void ZigBee::interviewDevice(const Device &device)
{
    if (device->interviewFinished())
        return;

    enqueueDeviceRequest(device, RequestType::Interview);
    device->timer()->start(DEVICE_INTERVIEW_TIMEOUT);
}

void ZigBee::interviewFinished(const Device &device)
{
    logInfo << "Device" << device->name() << "manufacturer name is" << device->manufacturerName() << "and model name is" << device->modelName();
    m_devices->setupDevice(device);

    if (!device->description().isEmpty())
        logInfo << "Device" << device->name() << "identified as" << device->description();

    for (auto it = device->endpoints().begin(); it != device->endpoints().end(); it++)
        for (int i = 0; i < it.value()->reportings().count(); i++)
            configureReporting(it.value(), it.value()->reportings().at(i));

    logInfo << "Device" << device->name() << "interview finished successfully";
    emit deviceEvent(device, "interviewFinished");

    device->timer()->stop();
    device->setInterviewFinished();

    m_devices->storeDatabase();
}

void ZigBee::interviewError(const Device &device, const QString &reason)
{
    if (!device->timer()->isActive())
        return;

    logWarning << "Device" << device->name() << "interview error:" << reason;
    emit deviceEvent(device, "interviewError");

    device->timer()->stop();
}

void ZigBee::configureReporting(const Endpoint &endpoint, const Reporting &reporting)
{
    Device device = endpoint->device();
    QByteArray request = zclHeader(0x00, m_requestId, CMD_CONFIGURE_REPORTING);

    enqueueBindingRequest(device, endpoint->id(), reporting->clusterId());

    for (int i = 0; i < reporting->attributes().count(); i++)
    {
        configureReportingStruct item;

        item.direction = 0x00;
        item.attributeId = qToLittleEndian(reporting->attributes().at(i));
        item.dataType = reporting->dataType();
        item.minInterval = qToLittleEndian(reporting->minInterval());
        item.maxInterval = qToLittleEndian(reporting->maxInterval());
        item.valueChange = qToLittleEndian(reporting->valueChange());

        request.append(reinterpret_cast <char*> (&item), sizeof(item) - sizeof(item.valueChange) + zclDataSize(item.dataType));
    }

    enqueueDataRequest(device, endpoint->id(), reporting->clusterId(), request, QString("%1 reporting configuration").arg(reporting->name()));
}

void ZigBee::parseAttribute(const Endpoint &endpoint, quint16 clusterId, quint16 attributeId, quint8 dataType, const QByteArray &data)
{
    Device device = endpoint->device();
    bool check = false;

    if (clusterId == CLUSTER_BASIC) // TODO: check if any devices sends some data via basic cluster
    {
        switch (attributeId)
        {
            case 0x0001:

                if (dataType != DATA_TYPE_8BIT_UNSIGNED)
                    return;

                device->setVersion(static_cast <quint8> (data.at(0)));
                break;

            case 0x0004:

                if (dataType != DATA_TYPE_CHARACTER_STRING)
                    return;

                device->setManufacturerName(QString(data).trimmed());
                break;

            case 0x0005:

                if (dataType != DATA_TYPE_CHARACTER_STRING)
                    return;

                device->setModelName(QString(data).trimmed());

                if (device->manufacturerName().isEmpty() && device->modelName().startsWith("lumi.sensor")) // some LUMI devices send modelName attribute on join
                {
                    device->setPowerSource(POWER_SOURCE_BATTERY);
                    device->setManufacturerName("LUMI");
                    interviewFinished(device);
                    return;
                }

                break;

            case 0x0007:

                if (dataType != DATA_TYPE_8BIT_UNSIGNED && dataType != DATA_TYPE_8BIT_ENUM)
                    return;

                device->setPowerSource(static_cast <quint8> (data.at(0)));
                break;
        }

        if (!device->interviewFinished() && !device->manufacturerName().isEmpty() && !device->modelName().isEmpty() && (attributeId == 0x0004 || attributeId == 0x0005))
        {
            QList <QString> tuya = // TUYA devices model names
            {
                "TS0001", "TS0002", "TS0004", "TS0004",
                "TS0011", "TS0012", "TS0013", "TS0014",
                "TS0201", "TS0202", "TS0203", "TS0204", "TS0205", "TS0207",
                "TS0601"
            };

            if (!device->manufacturerName().isEmpty() && tuya.contains(device->modelName()))
            {
                QList <QString> list = {"TS0001", "TS0011", "TS0201", "TS0202", "TS0207", "TS0601"};

                if (list.contains(device->modelName()))
                    device->setModelName(device->manufacturerName());

                device->setManufacturerName("TUYA");
            }

            interviewDevice(device);
        }

        return;
    }

    if (clusterId == CLUSTER_IAS_ZONE && (attributeId == 0x0000 || attributeId == 0x0010))
    {
        switch (attributeId)
        {
            case 0x0000:
            {
                if (dataType != DATA_TYPE_8BIT_ENUM)
                    return;

                endpoint->setZoneStatus(data.at(0) ? ZoneStatus::Enrolled : ZoneStatus::Enroll);
                break;
            }

            case 0x0010:
            {
                quint64 ieeeAddress = m_adapter->ieeeAddress();

                if (dataType != DATA_TYPE_IEEE_ADDRESS)
                    return;

                if (memcmp(&ieeeAddress, data.constData(), sizeof(ieeeAddress)))
                    endpoint->setZoneStatus(ZoneStatus::SetAddress);

                interviewDevice(device);
                break;
            }
        }

        return;
    }

    if (!device->interviewFinished())
        return;

    for (int i = 0; i < endpoint->properties().count(); i++)
    {
        const Property &property = endpoint->properties().at(i);

        if (property->clusterId() == clusterId)
        {
            QVariant value = property->value();

            property->parseAttribte(attributeId, dataType, data);
            check = true;

            if (property->value() == value)
                continue;

            endpoint->setUpdated(true);
        }
    }

    if (!check)
        logWarning << "No property found for device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "cluster" << QString::asprintf("0x%04X", clusterId) << "attribute" << QString::asprintf("0x%04X", attributeId) << "with data type" << QString::asprintf("0x%02X", dataType) << "and data" << data.toHex(':');
}

void ZigBee::clusterCommandReceived(const Endpoint &endpoint, quint16 clusterId, quint8 transactionId, quint8 commandId, const QByteArray &payload)
{
    Device device = endpoint->device();
    bool check = false;

    if (!device->interviewFinished())
        return;

    if (clusterId == CLUSTER_GROUPS)
    {
        switch (commandId)
        {
            case 0x00:
            case 0x03:
            {
                const groupControlResponseStruct *response = reinterpret_cast <const groupControlResponseStruct*> (payload.constData());

                switch (response->status)
                {
                    case STATUS_SUCCESS:
                        logInfo << "Device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "group" << qFromLittleEndian(response->groupId) << "successfully" << (commandId ? "removed" : "added");
                        break;

                    case STATUS_INSUFFICIENT_SPACE:
                        logWarning << "Device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "group" << qFromLittleEndian(response->groupId) << "not added, no free space available";
                        break;

                    case STATUS_DUPLICATE_EXISTS:
                        logWarning << "Device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "group" << qFromLittleEndian(response->groupId) << "already exists";
                        break;

                    case STATUS_NOT_FOUND:
                        logWarning << "Device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "group" << qFromLittleEndian(response->groupId) << "not found";
                        break;

                    default:
                        logWarning << "Device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "group" << qFromLittleEndian(response->groupId) << (commandId ? "remove" : "add") << "command status" << QString::asprintf("0x%02X", response->status) << "unrecognized";
                        break;
                }

                break;
            }

            default:
                logWarning << "Unrecognized group control command" << QString::asprintf("0x%02X", commandId) << "received from device" << device->name() << "with payload:" << payload.toHex(':');
                break;
        }

        return;
    }

    if (clusterId == CLUSTER_OTA_UPGRADE)
    {
        QFile file(m_otaUpgradeFile);
        otaFileHeaderStruct header;

        memset(&header, 0, sizeof(header));

        if (file.exists() && file.open(QFile::ReadOnly))
            memcpy(&header, file.read(sizeof(header)).constData(), sizeof(header));

        switch (commandId)
        {
            case 0x01:
            {
                const otaNextImageRequestStruct *request = reinterpret_cast <const otaNextImageRequestStruct*> (payload.constData());
                otaNextImageResponseStruct response;

                if (!file.isOpen() || request->manufacturerCode != header.manufacturerCode || request->imageType != header.imageType)
                {
                    enqueueDataRequest(device, endpoint->id(), CLUSTER_OTA_UPGRADE, zclHeader(FC_CLUSTER_SPECIFIC | FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, 0x02).append(STATUS_NO_IMAGE_AVAILABLE));
                    break;
                }

                if (request->fileVersion == header.fileVersion)
                {
                    logInfo << "Device" << device->name() << "OTA upgrade not started, version match:" << QString::asprintf("0x%08X", qFromLittleEndian(request->fileVersion)).toUtf8().constData();
                    enqueueDataRequest(device, endpoint->id(), CLUSTER_OTA_UPGRADE, zclHeader(FC_CLUSTER_SPECIFIC | FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, 0x02).append(STATUS_NO_IMAGE_AVAILABLE));
                    break;
                }

                logInfo << "Device" << device->name() << "OTA upgrade started...";

                response.status = 0x00;
                response.manufacturerCode = header.manufacturerCode;
                response.imageType = header.imageType;
                response.fileVersion = header.fileVersion;
                response.imageSize = header.imageSize;

                enqueueDataRequest(device, endpoint->id(), CLUSTER_OTA_UPGRADE, zclHeader(FC_CLUSTER_SPECIFIC | FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, 0x02).append(reinterpret_cast <char*> (&response), sizeof(response)));
                break;
            }

            case 0x03:
            {
                const otaImageBlockRequestStruct *request = reinterpret_cast <const otaImageBlockRequestStruct*> (payload.constData());
                otaImageBlockResponseStruct response;
                QByteArray block;

                if (!file.isOpen() || request->manufacturerCode != header.manufacturerCode || request->imageType != header.imageType ||request->fileVersion != header.fileVersion)
                {
                    enqueueDataRequest(device, endpoint->id(), CLUSTER_OTA_UPGRADE, zclHeader(FC_CLUSTER_SPECIFIC | FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, 0x05).append(STATUS_NO_IMAGE_AVAILABLE));
                    break;
                }

                file.seek(qFromLittleEndian(request->fileOffset));
                block = file.read(request->dataSizeMax);

                // TODO: use percentage here
                logInfo << "Device" << device->name() << "OTA upgrade writing" << block.length() << "bytes with offset" << QString::asprintf("0x%04X", qFromLittleEndian(request->fileOffset));

                response.status = 0x00;
                response.manufacturerCode = request->manufacturerCode;
                response.imageType = request->imageType;
                response.fileVersion = request->fileVersion;
                response.fileOffset = request->fileOffset;
                response.dataSize = static_cast <quint8> (block.length());

                enqueueDataRequest(device, endpoint->id(), CLUSTER_OTA_UPGRADE, zclHeader(FC_CLUSTER_SPECIFIC | FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, 0x05).append(reinterpret_cast <char*> (&response), sizeof(response)).append(block));
                break;
            }
            case 0x06:
            {
                const otaUpgradeEndRequestStruct *request = reinterpret_cast <const otaUpgradeEndRequestStruct*> (payload.constData());
                otaUpgradeEndResponseStruct response;

                m_otaUpgradeFile.clear();

                if (request->status)
                {
                    logWarning << "Device" << device->name() << "OTA upgrade failed, status code:" << QString::asprintf("%02X", request->status);;
                    break;
                }

                logInfo << "Device" << device->name() << "OTA upgrade finished successfully";

                response.manufacturerCode = request->manufacturerCode;
                response.imageType = request->imageType;
                response.fileVersion = request->fileVersion;
                response.currentTime = 0;
                response.upgradeTime = 0;

                enqueueDataRequest(device, endpoint->id(), CLUSTER_OTA_UPGRADE, zclHeader(FC_CLUSTER_SPECIFIC | FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, 0x07).append(reinterpret_cast <char*> (&response), sizeof(response)));
                break;
            }

            default:
                logWarning << "Unrecognized OTA upgrade command" << QString::asprintf("0x%02X", commandId) << "received from device" << device->name() << "with payload:" << payload.toHex(':');
                break;
        }

        if (file.isOpen())
            file.close();

        return;
    }

    for (int i = 0; i < endpoint->properties().count(); i++)
    {
        const Property &property = endpoint->properties().at(i);

        if (property->clusterId() == clusterId)
        {
            QVariant value = property->value();

            property->parseCommand(commandId, payload);
            check = true;

            if (property->value() == value)
                continue;

            endpoint->setUpdated(true);
        }
    }

    if (!check)
        logWarning << "No property found for device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "cluster" << QString::asprintf("0x%04X", clusterId) << "command" << QString::asprintf("0x%02X", commandId) << "with payload" << payload.toHex(':');
}

void ZigBee::globalCommandReceived(const Endpoint &endpoint, quint16 clusterId, quint8 transactionId, quint8 commandId, QByteArray payload)
{
    Device device = endpoint->device();

    switch (commandId)
    {
        case CMD_CONFIGURE_REPORTING_RESPONSE:
        case CMD_DEFAULT_RESPONSE:
            break;

        case CMD_READ_ATTRIBUTES:
        {
            QByteArray request = zclHeader(FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, CMD_READ_ATTRIBUTES_RESPONSE);
            quint16 attributeId;

            for (quint8 i = 0; i < payload.length(); i += sizeof(attributeId))
            {
                memcpy(&attributeId, payload.constData() + i, sizeof(attributeId));
                attributeId = qFromLittleEndian(attributeId);

                request.append(payload.mid(i, sizeof(attributeId)));

                if (clusterId == CLUSTER_TIME && (attributeId == 0x0000 || attributeId == 0x0002 || attributeId == 0x0007))
                {
                    QDateTime now = QDateTime::currentDateTime();
                    quint32 value;

                    request.append(1, static_cast <char> (STATUS_SUCCESS));

                    switch (attributeId)
                    {
                        case 0x0000:
                            logInfo << "Device" << device->name() << "requested UTC time";
                            value = qToLittleEndian <quint32> (now.toTime_t() - 946684800);
                            request.append(1, static_cast <char> (DATA_TYPE_UTC_TIME)).append(reinterpret_cast <char*> (&value), sizeof(value));
                            break;

                        case 0x0002:
                            logInfo << "Device" << device->name() << "requested time zone";
                            value = qToLittleEndian <quint32> (now.offsetFromUtc());
                            request.append(1, static_cast <char> (DATA_TYPE_32BIT_SIGNED)).append(reinterpret_cast <char*> (&value), sizeof(value));
                            break;

                        case 0x0007:
                            logInfo << "Device" << device->name() << "requested local time";
                            value = qToLittleEndian <quint32> (now.toTime_t() + now.offsetFromUtc() - 946684800);
                            request.append(1, static_cast <char> (DATA_TYPE_32BIT_UNSIGNED)).append(reinterpret_cast <char*> (&value), sizeof(value));
                            break;
                    }

                    continue;
                }

                logWarning << "Device" << device->name() << "requested unrecognized attribute" << QString::asprintf("0x%04X", attributeId) << "from cluster" << QString::asprintf("0x%04X", clusterId);
                request.append(1, static_cast <char> (STATUS_UNSUPPORTED_ATTRIBUTE));
            }

            enqueueDataRequest(device, endpoint->id(), clusterId, request);
            break;
        }

        case CMD_READ_ATTRIBUTES_RESPONSE:
        case CMD_REPORT_ATTRIBUTES:
        {
            while (payload.length() > 2)
            {
                quint8 dataType, offset, size = 0;
                quint16 attributeId;

                if (commandId == CMD_READ_ATTRIBUTES_RESPONSE)
                {
                    if (payload.at(2))
                    {
                        payload.remove(0, 3);
                        continue;
                    }

                    dataType = static_cast <quint8> (payload.at(3));
                    offset = 4;
                }
                else
                {
                    dataType = static_cast <quint8> (payload.at(2));
                    offset = 3;
                }

                memcpy(&attributeId, payload.constData(), sizeof(attributeId));
                attributeId = qFromLittleEndian(attributeId);
                size = zclDataSize(dataType, payload, &offset);

                if (dataType != DATA_TYPE_OCTET_STRING && dataType != DATA_TYPE_CHARACTER_STRING && !size)
                {
                    logWarning << "Unrecognized attribute" << QString::asprintf("0x%04X", attributeId) << "data type" << QString::asprintf("0x%02X", dataType) << "received from device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "cluster" << QString::asprintf("0x%04X", clusterId) << "with payload:" << payload.mid(offset).toHex(':');
                    return;
                }

                parseAttribute(endpoint, clusterId, attributeId, dataType, payload.mid(offset, size));
                payload.remove(0, offset + size);
            }

            break;
        }

        case CMD_WRITE_ATTRIBUTES_RESPONSE:
        {
            if (clusterId == CLUSTER_IAS_ZONE && !payload.at(0))
            {
                endpoint->setZoneStatus(ZoneStatus::Enroll);
                interviewDevice(device);
            }

            break;
        }

        default:
            logWarning << "Unrecognized command" << QString::asprintf("0x%02X", commandId) << "received from device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "cluster" << QString::asprintf("0x%04X", clusterId) << "with payload:" << payload.toHex(':');
            break;
    }
}

void ZigBee::touchLinkReset(const QByteArray &ieeeAddress, quint8 channel)
{
    touchLinkScanStruct payload;

    payload.transactionId = QRandomGenerator::global()->generate();
    payload.zigBeeInformation = 0x04;
    payload.touchLinkInformation = 0x12;

    if (!m_adapter->setInterPanChannel(channel))
        return;

    if (!m_adapter->extendedDataRequest(m_requestId, 0xFFFF, 0xFE, 0xFFFF, 0x0C, CLUSTER_TOUCHLINK, zclHeader(FC_CLUSTER_SPECIFIC | FC_DISABLE_DEFAULT_RESPONSE, m_requestId, 0x00).append(QByteArray(reinterpret_cast <char*> (&payload), sizeof(payload)))))
    {
        logWarning << "TouchLink scan request failed";
        return;
    }

    if (!m_adapter->extendedDataRequest(m_requestId, ieeeAddress, 0xFE, 0xFFFF, 0x0C, CLUSTER_TOUCHLINK, zclHeader(FC_CLUSTER_SPECIFIC | FC_DISABLE_DEFAULT_RESPONSE, m_requestId, 0x07).append(QByteArray(reinterpret_cast <char*> (&payload), sizeof(payload.transactionId)))))
    {
        logWarning << "TouchLink reset request failed";
        return;
    }

    logInfo << "TouchLink reset finished successfully";
}

void ZigBee::touchLinkScan(void)
{
    QByteArray request = zclHeader(FC_CLUSTER_SPECIFIC | FC_DISABLE_DEFAULT_RESPONSE, m_requestId, 0x00);
    touchLinkScanStruct payload;

    payload.transactionId = QRandomGenerator::global()->generate();
    payload.zigBeeInformation = 0x04;
    payload.touchLinkInformation = 0x12;

    request.append(QByteArray(reinterpret_cast <char*> (&payload), sizeof(payload)));
    logInfo << "TouchLink scan started...";

    for (m_interPanChannel = 11; m_interPanChannel <= 26; m_interPanChannel++)
    {
        if (!m_adapter->setInterPanChannel(m_interPanChannel))
            return;

        if (!m_adapter->extendedDataRequest(m_requestId, 0xFFFF, 0xFE, 0xFFFF, 0x0C, CLUSTER_TOUCHLINK, request))
        {
            logWarning << "TouchLink scan request failed";
            return;
        }
    }

    logInfo << "TouchLink scan finished successfully";
}

void ZigBee::blink(quint16 timeout)
{
    if (m_statusLedTimer->isActive() && m_statusLedPin == m_blinkLedPin)
        return;

    GPIO::setStatus(m_blinkLedPin, true);
    QTimer::singleShot(timeout, this, &ZigBee::updateBlinkLed);
}

void ZigBee::coordinatorReady(void)
{
    quint64 adapterAddress = qToBigEndian(qFromLittleEndian(m_adapter->ieeeAddress()));
    QByteArray ieeeAddress(reinterpret_cast <char*> (&adapterAddress), sizeof(adapterAddress));
    Device device(new DeviceObject(ieeeAddress, 0x0000, "HOMEd Coordinator"));

    logInfo << "Coordinator ready, address:" << ieeeAddress.toHex(':').constData();

    for (auto it = m_devices->begin(); it != m_devices->end(); it++)
    {
        if (it.key() == ieeeAddress || it.value()->logicalType() == LogicalType::Coordinator)
            m_devices->erase(it++);

        if (it == m_devices->end())
            break;
    }

    device->setLogicalType(LogicalType::Coordinator);
    device->setInterviewFinished();

    m_devices->insert(ieeeAddress, device);
    m_devices->setAdapterType(m_adapter->type());
    m_devices->setAdapterVersion(m_adapter->version());

    connect(m_adapter, &Adapter::deviceJoined, this, &ZigBee::deviceJoined, Qt::UniqueConnection);
    connect(m_adapter, &Adapter::deviceLeft, this, &ZigBee::deviceLeft, Qt::UniqueConnection);
    connect(m_adapter, &Adapter::nodeDescriptorReceived, this, &ZigBee::nodeDescriptorReceived, Qt::UniqueConnection);
    connect(m_adapter, &Adapter::activeEndpointsReceived, this, &ZigBee::activeEndpointsReceived, Qt::UniqueConnection);
    connect(m_adapter, &Adapter::simpleDescriptorReceived, this, &ZigBee::simpleDescriptorReceived, Qt::UniqueConnection);
    connect(m_adapter, &Adapter::neighborRecordReceived, this, &ZigBee::neighborRecordReceived, Qt::UniqueConnection);
    connect(m_adapter, &Adapter::messageReveived, this, &ZigBee::messageReveived, Qt::UniqueConnection);
    connect(m_adapter, &Adapter::extendedMessageReveived, this, &ZigBee::extendedMessageReveived, Qt::UniqueConnection);

    connect(m_requestTimer, &QTimer::timeout, this, &ZigBee::handleRequests, Qt::UniqueConnection);
    connect(m_neignborsTimer, &QTimer::timeout, this, &ZigBee::updateNeighbors, Qt::UniqueConnection);

    if (!m_requests.isEmpty())
        m_requestTimer->start();

    if (!m_neignborsTimer->isActive())
        m_neignborsTimer->start(UPDATE_NEIGHBORS_INTERVAL);

    m_adapter->setPermitJoin(m_devices->permitJoin());
    m_devices->storeDatabase();
}

void ZigBee::permitJoinUpdated(bool enabled)
{
    if (!enabled)
    {
        m_statusLedTimer->stop();
        GPIO::setStatus(m_statusLedPin, m_statusLedPin != m_blinkLedPin);
    }
    else
    {
        m_statusLedTimer->start(STATUS_LED_TIMEOUT);
        GPIO::setStatus(m_statusLedPin, true);
    }

    m_devices->setPermitJoin(enabled);
    m_devices->storeDatabase();
}

void ZigBee::requestFinished(quint8 id, quint8 status)
{
    auto it = m_requests.find(id);

    if (it == m_requests.end() || it.value()->status() == RequestStatus::Finished)
        return;

    switch (it.value()->type())
    {
        case RequestType::Binding:
        {
            BindingRequest request = qvariant_cast <BindingRequest> (it.value()->request());

            if (status)
            {
                logWarning << "Device" << request->device()->name() << "endpoint" << QString::asprintf("0x%02X", request->endpointId()) << "cluster" << QString::asprintf("0x%04X", request->clusterId()) << (request->unbind() ? "unbinding" : "binding") << "failed, status code:" << QString::asprintf("%02X", status);
                break;
            }

            if (!request->dstAddress().isEmpty())
                logInfo << "Device" << request->device()->name() << (request->unbind() ? "unbinding" : "binding") << "finished succesfully";

            break;
        }

        case RequestType::Data:
        {
            DataRequest request = qvariant_cast <DataRequest> (it.value()->request());

            if (status)
            {
                logWarning << "Device" << request->device()->name() << (!request->name().isEmpty() ? request->name().toUtf8().constData() : "data request") << "failed, status code:" << QString::asprintf("%02X", status);
                break;
            }

            if (!request->name().isEmpty())
                logInfo << "Device" << request->device()->name() << request->name().toUtf8().constData() << "finished successfully";

            break;
        }

        case RequestType::Remove:
        {
            Device device = qvariant_cast <Device> (it.value()->request());

            if (status)
                logWarning << "Device" << device->name() << "leave request failed, status code:" << QString::asprintf("%02X", status);

            if (device->removed())
                break;

            logInfo << "Device" << device->name() << "removed";
            m_devices->removeDevice(device);
            m_devices->storeDatabase();
            break;
        }

        default:
            break;
    }

    it.value()->setStatus(RequestStatus::Finished);
}

void ZigBee::deviceJoined(const QByteArray &ieeeAddress, quint16 networkAddress)
{
    auto it = m_devices->find(ieeeAddress);

    if (it == m_devices->end())
    {
        logInfo << "Device" << ieeeAddress.toHex(':') << "joined network with address" << QString::asprintf("0x%04X", networkAddress);
        it = m_devices->insert(ieeeAddress, Device(new DeviceObject(ieeeAddress, networkAddress)));
    }
    else
    {
        if (it.value()->removed())
            it.value()->setRemoved(false);

        logInfo << "Device" << it.value()->name() << "rejoined network with address" << QString::asprintf("0x%04X", networkAddress);
    }

    it.value()->updateLastSeen();
    blink(500);

    if (it.value()->networkAddress() != networkAddress)
    {
        logInfo << "Device" << it.value()->name() << "network address updated";
        it.value()->setNetworkAddress(networkAddress);
    }

    if (!it.value()->interviewFinished() && !it.value()->timer()->isActive())
    {
        logInfo << "Device" << it.value()->name() << "interview started...";
        connect(it.value()->timer(), &QTimer::timeout, this, &ZigBee::interviewTimeout);
        it.value()->timer()->setSingleShot(true);
        interviewDevice(it.value());
    }

    emit deviceEvent(it.value(), "deviceJoined");
}

void ZigBee::deviceLeft(const QByteArray &ieeeAddress)
{
    auto it = m_devices->find(ieeeAddress);

    if (it == m_devices->end() || it.value()->removed())
        return;

    it.value()->timer()->stop();
    blink(500);

    logInfo << "Device" << it.value()->name() << "left network";
    emit deviceEvent(it.value(), "deviceLeft");

    m_devices->removeDevice(it.value());
    m_devices->storeDatabase();
}

void ZigBee::nodeDescriptorReceived(quint16 networkAddress, LogicalType logicalType, quint16 manufacturerCode)
{
    Device device = m_devices->byNetwork(networkAddress);

    if (device.isNull() || device->removed())
        return;

    logInfo << "Device" << device->name() << "node descriptor received, manufacturer code is" << QString::asprintf("0x%04X", manufacturerCode) << "and logical type is" << QString(logicalType == LogicalType::Router ? "router" : "end device");

    device->setLogicalType(logicalType);
    device->setManufacturerCode(manufacturerCode);
    device->setDescriptorReceived();
    device->updateLastSeen();

    interviewDevice(device);
}

void ZigBee::activeEndpointsReceived(quint16 networkAddress, const QByteArray data)
{
    Device device = m_devices->byNetwork(networkAddress);
    QList <QString> list;

    if (device.isNull() || device->removed())
        return;

    for (int i = 0; i < data.length(); i++)
    {
        quint8 endpointId = static_cast <quint8> (data.at(i));

        if (device->endpoints().find(endpointId) == device->endpoints().end())
            device->endpoints().insert(endpointId, Endpoint(new EndpointObject(endpointId, device)));

        list.append(QString::asprintf("0x%02X", endpointId));
    }

    logInfo << "Device" << device->name() << "active endpoints received:" << list.join(", ");

    device->setEndpointsReceived();
    device->updateLastSeen();

    interviewDevice(device);
}

void ZigBee::simpleDescriptorReceived(quint16 networkAddress, quint8 endpointId, quint16 profileId, quint16 deviceId, const QList<quint16> &inClusters, const QList<quint16> &outClusters)
{
    Device device = m_devices->byNetwork(networkAddress);
    Endpoint endpoint;

    if (device.isNull() || device->removed())
        return;

    endpoint = m_devices->endpoint(device, endpointId ? endpointId : device->interviewEndpointId());
    logInfo << "Device" << device->name() << "endpoint" << QString::asprintf("0x%02X", endpoint->id()) << "simple descriptor received";

    endpoint->setProfileId(profileId);
    endpoint->setDeviceId(deviceId);
    endpoint->inClusters() = inClusters;
    endpoint->outClusters() = outClusters;
    endpoint->setDescriptorReceived();

    device->updateLastSeen();
    interviewDevice(device);
}

void ZigBee::neighborRecordReceived(quint16 networkAddress, quint16 neighborAddress, quint8 linkQuality, bool start)
{
    Device device = m_devices->byNetwork(networkAddress);

    if (device.isNull() || device->removed())
        return;

    if (start)
    {
        logInfo << "Device" << device->name() << "neighbors list received";
        device->neighbors().clear();
    }

    device->neighbors().insert(neighborAddress, linkQuality);
    device->updateLastSeen();
}

void ZigBee::messageReveived(quint16 networkAddress, quint8 endpointId, quint16 clusterId, quint8 linkQuality, const QByteArray &data)
{
    Device device = m_devices->byNetwork(networkAddress);
    Endpoint endpoint;
    quint8 frameControl = static_cast <quint8> (data.at(0)), transactionId, commandId;
    QByteArray payload;

    if (device.isNull() || device->removed())
        return;

    endpoint = m_devices->endpoint(device, endpointId);
    blink(50);

    if (frameControl & FC_MANUFACTURER_SPECIFIC)  // TODO: get manufacturerCode
    {
        transactionId = static_cast <quint8> (data.at(3));
        commandId = static_cast <quint8> (data.at(4));
        payload = data.mid(5);
    }
    else
    {
        transactionId = static_cast <quint8> (data.at(1));
        commandId = static_cast <quint8> (data.at(2));
        payload = data.mid(3);
    }

    if (frameControl & FC_CLUSTER_SPECIFIC)
        clusterCommandReceived(endpoint, clusterId, transactionId, commandId, payload);
    else
        globalCommandReceived(endpoint, clusterId, transactionId, commandId, payload);

    device->setLinkQuality(linkQuality);
    device->updateLastSeen();

    if (endpoint->updated())
    {
        m_devices->storeProperties();
        emit endpointUpdated(device, endpoint->id());
    }

    if ((frameControl & FC_CLUSTER_SPECIFIC || commandId == CMD_REPORT_ATTRIBUTES) && !(frameControl & FC_DISABLE_DEFAULT_RESPONSE))
    {
        defaultResponseStruct response;

        response.commandId = commandId;
        response.status = 0x00;

        enqueueDataRequest(device, endpoint->id(), clusterId, zclHeader(FC_SERVER_TO_CLIENT | FC_DISABLE_DEFAULT_RESPONSE, transactionId, CMD_DEFAULT_RESPONSE).append(QByteArray(reinterpret_cast <char*> (&response), sizeof(response))));
    }
}

void ZigBee::extendedMessageReveived(const QByteArray &ieeeAddress, quint8 endpointId, quint16 clusterId, quint8 linkQuality, const QByteArray &data)
{
    Q_UNUSED(endpointId)
    Q_UNUSED(linkQuality)

    if (clusterId == CLUSTER_TOUCHLINK && data.at(2) == 0x01)
    {
        logInfo << "TouchLink scan response received from device" << ieeeAddress.toHex(':') << "at channel" << m_interPanChannel;
        return;
    }

    logWarning << "Unrecognized extended message received from" << ieeeAddress.toHex(':') << "endpoint" << QString::asprintf("0x%02X", endpointId) << "cluster" << QString::asprintf("0x%04X", clusterId) << "with payload:" << data.toHex(':');
}

void ZigBee::handleRequests(void)
{
    for (auto it = m_requests.begin(); it != m_requests.end(); it++)
    {
        if (it.value()->status() != RequestStatus::Pending)
            continue;

        switch (it.value()->type())
        {
            case RequestType::Binding:
            {
                BindingRequest request = qvariant_cast <BindingRequest> (it.value()->request());

                if (!m_adapter->bindRequest(it.key(), request->device()->networkAddress(), request->device()->ieeeAddress(), request->endpointId(), request->clusterId(), request->dstAddress(), request->dstEndpointId(), request->unbind()))
                {
                    logWarning << "Device" << request->device()->name() << (request->unbind() ? "unbinding" : "binding") << "aborted";
                    it.value()->setStatus(RequestStatus::Aborted);
                }

                break;
            }

            case RequestType::Data:
            {
                DataRequest request = qvariant_cast <DataRequest> (it.value()->request());

                if (!m_adapter->dataRequest(it.key(), request->device()->networkAddress(), request->endpointId(), request->clusterId(), request->data()))
                {
                    logWarning << "Device" << request->device()->name() << (!request->name().isEmpty() ? request->name().toUtf8().constData() : "data request") << "aborted";
                    it.value()->setStatus(RequestStatus::Aborted);
                }

                break;
            }

            case RequestType::Remove:
            {
                Device device = qvariant_cast <Device> (it.value()->request());

                if (!m_adapter->leaveRequest(it.key(), device->networkAddress(), device->ieeeAddress()))
                {
                    logWarning << "Device" << device->name() << "leave request aborted";
                    it.value()->setStatus(RequestStatus::Aborted);
                }

                break;
            }

            case RequestType::LQI:
            {
                if (!m_adapter->lqiRequest(it.key(), qvariant_cast <Device> (it.value()->request())->networkAddress()))
                    it.value()->setStatus(RequestStatus::Aborted);

                break;
            }

            case RequestType::Interview:
            {
                if (!interviewRequest(it.key(), qvariant_cast <Device> (it.value()->request())))
                    it.value()->setStatus(RequestStatus::Aborted);

                break;
            }
        }

        if (it.value()->status() == RequestStatus::Finished || it.value()->status() == RequestStatus::Aborted)
            continue;

        it.value()->setStatus(RequestStatus::Sent);
    }

    for (auto it = m_requests.begin(); it != m_requests.end(); it++)
    {
        if (it.value()->status() == RequestStatus::Finished || it.value()->status() == RequestStatus::Aborted)
            m_requests.erase(it++);

        if (it == m_requests.end())
            break;
    }

    m_requestTimer->stop();
}

void ZigBee::updateNeighbors(void)
{
    for (auto it = m_devices->begin(); it != m_devices->end(); it++)
    {
        if (it.value()->logicalType() == LogicalType::EndDevice)
            continue;

        enqueueDeviceRequest(it.value(), RequestType::LQI);
    }
}

void ZigBee::interviewTimeout(void)
{
    Device device = m_devices->value(reinterpret_cast <DeviceObject*> (sender()->parent())->ieeeAddress());
    logWarning << "Device" << device->name() << "interview timed out";
    emit deviceEvent(device, "interviewTimeout");
}

void ZigBee::pollRequest(EndpointObject *endpoint, const Poll &poll)
{
    enqueueDataRequest(endpoint->device(), endpoint->id(), poll->clusterId(), attributesRequest(m_requestId, poll->attributes()));
}

void ZigBee::updateStatusLed(void)
{
    GPIO::setStatus(m_statusLedPin, !GPIO::getStatus(m_statusLedPin));
}

void ZigBee::updateBlinkLed(void)
{
    GPIO::setStatus(m_blinkLedPin, false);
}
