#include "TcpTransfer.h"
#include <QCryptographicHash>
#include "Other.h"

static const quint8 Direction_In = 1;
static const quint8 Direction_Out = 2;

static const int SocketMaxWaitingSize = 1024 * 1024;
static const int SocketReadBufferSize = 128 * 1024;

static const int GlobalMaxWaitingSize = 2 * 1024 * 1024;

static const int DataStreamMaxDataSize = 32768;

static inline quint8 getOppositeDirection(quint8 direction)
{
	switch (direction)
	{
	case Direction_In:
		return Direction_Out;
	case Direction_Out:
		return Direction_In;
	default:
		return 0;
	}
}

static const int HeaderSize = 4;

#pragma pack(push)
#pragma pack(1)

struct AddTransferFrame
{
	quint16 localPort;
	quint16 remoteDestPort;
	char remoteDestAddressText[40];
};

struct DeleteTransferFrame
{
	quint16 localPort;
};

struct NewConnectionFrame
{
	quint16 localPort;
	qint64 socketDescriptor;
};

struct DisconnectConnectionFrame
{
	qint64 socketDescriptor;
	quint8 direction;
};

struct DataStreamFrame
{
	qint64 socketDescriptor;
	quint8 direction;
	char data[0];
};

struct AckFrame
{
	qint64 socketDescriptor;
	quint8 direction;
	int writtenSize;
};

#pragma pack(pop)

TcpTransfer::TcpTransfer(QObject *parent)
	: QObject(parent)
{
	m_timer15s.setParent(this);
	connect(&m_timer15s, SIGNAL(timeout()), this, SLOT(timerFunction15s()));
	m_lastOutTime = QTime::currentTime();
	m_timer15s.start(15 * 1000);
}

TcpTransfer::~TcpTransfer()
{
	m_timer15s.stop();
	for (SocketOutInfo socketOut : m_mapSocketOut.values())
		delete socketOut.obj;
	m_mapSocketOut.clear();
	for (SocketInInfo socketIn : m_mapSocketIn.values())
		delete socketIn.obj;
	m_mapSocketIn.clear();
	for (QTcpServer * tcpServer : m_mapTcpServer)
		delete tcpServer;
	m_mapTcpServer.clear();
}

void TcpTransfer::dataInput(QByteArray package)
{
	m_buffer.append(package);
	while (m_buffer.size() >= HeaderSize)
	{
		const FrameType frameType = (FrameType)*(quint16*)m_buffer.constData();
		const int frameSize = *(quint16*)(m_buffer.constData() + 2);
		if(m_buffer.size() < frameSize + HeaderSize)
			break;
		if (isValidFrameType(frameType))
		{
			const QByteArray frame(m_buffer.constData() + HeaderSize, frameSize);
			dealFrame(frameType, frame);
		}
		m_buffer = m_buffer.mid(frameSize + HeaderSize);
	}
}

bool TcpTransfer::addTransfer(quint16 localPort, quint16 remoteDestPort, QHostAddress remoteDestAddress)
{
	if (m_mapTransferOut.contains(localPort))
		return false;
	QTcpServer * tcpServer = new QTcpServer();
	if (!tcpServer || !tcpServer->listen(QHostAddress::Any, localPort))
	{
		delete tcpServer;
		return false;
	}
	m_mapTcpServer[localPort] = tcpServer;
	m_mapTransferOut[localPort] = Peer(remoteDestAddress, remoteDestPort);
	connect(tcpServer, SIGNAL(newConnection()), this, SLOT(onTcpNewConnection()));
	output_AddTransfer(localPort, remoteDestPort, remoteDestAddress.toString());
	return true;
}

bool TcpTransfer::deleteTransfer(quint16 localPort)
{
	if (m_mapTransferOut.remove(localPort) == 0)
		return false;

	QTcpServer * tcpServer = m_mapTcpServer.value(localPort);
	if (tcpServer)
	{
		delete tcpServer;
		m_mapTcpServer.remove(localPort);
	}

	output_DeleteTransfer(localPort);
	return true;
}

bool TcpTransfer::isValidFrameType(FrameType frameType)
{
	return frameType > BeginUnknownFrameType && frameType < EndUnknownFrameType;
}

TcpTransfer::SocketInInfo * TcpTransfer::findSocketIn(const qint64 & peerSocketDescriptor)
{
	auto iter = m_mapSocketIn.find(peerSocketDescriptor);
	if (iter != m_mapSocketIn.end())
		return &(iter.value());
	else
		return nullptr;
}

