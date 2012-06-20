#ifndef CONNECTION_H
#define CONNECTION_H

#include "Messages.h"
#include <QObject>
#include <QMetaObject>
#include <QMetaMethod>
#include <QString>
#include <ByteArray.h>
#include <Map.h>

class QLocalSocket;
class ConnectionPrivate;

class Connection : public QObject
{
    Q_OBJECT
public:
    enum { Port = 18414 };

    Connection(QObject *parent = 0);
    Connection(QLocalSocket *socket, QObject *parent = 0);

    bool connectToServer(const QString &name);

    int pendingWrite() const;

    template<typename T>
    void send(const T *message);
    void send(int id, const ByteArray& message);

    template<typename T>
    static bool registerMessage();
public slots:
    void finish();
signals:
    void connected();
    void disconnected();
    void error();
    void newMessage(Message *message);
    void sendComplete();

private:
    ConnectionPrivate *mPriv;

    struct Meta
    {
        const QMetaObject *meta;
        int fromByteArrayId;
    };
    static Map<int, Meta> sMetas;

    friend class ConnectionPrivate;
};

template<typename T>
void Connection::send(const T *message)
{
    send(T::MessageId, message->toByteArray());
}

template<typename T>
bool Connection::registerMessage()
{
    const int id = T::MessageId;
    if (sMetas.contains(id))
        return true;
    const QMetaObject *obj = &T::staticMetaObject;

    if (!obj->constructorCount()
        || obj->constructor(0).parameterTypes().size() != 1
        || obj->constructor(0).parameterTypes().front() != "QObject*") {
        qWarning("no constructor");
        return false;
    }

    int fromByteArrayId = -1;
    for (int i = obj->methodOffset(); i < obj->methodCount(); ++i) {
        if (!qstrcmp(obj->method(i).signature(), "fromByteArray(ByteArray)"))
            fromByteArrayId = i;
    }

    if (fromByteArrayId == -1) {
        qWarning("no fromByteArray method");
        return false;
    }

    Q_ASSERT(!sMetas.contains(id));
    Meta m;
    m.meta = obj;
    m.fromByteArrayId = fromByteArrayId;
    sMetas[id] = m;

    return true;
}

#endif // CONNECTION_H