TcpTransfer::SocketOutInfo * TcpTransfer::findSocketOut(const qint64 & socketDescriptor)
{
	auto iter = m_mapSocketOut.find(socketDescriptor);
	if (iter != m_mapSocketOut.end())
		return &(iter.value());
	else
		return nullptr;
}

void TcpTransfer::dealFrame(FrameType type, const QByteArray & frameData)
{
	if (type == HeartBeatType)
	{
		input_heartBeat();
	}else if (type == AddTransferType)
	{
		if (frameData.size() < sizeof(AddTransferFrame))
			return;
		const AddTransferFrame * frame = (const AddTransferFrame*)frameData.constData();
		input_AddTransfer(frame->localPort, frame->remoteDestPort, frame->remoteDestAddressText);
	}
	else if (type == DeleteTransferType)
	{
		if (frameData.size() < sizeof(DeleteTransferFrame))
			return;
		const DeleteTransferFrame * frame = (const DeleteTransferFrame*)frameData.constData();
		input_DeleteTransfer(frame->localPort);
	}
	else if (type == NewConnectionType)
	{
		if (frameData.size() < sizeof(NewConnectionFrame))
			return;
		const NewConnectionFrame * frame = (const NewConnectionFrame*)frameData.constData();
		input_NewConnection(frame->localPort, frame->socketDescriptor);
	}
	else if (type == DisconnectConnectionType)
	{
		if (frameData.size() < sizeof(DisconnectConnectionFrame))
			return;
		const DisconnectConnectionFrame * frame = (const DisconnectConnectionFrame*)frameData.constData();
		input_DisconnectConnection(frame->socketDescriptor, frame->direction);
	}
	else if (type == DataStreamType)
	{
		if (frameData.size() < sizeof(DataStreamFrame))
			return;
		const DataStreamFrame * frame = (const DataStreamFrame*)frameData.constData();
		input_DataStream(frame->socketDescriptor, frame->direction, frame->data, frameData.size() - sizeof(DataStreamFrame));
	}
	else if (type == AckType)
	{
		if (frameData.size() < sizeof(AckType))
			return;
		const AckFrame * frame = (const AckFrame*)frameData.constData();
		input_Ack(frame->socketDescriptor, frame->direction, frame->writtenSize);
	}
}

void TcpTransfer::input_heartBeat()
{

}

void TcpTransfer::input_AddTransfer(quint16 localPort, quint16 remoteDestPort, QString remoteDestAddressText)
{
	if (remoteDestAddressText.isEmpty())
		return;
	QHostAddress remoteDestAddress(remoteDestAddressText);
	if (localPort && remoteDestPort && !remoteDestAddress.isNull())
		m_mapTransferIn[localPort] = Peer(remoteDestAddress, remoteDestPort);
}

void TcpTransfer::input_DeleteTransfer(quint16 localPort)
{
	if (localPort)
		m_mapTransferIn.remove(localPort);
}

void TcpTransfer::input_NewConnection(quint16 localPort, qint64 socketDescriptor)
{
	if (localPort && socketDescriptor && m_mapTransferIn.contains(localPort))
	{
		const Peer peer = m_mapTransferIn.value(localPort);
		Q_ASSERT(!m_mapSocketIn.contains(socketDescriptor));
		SocketInInfo & socketIn = m_mapSocketIn[socketDescriptor];
		if (socketIn.obj)
			delete socketIn.obj;
		socketIn.cachedData.clear();

		socketIn.obj = new QTcpSocket();
		socketIn.obj->setReadBufferSize(SocketReadBufferSize);
		socketIn.peerSocketDescriptor = socketDescriptor;
		connect(socketIn.obj, SIGNAL(stateChanged(QAbstractSocket::SocketState)),
			this, SLOT(onSocketInStateChanged(QAbstractSocket::SocketState)));
		connect(socketIn.obj, SIGNAL(readyRead()), this, SLOT(onSocketInReadyRead()));
		connect(socketIn.obj, SIGNAL(bytesWritten(qint64)), this, SLOT(onSocketInBytesWritten(qint64)));
		socketIn.obj->setProperty("peerSocketDescriptor", socketDescriptor);
		socketIn.obj->connectToHost(peer.address, peer.port);
	}
	qDebug() << QString("TcpTransfer::input_NewConnection localPort=%1 socketDescriptor=%2")
		.arg(localPort).arg(socketDescriptor);
}

void TcpTransfer::input_DisconnectConnection(qint64 socketDescriptor, quint8 direction)
{
	direction = getOppositeDirection(direction);
	if (direction == Direction_Out)
	{
		qDebug() << QString("TcpTransfer::input_DisconnectConnection socketOut disconnected socketDescriptor=%1")
			.arg(socketDescriptor);

		if (SocketOutInfo * socketOut = findSocketOut(socketDescriptor))
			delete socketOut->obj;
		m_lstGlobalWaitingSocket.removeOne(SocketIdentifier(socketDescriptor, Direction_Out));
	}
	else if (direction == Direction_In)
	{
		const qint64 & peerSocketDescriptor = socketDescriptor;
		qDebug() << QString("TcpTransfer::input_DisconnectConnection socketIn disconnected peerSocketDescriptor=%1")
			.arg(peerSocketDescriptor);

		if (SocketInInfo * socketIn = findSocketIn(peerSocketDescriptor))
			delete socketIn->obj;
		m_lstGlobalWaitingSocket.removeOne(SocketIdentifier(socketDescriptor, Direction_In));
	}
}

void TcpTransfer::input_DataStream(qint64 socketDescriptor, quint8 direction, const char * data, int dataSize)
{
	direction = getOppositeDirection(direction);
	if (direction == Direction_Out)
	{
		if (SocketOutInfo * socketOut = findSocketOut(socketDescriptor))
			socketOut->obj->write(data, dataSize);
	}
	else if (direction == Direction_In)
	{
		const qint64 & peerSocketDescriptor = socketDescriptor;
		if (SocketInInfo * socketIn = findSocketIn(peerSocketDescriptor))
		{
			if (socketIn->obj->state() == QAbstractSocket::ConnectedState)
				socketIn->obj->write(data, dataSize);
			else
				socketIn->cachedData.append(data, dataSize);
		}
	}
	output_Ack(0, 0, dataSize);
}

void TcpTransfer::input_Ack(qint64 socketDescriptor, quint8 direction, int writtenSize)
{
	if (direction == 0 && socketDescriptor == 0)
	{
		// 这是全局流控
		m_globalWaitingSize -= writtenSize;
		if (m_globalWaitingSize < 0)
		{
			m_globalWaitingSize = 0;
			qWarning() << QString("TcpTransfer::input_Ack m_globalWaitingSize < 0");
		}
		while (m_globalWaitingSize < GlobalMaxWaitingSize && m_lstGlobalWaitingSocket.size() > 0)
		{
			const SocketIdentifier & socketIdentifier = m_lstGlobalWaitingSocket.first();
			if(socketIdentifier.direction == Direction_In)
			{ 
				if (SocketInInfo * socketIn = findSocketIn(socketIdentifier.peerSocketDescriptor))
					readAndSendSocketIn(*socketIn);
			}
			else if (socketIdentifier.direction == Direction_Out)
			{
				if (SocketOutInfo * socketOut = findSocketOut(socketIdentifier.socketDescriptor))
					readAndSendSocketOut(*socketOut);
			}
			m_lstGlobalWaitingSocket.removeFirst();
		}
	}
	else
	{
		direction = getOppositeDirection(direction);
		if (direction == Direction_Out)
		{
			if (SocketOutInfo * socketOut = findSocketOut(socketDescriptor))
			{
				socketOut->peerWaitingSize -= writtenSize;
				if (socketOut->peerWaitingSize < 0)
				{
					socketOut->peerWaitingSize = 0;
					qWarning() << QString("TcpTransfer::input_Ack out peerWaitingSize < 0, socketDescriptor=%1")
						.arg(socketDescriptor);
				}
				readAndSendSocketOut(*socketOut);
			}
		}
		else if (direction == Direction_In)
		{
			const qint64 & peerSocketDescriptor = socketDescriptor;
			if (SocketInInfo * socketIn = findSocketIn(peerSocketDescriptor))
			{
				socketIn->peerWaitingSize -= writtenSize;
				if (socketIn->peerWaitingSize < 0)
				{
					socketIn->peerWaitingSize = 0;
					qWarning() << QString("TcpTransfer::input_Ack in peerWaitingSize < 0, peerSocketDescriptor=%1")
						.arg(peerSocketDescriptor);
				}
				readAndSendSocketIn(*socketIn);
			}
		}
	}
}

void TcpTransfer::outputFrame(FrameType type, const QByteArray & frameData, const char * extraData, int extraDataSize)
{
	if (extraDataSize < 0)
		extraDataSize = 0;

	const int totalSize = frameData.size() + extraDataSize;
	if (totalSize > 65535)
	{
		qCritical() << QString("TcpTransfer::outputFrame totalSize %1 too big").arg(totalSize);
		return;
	}

	quint16 header[2] = { 0 };
	header[0] = (quint16)type;
	header[1] = (quint16)totalSize;

	QByteArray package = frameData;
	package.insert(0, QByteArray::fromRawData((const char*)&header, sizeof(header)));

	emit dataOutput(package);
	if (extraDataSize > 0)
		emit dataOutput(QByteArray::fromRawData(extraData, extraDataSize));

	m_lastOutTime = QTime::currentTime();
}

void TcpTransfer::output_heartBeat()
{
	outputFrame(HeartBeatType, QByteArray());
}

void TcpTransfer::output_AddTransfer(quint16 localPort, quint16 remoteDestPort, QString remoteDestAddressText)
{
	AddTransferFrame frame = { 0 };
	frame.localPort = localPort;
	frame.remoteDestPort = remoteDestPort;
	strcpy_(frame.remoteDestAddressText, remoteDestAddressText.toUtf8().constData());
	outputFrame(AddTransferType, QByteArray::fromRawData((const char*)&frame, sizeof(frame)));
}

void TcpTransfer::output_DeleteTransfer(quint16 localPort)
{
	DeleteTransferFrame frame;
	frame.localPort = localPort;
	outputFrame(DeleteTransferType, QByteArray::fromRawData((const char*)&frame, sizeof(frame)));
}

void TcpTransfer::output_NewConnection(quint16 localPort, qint64 socketDescriptor)
{
	NewConnectionFrame frame;
	frame.localPort = localPort;
	frame.socketDescriptor = socketDescriptor;
	outputFrame(NewConnectionType, QByteArray::fromRawData((const char*)&frame, sizeof(frame)));
}

void TcpTransfer::output_DisconnectConnection(qint64 socketDescriptor, quint8 direction)
{
	DisconnectConnectionFrame frame;
	frame.socketDescriptor = socketDescriptor;
	frame.direction = direction;
	outputFrame(DisconnectConnectionType, QByteArray::fromRawData((const char*)&frame, sizeof(frame)));
}

void TcpTransfer::output_DataStream(qint64 socketDescriptor, quint8 direction, const char * data, int dataSize)
{
	DataStreamFrame frame;
	frame.socketDescriptor = socketDescriptor;
	frame.direction = direction;
	outputFrame(DataStreamType, QByteArray::fromRawData((const char*)&frame, sizeof(frame)), data, dataSize);
}

void TcpTransfer::output_Ack(qint64 socketDescriptor, quint8 direction, int writtenSize)
{
	AckFrame frame;
	frame.socketDescriptor = socketDescriptor;
	frame.direction = direction;
	frame.writtenSize = writtenSize;
	outputFrame(AckType, QByteArray::fromRawData((const char*)&frame, sizeof(frame)));
}

int TcpTransfer::readAndSendSocketOut(SocketOutInfo & socketOut)
{
	int totalSendSize = 0;
	while (1)
	{
		const int availableSize = socketOut.obj->bytesAvailable();
		if (availableSize == 0)
			break;
		const int singleMaxSendSize = qMin(SocketMaxWaitingSize - socketOut.peerWaitingSize, DataStreamMaxDataSize);
		if (singleMaxSendSize <= 0)
			break;
		const int globalMaxSendSize = GlobalMaxWaitingSize - m_globalWaitingSize;
		// 如果全局流控允许发送的数量比自身流控更少，并且availableSize大于全局流控允许的数量，就认为这次的限制发送是全局流控导致的
		if (globalMaxSendSize < singleMaxSendSize && availableSize > globalMaxSendSize)
		{
			SocketIdentifier socketIdentifier(socketOut.socketDescriptor, Direction_Out);
			if (!m_lstGlobalWaitingSocket.contains(socketIdentifier))
			{
				m_lstGlobalWaitingSocket << socketIdentifier;
				qWarning() << QString("TcpTransfer::readAndSendSocketOut %1 global waiting").arg(socketOut.socketDescriptor);
			}
		}
		const int maxSendSize = qMin(globalMaxSendSize, singleMaxSendSize);
		if (maxSendSize <= 0)
			break;

		QByteArray bytes = socketOut.obj->read(maxSendSize);
		socketOut.peerWaitingSize += bytes.size();
		m_globalWaitingSize += bytes.size();
		output_DataStream(socketOut.socketDescriptor, Direction_Out, bytes.constData(), bytes.size());
		totalSendSize += bytes.size();

		// 发送数量小于单次最大，说明要么全部发完，要么受到了流控
		if(bytes.size() < DataStreamMaxDataSize)
			break;
	}
	return totalSendSize;
}

int TcpTransfer::readAndSendSocketIn(SocketInInfo & socketIn)
{
	int totalSendSize = 0;
	while (1)
	{
		const int availableSize = socketIn.obj->bytesAvailable();
		if (availableSize == 0)
			break;
		const int singleMaxSendSize = qMin(SocketMaxWaitingSize - socketIn.peerWaitingSize, DataStreamMaxDataSize);
		if (singleMaxSendSize <= 0)
			break;
		const int globalMaxSendSize = GlobalMaxWaitingSize - m_globalWaitingSize;
		if (globalMaxSendSize < singleMaxSendSize && availableSize > globalMaxSendSize)
		{
			SocketIdentifier socketIdentifier(socketIn.peerSocketDescriptor, Direction_Out);
			if (!m_lstGlobalWaitingSocket.contains(socketIdentifier))
			{
				m_lstGlobalWaitingSocket << socketIdentifier;
				qWarning() << QString("TcpTransfer::readAndSendSocketIn %1 global waiting").arg(socketIn.peerSocketDescriptor);
			}
		}
		const int maxSendSize = qMin(globalMaxSendSize, singleMaxSendSize);
		if (maxSendSize <= 0)
			break;

		QByteArray bytes = socketIn.obj->read(maxSendSize);
		socketIn.peerWaitingSize += bytes.size();
		m_globalWaitingSize += bytes.size();
		output_DataStream(socketIn.peerSocketDescriptor, Direction_In, bytes.constData(), bytes.size());
		totalSendSize += bytes.size();

		if (bytes.size() < DataStreamMaxDataSize)
			break;
	}
	return totalSendSize;
}

void TcpTransfer::timerFunction15s()
{
	if (!m_lastOutTime.isValid() || m_lastOutTime.elapsed() > 30 * 1000)
		output_heartBeat();
}

void TcpTransfer::onSocketInStateChanged(QAbstractSocket::SocketState state)
{
	QTcpSocket * tcpSocket = (QTcpSocket*)sender();
	if (!tcpSocket)
		return;
	if (state == QAbstractSocket::UnconnectedState)
	{
		// UnconnectedState状态表示连接失败或断开连接
		const qint64 peerSocketDescriptor = tcpSocket->property("peerSocketDescriptor").toLongLong();
		if (SocketInInfo * socketIn = findSocketIn(peerSocketDescriptor))
		{
			socketIn->obj->deleteLater();
			socketIn->obj = nullptr;
			m_mapSocketIn.remove(peerSocketDescriptor);
			output_DisconnectConnection(peerSocketDescriptor, Direction_In);
		}
		m_lstGlobalWaitingSocket.removeOne(SocketIdentifier(peerSocketDescriptor, Direction_In));
		qDebug() << QString("TcpTransfer::onSocketInStateChanged disconnected peerSocketDescriptor=%1").arg(peerSocketDescriptor);
	}
	else if (state == QAbstractSocket::ConnectedState)
	{
		const qint64 peerSocketDescriptor = tcpSocket->property("peerSocketDescriptor").toLongLong();
		SocketInInfo * socketIn = findSocketIn(peerSocketDescriptor);
		if (!socketIn)
			return;
		if (socketIn->cachedData.size() > 0)
		{
			socketIn->obj->write(socketIn->cachedData);
			socketIn->cachedData.clear();
		}
		qDebug() << QString("TcpTransfer::onSocketInStateChanged connected peerSocketDescriptor=%1").arg(peerSocketDescriptor);
	}
}

void TcpTransfer::onSocketOutStateChanged(QAbstractSocket::SocketState state)
{
	QTcpSocket * socketOut = (QTcpSocket*)sender();
	if (!socketOut)
		return;
	if (state == QAbstractSocket::UnconnectedState)
	{
		const qint64 socketDescriptor = socketOut->property("socketDescriptor").toLongLong();
		if (m_mapSocketOut.contains(socketDescriptor))
		{
			m_mapSocketOut.remove(socketDescriptor);
			socketOut->deleteLater();
			output_DisconnectConnection(socketDescriptor, Direction_Out);
		}
		m_lstGlobalWaitingSocket.removeOne(SocketIdentifier(socketDescriptor, Direction_Out));
		qDebug() << QString("TcpTransfer::onSocketOutStateChanged disconnected socketDescriptor=%1").arg(socketDescriptor);
	}
}

void TcpTransfer::onTcpNewConnection()
{
	QTcpServer * tcpServer = (QTcpServer*)sender();
	if (!tcpServer)
		return;
	const quint16 localPort = tcpServer->serverPort();
	while (tcpServer->hasPendingConnections())
	{
		QTcpSocket * socketOutObj = tcpServer->nextPendingConnection();
		const qint64 socketDescriptor = socketOutObj->socketDescriptor();

		Q_ASSERT(!m_mapSocketOut.contains(socketDescriptor));
		SocketOutInfo & socketOut = m_mapSocketOut[socketDescriptor];
		if (socketOut.obj)
			delete socketOut.obj;

		socketOut.obj = socketOutObj;
		socketOut.socketDescriptor = socketDescriptor;

		socketOut.obj->setReadBufferSize(SocketReadBufferSize);
		socketOut.obj->setProperty("localPort", localPort);
		socketOut.obj->setProperty("socketDescriptor", socketDescriptor);
		connect(socketOut.obj, SIGNAL(stateChanged(QAbstractSocket::SocketState)),
			this, SLOT(onSocketOutStateChanged(QAbstractSocket::SocketState)));
		connect(socketOut.obj, SIGNAL(readyRead()), this, SLOT(onSocketOutReadyRead()));
		connect(socketOut.obj, SIGNAL(bytesWritten(qint64)), this, SLOT(onSocketOutBytesWritten(qint64)));
		output_NewConnection(localPort, socketDescriptor);

		qDebug() << QString("TcpTransfer::onTcpNewConnection localPort=%1 socketDescriptor=%2")
			.arg(localPort).arg(socketDescriptor);
	}
}

void TcpTransfer::onSocketOutReadyRead()
{
	QTcpSocket * socketOutObj = (QTcpSocket*)sender();
	if (!socketOutObj)
		return;
	const qint64 socketDescriptor = socketOutObj->socketDescriptor();
	if (SocketOutInfo * socketOut = findSocketOut(socketDescriptor))
	{
		readAndSendSocketOut(*socketOut);
	}
	else
	{
		qCritical() << QString("TcpTransfer::onSocketOutReadyRead unknown socketDescriptor %1").arg(socketDescriptor);
		socketOutObj->deleteLater();
	}	
}

void TcpTransfer::onSocketInReadyRead()
{
	QTcpSocket * socketInObj = (QTcpSocket*)sender();
	if (!socketInObj)
		return;
	const qint64 peerSocketDescriptor = socketInObj->property("peerSocketDescriptor").toLongLong();

	if (SocketInInfo * socketIn = findSocketIn(peerSocketDescriptor))
	{
		readAndSendSocketIn(*socketIn);
	}
	else
	{
		qCritical() << QString("TcpTransfer::onSocketOutReadyRead unknown peerSocketDescriptor %1").arg(peerSocketDescriptor);
		socketInObj->deleteLater();
	}
}

void TcpTransfer::onSocketOutBytesWritten(qint64 size)
{
	QTcpSocket * socketOut = (QTcpSocket*)sender();
	if (!socketOut)
		return;
	const qint64 socketDescriptor = socketOut->socketDescriptor();
	if (socketDescriptor == 0)
		return;
	output_Ack(socketDescriptor, Direction_Out, size);
}

void TcpTransfer::onSocketInBytesWritten(qint64 size)
{
	QTcpSocket * socketIn = (QTcpSocket*)sender();
	if (!socketIn)
		return;
	const qint64 peerSocketDescriptor = socketIn->property("peerSocketDescriptor").toLongLong();
	if (peerSocketDescriptor == 0)
		return;
	output_Ack(peerSocketDescriptor, Direction_In, size);
}
