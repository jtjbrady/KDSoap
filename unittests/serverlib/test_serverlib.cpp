/****************************************************************************
**
** This file is part of the KD Soap project.
**
** SPDX-FileCopyrightText: 2010 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>
**
** SPDX-License-Identifier: MIT
**
****************************************************************************/

#include "KDSoapAuthentication.h"
#include "KDSoapClientInterface.h"
#include "KDSoapMessage.h"
#include "KDSoapNamespaceManager.h"
#include "KDSoapPendingCallWatcher.h"
#include "KDSoapServer.h"
#include "KDSoapServerAuthInterface.h"
#include "KDSoapServerCustomVerbRequestInterface.h"
#include "KDSoapServerObjectInterface.h"
#include "KDSoapServerRawXMLInterface.h"
#include "KDSoapThreadPool.h"
#include "KDSoapValue.h"
#include "httpserver_p.h" // KDSoapUnitTestHelpers
#include <QAuthenticator>
#include <QDebug>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTest>
#ifndef QT_NO_OPENSSL
#include <QSslConfiguration>
#endif
#include <QSignalSpy>
#include <QTimer>
using namespace KDSoapUnitTestHelpers;

Q_DECLARE_METATYPE(QFile::Permissions)

static const char *myWsdlNamespace = "http://www.kdab.com/xml/MyWsdl/";

class CountryServerObject;
typedef QList<CountryServerObject *> ServerObjectsList;
ServerObjectsList s_serverObjects;
QMutex s_serverObjectsMutex;

class PublicThread : public QThread
{
public:
    using QThread::msleep;
};

static const char s_longEmployeeName[] = "This is a long string in order to test chunking in this test";
static QByteArray rawCountryMessage(const QByteArray &employeeName = "David Ä Faure", const KDSoapClientInterface::SoapVersion soapVersion = KDSoapClientInterface::SoapVersion::SOAP1_1)
{
    switch (soapVersion) {
    case KDSoapClientInterface::SoapVersion::SOAP1_1:
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "xmlns:soap-enc=\"http://schemas.xmlsoap.org/soap/encoding/\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
               "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><soap:Body><n1:getEmployeeCountry "
               "xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\"><employeeName>"
            + employeeName + "</employeeName></n1:getEmployeeCountry></soap:Body></soap:Envelope>";
    case KDSoapClientInterface::SoapVersion::SOAP1_2:
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:soap-enc=\"http://www.w3.org/2003/05/soap-encoding\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
               "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><soap:Body><n1:getEmployeeCountry "
               "xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\"><employeeName>"
            + employeeName + "</employeeName></n1:getEmployeeCountry></soap:Body></soap:Envelope>";
    default:
        return "<error/>"; // Returning nothing results in the test hanging.
    }
}
static QByteArray expectedCountryResponse(const QByteArray &employeeName = "David Ä Faure", const KDSoapClientInterface::SoapVersion soapVersion = KDSoapClientInterface::SoapVersion::SOAP1_1)
{
    switch (soapVersion) {
    case KDSoapClientInterface::SoapVersion::SOAP1_1:
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
               "xmlns:soap-enc=\"http://schemas.xmlsoap.org/soap/encoding/\"><soap:Body><n1:getEmployeeCountry "
               "xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\"><employeeCountry>"
            + employeeName + " France</employeeCountry>getEmployeeCountryResponse</n1:getEmployeeCountry></soap:Body></soap:Envelope>\n";
    case KDSoapClientInterface::SoapVersion::SOAP1_2:
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:soap-enc=\"http://www.w3.org/2003/05/soap-encoding\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" "
               "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><soap:Body><n1:getEmployeeCountry "
               "xmlns:n1=\"http://www.kdab.com/xml/MyWsdl/\"><employeeCountry>"
            + employeeName + " France</employeeCountry>getEmployeeCountryResponse</n1:getEmployeeCountry></soap:Body></soap:Envelope>\n";
    default:
        return "<error/>";
    }
}

class CountryServerObject : public QObject,
                            public KDSoapServerObjectInterface,
                            public KDSoapServerAuthInterface,
                            public KDSoapServerRawXMLInterface,
                            public KDSoapServerCustomVerbRequestInterface
{
    Q_OBJECT
    Q_INTERFACES(KDSoapServerObjectInterface)
    Q_INTERFACES(KDSoapServerAuthInterface)
    Q_INTERFACES(KDSoapServerRawXMLInterface)
    Q_INTERFACES(KDSoapServerCustomVerbRequestInterface)
public:
    CountryServerObject(bool auth, bool rawXML)
        : QObject()
        , KDSoapServerObjectInterface()
        , m_requireAuth(auth)
        , m_useRawXML(rawXML)
        , m_rawXMLValid(false)
    {
        // qDebug() << "Server object created in thread" << QThread::currentThread();
        QMutexLocker locker(&s_serverObjectsMutex);
        s_serverObjects.append(this);
    }
    ~CountryServerObject()
    {
        QMutexLocker locker(&s_serverObjectsMutex);
        Q_ASSERT(s_serverObjects.contains(this));
        s_serverObjects.removeOne(this);
    }

    virtual void processRequest(const KDSoapMessage &request, KDSoapMessage &response, const QByteArray &soapAction) override;

    virtual QIODevice *processFileRequest(const QString &path, QByteArray &contentType) override
    {
        Q_ASSERT(!path.startsWith(".."));
        if (path == QLatin1String("/path/to/file_download.txt")) {
            QFile *file = new QFile(QLatin1String("file_download.txt")); // local file, created by the unittest
            contentType = "text/plain";
            return file; // will be deleted by KDSoap
        }
        return 0;
    }

    virtual bool validateAuthentication(const KDSoapAuthentication &auth, const QString &path) override
    {
        if (!m_requireAuth) {
            return true;
        }

        if ((path == QLatin1String("/") || path == QLatin1String("/path/to/file_download.txt")) && auth.user() == QLatin1String("kdab")) {
            return auth.password() == QLatin1String("pass42");
        }
        return false;
    }

    // KDSoapServerRawXMLInterface interface
    bool newRequest(const QByteArray &requestType, const QMap<QByteArray, QByteArray> &httpHeaders) override
    {
        if (m_useRawXML && requestType == "POST") {
            if (!httpHeaders.contains("content-type") || !httpHeaders.contains("soapaction")) {
                m_rawXMLValid = false;
                qWarning() << "Didn't get all expected headers:" << httpHeaders;
            } else {
                m_rawXMLValid = true;
            }
            return true;
        }
        return false;
    }
    void processXML(const QByteArray &xmlChunk) override
    {
        if (!m_useRawXML) { // should never happen
            Q_ASSERT(m_useRawXML);
            m_rawXMLValid = false;
        }
        m_assembledXML += xmlChunk;
    }
    void endRequest() override
    {
        if (m_assembledXML != rawCountryMessage(s_longEmployeeName)) {
            qWarning() << "Expected" << rawCountryMessage(s_longEmployeeName) << "\nGot" << m_assembledXML;
            m_rawXMLValid = false;
        }
        if (m_rawXMLValid) {
            writeXML(expectedCountryResponse(s_longEmployeeName));
        } else {
            writeHTTP("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        }
        m_assembledXML.clear();
    }

    // KDSoapServerCustomVerbRequestInterface
    virtual bool processCustomVerbRequest(const QByteArray &requestType, const QByteArray &requestData,
                                          const QMap<QByteArray, QByteArray> &httpHeaders, QByteArray &customAnswer) override
    {
        Q_UNUSED(requestData);
        Q_UNUSED(httpHeaders);
        if (requestType == "PULL") {
            customAnswer = "HTTP/1.1 200 OK\r\n";
            customAnswer += "Content-Length: 11\r\n";
            customAnswer += "\r\n";
            customAnswer += "Hello world";

            return true;
        }

        return false;
    }

    virtual HttpResponseHeaderItems additionalHttpResponseHeaderItems() const override
    {
        static KDSoapServerObjectInterface::HttpResponseHeaderItems result = KDSoapServerObjectInterface::HttpResponseHeaderItems()
            << KDSoapServerObjectInterface::HttpResponseHeaderItem("Access-Control-Allow-Origin", "*")
            << KDSoapServerObjectInterface::HttpResponseHeaderItem("Access-Control-Allow-Headers", "Content-Type");
        return result;
    }

public: // SOAP-accessible methods
    QString getEmployeeCountry(const QString &employeeName)
    {
        // Should be called in same thread as constructor
        s_serverObjectsMutex.lock();
        Q_ASSERT(s_serverObjects.contains(this));
        s_serverObjectsMutex.unlock();
        if (employeeName.isEmpty()) {
            setFault(QLatin1String("Client.Data"), QLatin1String("Empty employee name"), QLatin1String("CountryServerObject"),
                     tr("Employee name must not be empty"));
            return QString();
        }
        // qDebug() << "getEmployeeCountry(" << employeeName << ") called";
        if (employeeName == QLatin1String("Slow")) {
            PublicThread::msleep(100);
        }
        return employeeName + QString::fromLatin1(" France");
    }

    double getStuff(int foo, float bar, const QDateTime &dateTime)
    {
        // qDebug() << "getStuff called:" << foo << bar << dateTime.toTime_t();
        // qDebug() << "Request headers:" << requestHeaders();
        if (soapAction() != "MySoapAction") {
            qDebug() << "ERROR: SoapAction was" << soapAction();
            return 0; // error
        }
        const QString header1 = requestHeaders().header(QLatin1String("header1")).value().toString();
        if (header1 == QLatin1String("headerValue")) {
            KDSoapHeaders headers;
            KDSoapMessage header2;
            KDSoapValue header2Value(QString::fromLatin1("header2"), QString::fromLatin1("responseHeader"));
            header2Value.setNamespaceUri(QLatin1String("http://foo"));
            header2.childValues().append(header2Value);
            headers.append(header2);
            setResponseHeaders(headers);
        }
        return double(foo) + bar + double(dateTime.toMSecsSinceEpoch() / 1000.0);
    }
    QByteArray hexBinaryTest(const QByteArray &input1, const QByteArray &input2) const
    {
        if (soapAction() != "ActionHex") {
            qDebug() << "ERROR: SoapAction was" << soapAction();
            return ""; // error
        }
        return input1 + input2;
    }

private:
    bool m_requireAuth;
    bool m_useRawXML;
    bool m_rawXMLValid;
    QByteArray m_assembledXML;
};

class CountryServer : public KDSoapServer
{
    Q_OBJECT
public:
    CountryServer()
        : KDSoapServer()
        , m_requireAuth(false)
        , m_useRawXML(false)
    {
    }

    virtual QObject *createServerObject() override
    {
        return new CountryServerObject(m_requireAuth, m_useRawXML);
    }

    void setRequireAuth(bool b)
    {
        m_requireAuth = b;
    }
    void setUseRawXML(bool b)
    {
        m_useRawXML = b;
    }

Q_SIGNALS:
    void releaseSemaphore();

public Q_SLOTS:
    void quit()
    {
        thread()->quit();
    }
    void suspend()
    {
        KDSoapServer::suspend();
        emit releaseSemaphore();
    }
    void resume()
    {
        KDSoapServer::resume();
        emit releaseSemaphore();
    }

private:
    bool m_requireAuth;
    bool m_useRawXML;
};

// We need to do the listening and socket handling in a separate thread,
// so that the main thread can use synchronous calls. Note that this is
// really specific to unit tests and doesn't need to be done in a real
// KDSoap-based server.
class CountryServerThread : public QThread
{
    Q_OBJECT
public:
    CountryServerThread(KDSoapThreadPool *pool = 0)
        : m_threadPool(pool)
        , m_pServer(0)
    {
    }
    ~CountryServerThread()
    {
        // helgrind says don't call quit() here, it races with exec()
        if (m_pServer) {
            QMetaObject::invokeMethod(m_pServer, "quit");
        }
        wait();
    }
    CountryServer *startThread()
    {
        start();
        m_semaphore.acquire(); // wait for init to be done
        return m_pServer;
    }
    void suspend()
    {
        QMetaObject::invokeMethod(m_pServer, "suspend");
        m_semaphore.acquire();
    }
    void resume()
    {
        QMetaObject::invokeMethod(m_pServer, "resume");
        m_semaphore.acquire();
    }

protected:
    void run() override
    {
        CountryServer server;
        if (m_threadPool) {
            server.setThreadPool(m_threadPool);
        }
        if (server.listen()) {
            m_pServer = &server;
        }
        connect(&server, &CountryServer::releaseSemaphore, this, &CountryServerThread::slotReleaseSemaphore, Qt::DirectConnection);
        m_semaphore.release();
        exec();
        m_pServer = 0;
    }
private Q_SLOTS:
    void slotReleaseSemaphore()
    {
        m_semaphore.release();
    }

private:
    KDSoapThreadPool *m_threadPool;
    QSemaphore m_semaphore;
    CountryServer *m_pServer;
};

// to avoid a bit of duplication
class ClientSocket : public QTcpSocket
{
public:
    ClientSocket(CountryServer *server)
    {
        QUrl url(server->endPoint());
        connectToHost(url.host(), server->serverPort());
    }
};

class ServerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
#ifndef QT_NO_OPENSSL
        if (!QSslSocket::supportsSsl()) {
            qWarning("No SSL support on this machine, check that ssleay.so/ssleay32.dll is installed");
        } else {
            QVERIFY(KDSoapUnitTestHelpers::setSslConfiguration());
            QSslConfiguration defaultConfig = QSslConfiguration::defaultConfiguration();
            QFile certFile(QString::fromLatin1(":/certs/test-127.0.0.1-cert.pem"));
            if (certFile.open(QIODevice::ReadOnly)) {
                defaultConfig.setLocalCertificate(QSslCertificate(certFile.readAll()));
            }
            QFile keyFile(QString::fromLatin1(":/certs/test-127.0.0.1-key.pem"));
            if (keyFile.open(QIODevice::ReadOnly)) {
                defaultConfig.setPrivateKey(QSslKey(keyFile.readAll(), QSsl::Rsa));
            }
            QSslConfiguration::setDefaultConfiguration(defaultConfig);
        }
#endif
    }

    void testCall()
    {
        {
            CountryServerThread serverThread;
            CountryServer *server = serverThread.startThread();

            // qDebug() << "server ready, proceeding" << server->endPoint();
            KDSoapClientInterface *client = new KDSoapClientInterface(server->endPoint(), countryMessageNamespace());
            const KDSoapMessage response = client->call(QLatin1String("getEmployeeCountry"), countryMessage());
            QVERIFY(!response.isFault());
            QCOMPARE(response.childValues().first().value().toString(), expectedCountry());

            QCOMPARE(s_serverObjects.count(), 1);
            QCOMPARE(s_serverObjects.at(0)->thread(), &serverThread); // request handled by server thread itself (no thread pool)
            QCOMPARE(server->totalConnectionCount(), 1);
            delete client;
            QTest::qWait(100);
        }
        QCOMPARE(s_serverObjects.count(), 0);
    }

    void testAuth()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        server->setRequireAuth(true);
        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapAuthentication auth;
        auth.setUser(QLatin1String("kdab"));
        auth.setPassword(QLatin1String("pass42"));
        client.setAuthentication(auth);
        const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
        if (response.isFault()) {
            qDebug() << response.faultAsString();
            QVERIFY(!response.isFault());
        }
        QCOMPARE(response.childValues().first().value().toString(), expectedCountry());
    }

    void testRefusedAuth()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        server->setRequireAuth(true);
        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapAuthentication auth;
        auth.setUser(QLatin1String("kdab"));
        auth.setPassword(QLatin1String("invalid"));
        client.setAuthentication(auth);
        const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
        QVERIFY(response.isFault());
    }

    void testParamTypes()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        const KDSoapMessage response =
            client.call(QLatin1String("getStuff"), getStuffMessage(), QString::fromLatin1("MySoapAction"), getStuffRequestHeaders());
        if (response.isFault()) {
            qDebug() << response.faultAsString();
            QVERIFY(!response.isFault());
        }
        QCOMPARE(response.value().toDouble(), double(4 + 3.2 + 123456.789));
        const KDSoapHeaders responseHeaders = client.lastResponseHeaders();
        // qDebug() << responseHeaders;
        QCOMPARE(responseHeaders.header(QLatin1String("header2"), QLatin1String("http://foo")).value().toString(),
                 QString::fromLatin1("responseHeader"));
    }

    void testHeadersAsyncCall() // KDSOAP-45
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        m_returnMessages.clear();
        m_expectedMessages = 1;
        KDSoapPendingCall pendingCall =
            client.asyncCall(QLatin1String("getStuff"), getStuffMessage(), QString::fromLatin1("MySoapAction"), getStuffRequestHeaders());
        KDSoapPendingCallWatcher *watcher = new KDSoapPendingCallWatcher(pendingCall, this);
        connect(watcher, &KDSoapPendingCallWatcher::finished, this, &ServerTest::slotFinished);
        m_eventLoop.exec();
        QCOMPARE(m_returnMessages.count(), 1);
        QCOMPARE(m_returnMessages.at(0).value().toDouble(), double(4 + 3.2 + 123456.789));
        QCOMPARE(m_returnHeaders.count(), 1);
        QCOMPARE(m_returnHeaders.at(0).header(QLatin1String("header2"), QLatin1String("http://foo")).value().toString(),
                 QLatin1String("responseHeader"));
    }

    void testHexBinary()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        // qDebug() << "server ready, proceeding" << server->endPoint();
        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        client.setSoapVersion(KDSoapClientInterface::SOAP1_2);
        KDSoapMessage message;
        message.addArgument(QLatin1String("a"), QByteArray("KD"), KDSoapNamespaceManager::xmlSchema2001(), QString::fromLatin1("base64Binary"));
        message.addArgument(QLatin1String("b"), QByteArray("Soap"), KDSoapNamespaceManager::xmlSchema2001(), QString::fromLatin1("hexBinary"));
        const KDSoapMessage response = client.call(QLatin1String("hexBinaryTest"), message, QString::fromLatin1("ActionHex"));
        QCOMPARE(QString::fromLatin1(QByteArray::fromBase64(response.value().toByteArray()).constData()), QString::fromLatin1("KDSoap"));
    }

    void testMethodNotFound()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        // QTest::ignoreMessage(QtDebugMsg, "Slot not found: \"doesNotExist\" [soapAction = \"http://www.kdab.com/xml/MyWsdl/doesNotExist\" ]");
        const KDSoapMessage response = client.call(QLatin1String("doesNotExist"), message);
        QVERIFY(response.isFault());
        QCOMPARE(response.arguments().child(QLatin1String("faultcode")).value().toString(), QString::fromLatin1("Server.MethodNotFound"));
        QCOMPARE(response.arguments().child(QLatin1String("faultstring")).value().toString(), QString::fromLatin1("doesNotExist not found"));
    }

    void testMissingParams()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        KDSoapMessage message;
        message.addArgument(QLatin1String("foo"), 4);
        const KDSoapMessage response = client.call(QLatin1String("getStuff"), message);
        QVERIFY(response.isFault());
        QCOMPARE(response.faultAsString(), QString::fromLatin1("Fault code Server.RequiredArgumentMissing: bar,dateTime"));
    }

    void testThreadPoolBasic()
    {
        {
            KDSoapThreadPool threadPool;
            CountryServerThread serverThread(&threadPool);
            CountryServer *server = serverThread.startThread();

            KDSoapClientInterface *client = new KDSoapClientInterface(server->endPoint(), countryMessageNamespace());
            const KDSoapMessage response = client->call(QLatin1String("getEmployeeCountry"), countryMessage());
            QCOMPARE(response.childValues().first().value().toString(), expectedCountry());
            QCOMPARE(s_serverObjects.count(), 1);
            QThread *thread = s_serverObjects.at(0)->thread();
            QVERIFY(thread != qApp->thread());
            QVERIFY(thread != &serverThread);
            QCOMPARE(server->totalConnectionCount(), 1);
            delete client;
        }
        QCOMPARE(s_serverObjects.count(), 0);
    }

    void testMultipleThreads_data()
    {
        QTest::addColumn<int>("maxThreads");
        QTest::addColumn<int>("numRequests");
        QTest::addColumn<int>("numClients");
        QTest::addColumn<int>("expectedThreads");

        // QNetworkAccessManager only does 6 concurrent http requests
        // (QHttpNetworkConnectionPrivate::defaultHttpChannelCount = 6)
        // so with numRequests > 6, don't expect more than 6 threads being used; for this
        // we would need more than one QNAM, i.e. more than one KDSoapClientInterface.

        QTest::newRow("5_parallel_requests") << 5 << 5 << 1 << 5;
        QTest::newRow("5_requests_in_3_threads") << 3 << 5 << 1 << 3;
        QTest::newRow("3_requests_in_3_threads_from_2_clients") << 3 << 3 << 2 << 3; // this one reuses the idle threads
    }

    void testMultipleThreads()
    {
        QFETCH(int, maxThreads);
        QFETCH(int, numRequests);
        QFETCH(int, numClients);
        QFETCH(int, expectedThreads);
        {
            KDSoapThreadPool threadPool;
            threadPool.setMaxThreadCount(maxThreads);
            CountryServerThread serverThread(&threadPool);
            CountryServer *server = serverThread.startThread();
            for (int i = 0; i < numClients; ++i) {
                if (i > 0) {
                    QTest::qWait(100); // handle disconnection from previous client
                }
                // qDebug() << "Creating new client";
                KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
                m_returnMessages.clear();
                m_expectedMessages = numRequests;

                makeAsyncCalls(client, numRequests);
                m_eventLoop.exec();

                QCOMPARE(m_returnMessages.count(), m_expectedMessages);
                for (const KDSoapMessage &response : std::as_const(m_returnMessages)) {
                    QCOMPARE(response.childValues().first().value().toString(), expectedCountry());
                }
                QCOMPARE(s_serverObjects.count(), numRequests);
                QSet<QThread *> usedThreads;
                for (CountryServerObject *obj : std::as_const(s_serverObjects)) {
                    QThread *thread = obj->thread();
                    QVERIFY(thread != qApp->thread());
                    QVERIFY(thread != &serverThread);
                    usedThreads.insert(thread);
                }
                QCOMPARE(usedThreads.count(), expectedThreads);
            }
            QCOMPARE(server->totalConnectionCount(), numClients * numRequests);
        }
        QCOMPARE(s_serverObjects.count(), 0);
    }

// OSX: "Fault code 99: Unknown error", sometimes
// Windows/Linux with Qt 4.8 or 5.5: nothing happens after "82 sockets seen. 100 connected right now. Messages received 100"
#if 0
    void testMultipleThreadsMultipleClients_data()
    {
        QTest::addColumn<int>("maxThreads");
        QTest::addColumn<int>("numClients"); // number of "client interface" instances
        QTest::addColumn<int>("numRequests"); // number of requests per client interface (maximum 6)

        QTest::newRow("100 requests") << 5 << 20 << 5;
        QTest::newRow("300 requests") << 5 << 50 << 6;

#if 0 // disable for now, it breaks without glib, and it regularly breaks buildbot (354 messages received...)
#ifndef Q_OS_WIN // builbot gets "Fault code 99: Unknown error" after 358 connected sockets
        // Qt >= 4.8 uses many file descriptors (thread pipes) and then:
        // without glib, select() on fd > 1024, which gives "QSocketNotifier: Internal error".
        // With glib, though, it works.
        QTest::newRow("500 requests") << 5 << 125 << 4;
        QTest::newRow("600 requests, requires >1K fd") << 5 << 100 << 6;
        //QTest::newRow("1800 requests") << 5 << 300 << 6;
        QTest::newRow("3000 requests, requires >4K fd") << 5 << 500 << 6;
        QTest::newRow("10000 requests") << 5 << 1700 << 6;
#endif
#endif

        // Performance results (on a dual-core linux laptop)
        // time sudo ./server testMultipleThreadsMultipleClients:'10000 requests' ("total" number of seconds)
        // In release mode with Qt 4.6 in debug mode but a one-line change in QXmlStreamReaderPrivate::getChar: 32s total
        // In release mode with Qt 4.7 in release mode: 20s total
    }

    void testMultipleThreadsMultipleClients()
    {
        QFETCH(int, maxThreads);
        QFETCH(int, numClients);
        QFETCH(int, numRequests);
        const int expectedConnectedSockets = numClients * numRequests;
        // the *2 is because in this unittest, we have both the client and the server socket, in the same process
        int numFileDescriptors = expectedConnectedSockets * 2;
        // Qt (since 4.8) uses threads so it needs pipes -- more fds
        numFileDescriptors += numClients;
        if (!KDSoapServer::setExpectedSocketCount(numFileDescriptors)) {
            if (expectedConnectedSockets > 500) {
                QSKIP("needs root");
            } else {
                QVERIFY(false);    // should not happen
            }
        }

        // Test making many more concurrent connections, using multiple QNAMs to circumvent the 6 connections limit.
        KDSoapThreadPool threadPool;
        threadPool.setMaxThreadCount(maxThreads);
        CountryServerThread serverThread(&threadPool);
        CountryServer *server = serverThread.startThread();
        QVector<KDSoapClientInterface *> clients;
        clients.resize(numClients);
        m_returnMessages.clear();
        m_expectedMessages = numRequests * numClients;
        for (int i = 0; i < numClients; ++i) {
            KDSoapClientInterface *client = new KDSoapClientInterface(server->endPoint(), countryMessageNamespace());
            clients[i] = client;

            makeAsyncCalls(*client, numRequests);
        }
        m_server = server;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, &ServerTest::slotStats);
        timer.start(1000);

        QTimer expireTimer;
        connect(&expireTimer, &QTimer::timeout, this, &ServerTest::slotTimeout);
        m_eventLoop.quit();
        expireTimer.start(30000); // 30 s. Make this higher when running in valgrind.

        // FOR DEBUG
        //qDebug() << server->endPoint();
        //qApp->exec();

        m_eventLoop.exec();
        qDebug() << "exec returned";
        slotStats();

        int tries = 0;
        while (server->totalConnectionCount() < expectedConnectedSockets && ++tries < 30) {
            QTest::qWait(500); // makes totalConnectionCount() more reliable.
        }
        if (tries > 0) {
            qDebug() << "after qWait (" << tries << "times )";
            slotStats();
        }
        if (server->totalConnectionCount() < expectedConnectedSockets) {
            for (const KDSoapMessage &response : std::as_const(m_returnMessages)) {
                if (response.isFault()) {
                    qDebug() << response.faultAsString();
                    break;
                }
            }
        }
        // On Windows and Mac, it seems some sockets connect and then don't deliver a message
        // so the total number of connection counts could be more than expected
        //QCOMPARE(server->totalConnectionCount(), expectedConnectedSockets);
        QVERIFY2(server->totalConnectionCount() >= expectedConnectedSockets,
                 qPrintable(QString::number(server->totalConnectionCount())));

        QCOMPARE(m_returnMessages.count(), m_expectedMessages);
        for (const KDSoapMessage &response : std::as_const(m_returnMessages)) {
            QCOMPARE(response.childValues().first().value().toString(), expectedCountry());
        }
        //QCOMPARE(s_serverObjects.count(), expectedServerObjects);
        qDeleteAll(clients);
    }
#endif

    void testSuspend()
    {
        KDSoapThreadPool threadPool;
        threadPool.setMaxThreadCount(6);
        CountryServerThread serverThread(&threadPool);
        CountryServer *server = serverThread.startThread();
        const QString endPoint = server->endPoint();
        KDSoapClientInterface client(endPoint, countryMessageNamespace());
        m_returnMessages.clear();
        m_expectedMessages = 2;
        makeAsyncCalls(client, m_expectedMessages);
        m_eventLoop.exec();
        QCOMPARE(server->totalConnectionCount(), m_expectedMessages);
        const quint16 oldPort = server->serverPort();
        QCOMPARE(m_returnMessages.count(), 2);

        // suspend
        serverThread.suspend();
        m_returnMessages.clear();
        m_expectedMessages = 3;
        QCOMPARE(m_returnMessages.count(), 0);

        // -> a new client can't connect at all:
        // qDebug() << "make call from new client";
        QCOMPARE(server->endPoint(), QString()); // can't use that, it's not even listening anymore
        KDSoapClientInterface client2(endPoint, countryMessageNamespace());
        makeAsyncCalls(client2, 3);
        m_eventLoop.exec();
        QCOMPARE(m_returnMessages.count(), 3);
        QCOMPARE(m_returnMessages.first().isFault(), true);
        QCOMPARE(m_returnMessages.first().faultAsString(), QString::fromLatin1("Fault code 1: Connection refused"));
        m_returnMessages.clear();

        // -> and an existing connected client shouldn't be allowed to make new calls -- TODO: force disconnect
        // qDebug() << "make call from connected client";
        m_expectedMessages = 1;
        makeAsyncCalls(client, 1);
        m_eventLoop.exec();
        QCOMPARE(m_returnMessages.count(), 1);
        QCOMPARE(m_returnMessages.first().isFault(), true);
        QCOMPARE(m_returnMessages.first().faultAsString(), QString::fromLatin1("Fault code 1: Connection refused"));
        m_returnMessages.clear();

        // resume
        m_expectedMessages = 1;
        serverThread.resume();
        QCOMPARE(server->serverPort(), oldPort);
        makeAsyncCalls(client, 1);
        m_eventLoop.exec();
        QCOMPARE(m_returnMessages.count(), 1);

        // Test calling resume again, should warn
        QTest::ignoreMessage(QtWarningMsg, "KDSoapServer: resume() called without calling suspend() first");
        serverThread.resume();
    }

    void testSuspendUnderLoad()
    {
#ifdef Q_OS_MAC
        QSKIP("fails with 'select: Invalid argument' on mac, to be investigated");
#endif
        const int numRequests = 5;
        const int numClients = 80;
        const int maxThreads = 5;

        KDSoapThreadPool *threadPool = new KDSoapThreadPool;
        threadPool->setMaxThreadCount(maxThreads);
        CountryServerThread serverThread(threadPool);
        CountryServer *server = serverThread.startThread();
        QVector<KDSoapClientInterface *> clients;
        clients.resize(numClients);
        m_returnMessages.clear();
        m_expectedMessages = numRequests * numClients;
        for (int i = 0; i < numClients; ++i) {
            KDSoapClientInterface *client = new KDSoapClientInterface(server->endPoint(), countryMessageNamespace());
            clients[i] = client;
            makeAsyncCalls(*client, numRequests);
        }
        m_server = server;

        // Testing suspend/resume under load
        for (int n = 0; n < 4; ++n) {
            QTest::qWait(100);
            qDebug() << "suspend (" << n << ")";
            serverThread.suspend();
            QTest::qWait(100);
            qDebug() << "resume (" << n << ")";
            serverThread.resume();
        }

        if (m_returnMessages.count() < m_expectedMessages) {
            m_eventLoop.exec();
        }
        // Don't look at m_returnMessages or totalConnectionCount here,
        // some of them got an error, trying to connect while server was suspended.

        qDeleteAll(clients);
        server->setThreadPool(nullptr);
        delete threadPool; // stop all threads before deleting the server (which they access)
        // ## it would have been better API to make the server own the threadpool, to avoid this switcheroo on deletion
    }

    void testServerFault() // fault returned by server
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        makeFaultyCall(server->endPoint());
    }

    void testLogging()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        const QString fileName = QString::fromLatin1("output.log");
        QFile::remove(fileName);
        server->setLogFileName(fileName);
        QCOMPARE(server->logFileName(), fileName);
        server->setLogLevel(KDSoapServer::LogEveryCall);

        makeSimpleCall(server->endPoint());
        makeFaultyCall(server->endPoint());
        server->flushLogFile();

        QList<QByteArray> expected;
        expected << "CALL getEmployeeCountry";
        expected << "FAULT getEmployeeCountry -- Fault code Client.Data: Empty employee name (CountryServerObject). Error detail: Employee name must "
                    "not be empty";
        compareLines(expected, fileName);

        server->setLogLevel(KDSoapServer::LogNothing);
        makeSimpleCall(server->endPoint());
        makeFaultyCall(server->endPoint());
        server->flushLogFile();
        compareLines(expected, fileName);

        server->setLogLevel(KDSoapServer::LogFaults);
        makeSimpleCall(server->endPoint());
        makeFaultyCall(server->endPoint());
        expected << "FAULT getEmployeeCountry -- Fault code Client.Data: Empty employee name (CountryServerObject). Error detail: Employee name must "
                    "not be empty";
        server->flushLogFile();
        compareLines(expected, fileName);

        // Now make too many connections
        server->setMaxConnections(2);
        const int numClients = 4;
        QVector<KDSoapClientInterface *> clients;
        m_expectedMessages = 2;
        m_returnMessages.clear();
        clients.resize(numClients);
        for (int i = 0; i < numClients; ++i) {
            KDSoapClientInterface *client = new KDSoapClientInterface(server->endPoint(), countryMessageNamespace());
            clients[i] = client;
            makeAsyncCalls(*client, 1, true /*slow*/);
        }
        m_eventLoop.exec();
        QTest::qWait(1000);
        QCOMPARE(m_returnMessages.count(), 2);
        expected << "ERROR Too many connections (2), incoming connection rejected";
        expected << "ERROR Too many connections (2), incoming connection rejected";
        server->flushLogFile();
        compareLines(expected, fileName);

        qDeleteAll(clients);
        QFile::remove(fileName);
    }

    void testWsdlFile()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        const QString fileName = QString::fromLatin1("foo.wsdl");
        QFile file(fileName);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Hello world");
        file.flush();
        const QString pathInUrl = QString::fromLatin1("/path/to/file.wsdl");
        server->setWsdlFile(fileName, pathInUrl);

        QString url = server->endPoint();
        url.chop(1) /*trailing slash*/;
        url += pathInUrl;
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl {url});
        QNetworkReply *reply = manager.get(request);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        QCOMPARE(( int )reply->error(), ( int )QNetworkReply::NoError);
        QCOMPARE(reply->readAll(), QByteArray("Hello world"));
        QFile::remove(fileName);
    }

    void testFileDownload_data()
    {
        QTest::addColumn<QString>("fileToDownload"); // client
        QTest::addColumn<QFile::Permissions>("permissions"); // server
        QTest::addColumn<QByteArray>("expectedHttpReply");

        const QFile::Permissions readable = QFile::ReadOwner | QFile::ReadUser;
        const QFile::Permissions writable = QFile::WriteOwner | QFile::WriteUser;

        const QByteArray httpOK = "200 OK";
        const QByteArray httpForbidden = "403 Forbidden";
        const QByteArray httpNotFound = "404 Not Found";

        QTest::newRow("readable") << "/path/to/file_download.txt" << readable << httpOK;
        QTest::newRow("nonexistent") << "/nonexistent.txt" << readable << httpNotFound;
        QTest::newRow("unreadable") << "/path/to/file_download.txt" << writable << httpForbidden;
        QTest::newRow("dot_dot_in_middle") << "/subdir/../other/../path/to/file_download.txt" << readable << httpOK;
        QTest::newRow("double_slash") << "/subdir/../other//../path//to/file_download.txt" << readable << httpOK;
        QTest::newRow("dot_dot_at_start") << "../../path/to/file_download.txt" << readable << httpForbidden;
        QTest::newRow("with_query") << "/?query=../../path/to/file_download.txt" << readable << httpNotFound; // "GET /"
        QTest::newRow("another_query") << "?query=/../path/to/file_download.txt" << readable << httpForbidden;
        QTest::newRow("query_is_preserved") << "/path/to/file_download.txt?a=b&c=d" << readable << httpNotFound;
        QTest::newRow("with_ref") << "#/../../../path/to/file_download.txt" << readable << httpForbidden;
        QTest::newRow("invalid") << "#/path/to/file_download.txt" << readable << httpForbidden;

        QTest::newRow("leading_double_slash") << "//path/to/file_download.txt" << readable << httpOK;
        QTest::newRow("leading_triple_slash") << "///path/to/file_download.txt" << readable << httpOK;
        QTest::newRow("leading_triple_slash_and_dot_dot") << "///../path/to/file_download.txt" << readable << httpForbidden;
        QTest::newRow("leading_double_slash_and_dot_dot") << "//../path/to/file_download.txt" << readable << httpForbidden;
        QTest::newRow("leading_slash_and_dot_dot") << "/../path/to/file_download.txt" << readable << httpForbidden;
    }

    void testFileDownload()
    {
        QFETCH(QString, fileToDownload);
        QFETCH(QFile::Permissions, permissions);
        QFETCH(QByteArray, expectedHttpReply);

        QTimer download_timeout;
        download_timeout.setInterval(5000); // 5 seconds
        download_timeout.setSingleShot(true);
        QSignalSpy timeout_spy(&download_timeout, &QTimer::timeout);
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        server->setRequireAuth(false);

        const QString fileName = QString::fromLatin1("file_download.txt");
        QFile file(fileName);
        QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
        file.write("Hello world");
        file.flush();
        file.setPermissions(permissions);

        ClientSocket socket(server);
        QVERIFY(socket.waitForConnected());
        const QByteArray request = "GET " + fileToDownload.toLatin1() + " HTTP/1.1\r\n"
                                                                        "Content-Type: text/xml;charset=utf-8\r\n"
                                                                        "Content-Length: 0\r\n"
                                                                        "Host: 127.0.0.1:12345\r\n" // ignored
                                                                        "\r\n";
        socket.write(request);
        QVERIFY(socket.waitForBytesWritten(3000));
        QVERIFY(socket.bytesAvailable() || socket.waitForReadyRead(30000));
        const QByteArray reply = socket.readAll();

        file.setPermissions(QFile::ReadOwner | QFile::ReadUser | QFile::WriteOwner | QFile::WriteUser);
        QFile::remove(fileName);

        QCOMPARE(timeout_spy.count(), 0);
#if defined(Q_OS_WIN)
        if (!(permissions & QFile::ReadOwner)) {
            // on Windows, setting permissions to writeonly using QFile::setPermissions does not work
            // this has been confirmed in tst_qfile.cpp in the Qt unittests
            QEXPECT_FAIL("unreadable", "Windows does not currently support non-readable files.", Abort);
        }
#endif

        const QByteArray firstLine = reply.left(reply.indexOf('\r'));
        QCOMPARE(firstLine, "HTTP/1.1 " + expectedHttpReply);

        if (expectedHttpReply.endsWith("OK")) { // krazy:exclude=strings
            const QByteArray lastLine = reply.mid(reply.lastIndexOf("\r\n") + 2);
            QCOMPARE(lastLine, QByteArray("Hello world"));
        }
    }

    void testFileDownloadAuth_data()
    {
        QTest::addColumn<bool>("requireAuth"); // server
        QTest::addColumn<bool>("provideCorrectAuth"); // client
        QTest::addColumn<bool>("expectedSuccess");

        QTest::newRow("noauth") << false << false << true;
        QTest::newRow("failing_auth") << true << false << false;
        QTest::newRow("correct_auth") << true << true << true;
    }

    void testFileDownloadAuth()
    {
        QFETCH(bool, requireAuth);
        QFETCH(bool, provideCorrectAuth);
        QFETCH(bool, expectedSuccess);

        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        server->setRequireAuth(requireAuth);

        const QString fileName = QString::fromLatin1("file_download.txt");
        QFile file(fileName);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Hello world");
        file.flush();
        const QString pathInUrl = QString::fromLatin1("/path/to/file_download.txt");

        QString url = server->endPoint();
        url.chop(1) /*trailing slash*/;
        url += pathInUrl;

        m_auth.setUser(QLatin1String("kdab"));
        m_auth.setPassword(QLatin1String(provideCorrectAuth ? "pass42" : "invalid"));
        QNetworkAccessManager manager;
        connect(&manager, &QNetworkAccessManager::authenticationRequired, this, &ServerTest::slotAuthRequired);
        QNetworkRequest request(QUrl {url});
        QNetworkReply *reply = manager.get(request);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (expectedSuccess) {
            QCOMPARE(( int )reply->error(), ( int )QNetworkReply::NoError);
            QCOMPARE(reply->readAll(), QByteArray("Hello world"));
        } else {
            QCOMPARE(( int )reply->error(), ( int )QNetworkReply::AuthenticationRequiredError);
        }
        QFile::remove(fileName);
    }

    void testCustomVerbRequestAuth_data()
    {
        QTest::addColumn<bool>("requireAuth"); // server
        QTest::addColumn<bool>("provideCorrectAuth"); // client
        QTest::addColumn<QByteArray>("customHttpVerb");
        QTest::addColumn<int>("expectedError");
        QTest::addColumn<QByteArray>("expectedReply");

        QTest::newRow("noauth_known_verb") << false << false << QByteArray("PULL") << ( int )QNetworkReply::NoError << QByteArray("Hello world");
        QTest::newRow("failing_auth_known_verb") << true << false << QByteArray("PULL") << ( int )QNetworkReply::AuthenticationRequiredError
                                                 << QByteArray();
        QTest::newRow("correct_auth_known_verb") << true << true << QByteArray("PULL") << ( int )QNetworkReply::NoError << QByteArray("Hello world");
        QTest::newRow("noauth_unknown_verb") << false << false << QByteArray("UNKNOWN") << ( int )QNetworkReply::ContentOperationNotPermittedError
                                             << QByteArray();
        QTest::newRow("failing_auth_unknown_verb") << true << false << QByteArray("UNKNOWN") << ( int )QNetworkReply::AuthenticationRequiredError
                                                   << QByteArray();
        QTest::newRow("correct_auth_unknown_verb") << true << true << QByteArray("UNKNOWN") << ( int )QNetworkReply::ContentOperationNotPermittedError
                                                   << QByteArray();
    }

    void testCustomVerbRequestAuth()
    {
        QFETCH(bool, requireAuth);
        QFETCH(bool, provideCorrectAuth);
        QFETCH(QByteArray, customHttpVerb);
        QFETCH(int, expectedError);
        QFETCH(QByteArray, expectedReply);

        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        server->setRequireAuth(requireAuth);

        QString url = server->endPoint();
        url.chop(1) /*trailing slash*/;

        m_auth.setUser(QLatin1String("kdab"));
        m_auth.setPassword(QLatin1String(provideCorrectAuth ? "pass42" : "invalid"));
        QNetworkAccessManager manager;
        connect(&manager, &QNetworkAccessManager::authenticationRequired, this, &ServerTest::slotAuthRequired);
        QNetworkRequest request(QUrl {url});
        QNetworkReply *reply;
        reply = manager.sendCustomRequest(request, customHttpVerb);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        QCOMPARE(( int )reply->error(), expectedError);
        QCOMPARE(reply->readAll(), expectedReply);
    }

    // Using QNetworkAccessManager directly to send the request
    void testPostWithQNAM()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        QUrl url(server->endPoint());
        QNetworkRequest request(url);
        request.setRawHeader("SoapAction", "http://www.kdab.com/xml/MyWsdl/getEmployeeCountry");
        QString soapHeader = QString::fromLatin1("text/xml;charset=utf-8");
        request.setHeader(QNetworkRequest::ContentTypeHeader, soapHeader.toUtf8());
        QNetworkAccessManager accessManager;
        QNetworkReply *reply = accessManager.post(request, rawCountryMessage());
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QCOMPARE(reply->header(QNetworkRequest::KnownHeaders::ContentTypeHeader), "text/xml");
        const QByteArray response = reply->readAll();
        QVERIFY(xmlBufferCompare(response, expectedCountryResponse()));
    }

    void testPostWithSocket_data()
    {
        QTest::addColumn<int>("chunkSize");
        QTest::addColumn<bool>("useRawXML");

        QTest::newRow("no_chunks") << 1000 << false;
        QTest::newRow("100") << 100 << false;
        QTest::newRow("50") << 50 << false;
        QTest::newRow("20") << 20 << false;
        QTest::newRow("10") << 10 << false;

        QTest::newRow("rawXML") << 50 << true;
    }

    // Even more low-level, using a QTcpSocket to send the request
    // We can use this to send the request in multiple chunks and check the server handles it
    void testPostWithSocket()
    {
        QFETCH(int, chunkSize);
        QFETCH(bool, useRawXML);
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        server->setUseRawXML(useRawXML);

        ClientSocket socket(server);
        QVERIFY(socket.waitForConnected());
        const QByteArray message = rawCountryMessage(s_longEmployeeName);
        const QByteArray request = "POST / HTTP/1.1\r\n"
                                   "SoapAction: http://www.kdab.com/xml/MyWsdl/getEmployeeCountry\r\n"
                                   "Content-Type: text/xml;charset=utf-8\r\n"
                                   "Content-Length: "
            + QByteArray::number(message.size())
            + "\r\n"
              "Host: 127.0.0.1:12345\r\n" // ignored
              "\r\n"
            + message;
        for (int pos = 0; pos < request.size(); pos += chunkSize) {
            const QByteArray part = request.mid(pos, chunkSize);
            socket.write(part);
            QVERIFY(socket.waitForBytesWritten());
        }
        verifySocketResponse(socket, s_longEmployeeName);
    }

    void testChunkedTransferEncoding_data()
    {
        QTest::addColumn<int>("chunkSize");
        QTest::addColumn<bool>("withTrailers");

        QTest::newRow("no_chunks_t") << 1000 << true;
        QTest::newRow("no_chunks_f") << 1000 << false;
        QTest::newRow("100_t") << 100 << true;
        QTest::newRow("100_f") << 100 << false;
        QTest::newRow("50_t") << 50 << true;
        QTest::newRow("50_f") << 50 << false;
        QTest::newRow("20_t") << 20 << true;
        QTest::newRow("20_f") << 20 << false;
        QTest::newRow("10_t") << 10 << true;
        QTest::newRow("10_f") << 10 << false;
        QTest::newRow("5_f") << 5 << false;
    }

    void testChunkedTransferEncoding() // SOAP-123
    {
        QFETCH(int, chunkSize);
        QFETCH(bool, withTrailers);

        for (int i = 0; i < 2; ++i) {
            CountryServerThread serverThread;
            CountryServer *server = serverThread.startThread();

            if (i == 1) {
                server->setUseRawXML(true);
            }
            ClientSocket socket(server);
            QVERIFY(socket.waitForConnected());
            const QByteArray message = rawCountryMessage(s_longEmployeeName);
            const QByteArray headers = "POST / HTTP/1.1\r\n"
                                       "SoapAction: http://www.kdab.com/xml/MyWsdl/getEmployeeCountry\r\n"
                                       "Content-Type: text/xml;charset=utf-8\r\n"
                                       "Transfer-Encoding: chunked\r\n"
                                       "Host: 127.0.0.1:12345\r\n" // ignored
                                       "\r\n";
            socket.write(headers);
            QVERIFY(socket.waitForBytesWritten());
            for (int pos = 0; pos < message.size(); pos += chunkSize) {
                const QByteArray thisChunk = message.mid(pos, chunkSize);
                const QByteArray messagePart = QByteArray::number(thisChunk.size(), 16) + "\r\n" + thisChunk + "\r\n";
                // fragment that packet, for more testing
                const int fragmentSize = chunkSize / 5;
                for (int i = 0; i < messagePart.size(); i += fragmentSize) {
                    socket.write(messagePart.mid(i, fragmentSize));
                    QVERIFY(socket.waitForBytesWritten());
                }
            }
            // final chunk and trailers
            if (withTrailers) {
                socket.write("0\r\nIgnore: me\r\n\r\n");
            } else {
                socket.write("0\r\n\r\n");
            }
            QVERIFY(socket.waitForBytesWritten());
            verifySocketResponse(socket, s_longEmployeeName);
        }
    }

    void testContentTypeParsing() // SOAP 112
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        QUrl url(server->endPoint());
        QNetworkRequest request(url);
        QString soapHeader = QString::fromLatin1(
            "application/soap+xml; charset=utf-8; action=\"http://www.kdab.com/xml/MyWsdl/getEmployeeCountry\""); // space after semi-colon on purpose
        request.setHeader(QNetworkRequest::ContentTypeHeader, soapHeader.toUtf8());

        QNetworkAccessManager accessManager;
        QNetworkReply *reply = accessManager.post(request, rawCountryMessage("David Ä Faure", KDSoapClientInterface::SoapVersion::SOAP1_2));
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QCOMPARE(reply->header(QNetworkRequest::KnownHeaders::ContentTypeHeader), "application/soap+xml;charset=utf-8");
        const QByteArray response = reply->readAll();
        QVERIFY(xmlBufferCompare(response, expectedCountryResponse("David Ä Faure", KDSoapClientInterface::SoapVersion::SOAP1_2)));
    }

    void testGetShouldFail()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        QUrl url(server->endPoint());
        QNetworkRequest request(url);
        request.setRawHeader("SoapAction", "http://www.kdab.com/xml/MyWsdl/getEmployeeCountry");
        QString soapHeader = QString::fromLatin1("text/xml;charset=utf-8");
        request.setHeader(QNetworkRequest::ContentTypeHeader, soapHeader.toUtf8());
        QNetworkAccessManager accessManager;
        QNetworkReply *reply = accessManager.get(request);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QByteArray response = reply->readAll();
        QCOMPARE(response.constData(), "");
        QCOMPARE(( int )reply->error(), ( int )QNetworkReply::ContentNotFoundError);
    }

    void testHeadShouldFail()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        QUrl url(server->endPoint());
        QNetworkRequest request(url);
        QNetworkAccessManager accessManager;
        QTest::ignoreMessage(QtWarningMsg, "Unknown HTTP request: \"HEAD\" ");
        QNetworkReply *reply = accessManager.head(request);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QCOMPARE(( int )reply->error(), ( int )QNetworkReply::ContentOperationNotPermittedError);
        reply->deleteLater();
    }

    void testSetPath_data()
    {
        QTest::addColumn<QString>("serverPath");
        QTest::addColumn<QString>("requestPath");
        QTest::addColumn<bool>("expectedSuccess");

        QTest::newRow("success on /foo") << "/foo"
                                         << "/foo" << true;
        QTest::newRow("mismatching paths") << "/foo"
                                           << "/bar" << false;
    }

    void testSetPath()
    {
        QFETCH(QString, serverPath);
        QFETCH(QString, requestPath);
        QFETCH(bool, expectedSuccess);

        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        server->setPath(serverPath);
        QVERIFY(server->endPoint().endsWith(serverPath));
        const QString url = server->endPoint().remove(serverPath).append(requestPath);
        KDSoapClientInterface client(url, countryMessageNamespace());
        if (serverPath != requestPath) {
            QTest::ignoreMessage(QtWarningMsg, "Invalid path: \"/bar\"");
        }

        const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
        QCOMPARE(response.isFault(), !expectedSuccess);
        if (!expectedSuccess) {
            QCOMPARE(response.arguments().child(QLatin1String("faultcode")).value().toString(), QString::fromLatin1("Client.Data"));
            QCOMPARE(response.arguments().child(QLatin1String("faultstring")).value().toString(),
                     QString::fromLatin1("Method %1 not found in path %2").arg(QLatin1String("getEmployeeCountry"), requestPath));
        }
    }

    void testSsl()
    {
#ifndef QT_NO_OPENSSL
        if (!QSslSocket::supportsSsl()) {
            return;
        }
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();
        server->setFeatures(KDSoapServer::Ssl);
        QVERIFY(server->endPoint().startsWith(QLatin1String("https")));
        makeSimpleCall(server->endPoint());
#endif
    }

    void testAdditionalHttpResponseHeaderItems()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        QUrl url(server->endPoint());
        QNetworkRequest request(url);
        request.setRawHeader("SoapAction", "http://www.kdab.com/xml/MyWsdl/getEmployeeCountry");
        QString soapHeader = QString::fromLatin1("text/xml;charset=utf-8");
        request.setHeader(QNetworkRequest::ContentTypeHeader, soapHeader.toUtf8());
        QNetworkAccessManager accessManager;
        QNetworkReply *reply = accessManager.post(request, rawCountryMessage());
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        reply->readAll();

        QVERIFY(reply->hasRawHeader("Access-Control-Allow-Origin"));
        QCOMPARE(reply->rawHeader("Access-Control-Allow-Origin").constData(), "*");
        QVERIFY(reply->hasRawHeader("Access-Control-Allow-Headers"));
        QCOMPARE(reply->rawHeader("Access-Control-Allow-Headers").constData(), "Content-Type");
    }

    void testTimeout()
    {
        CountryServerThread serverThread;
        CountryServer *server = serverThread.startThread();

        KDSoapClientInterface client(server->endPoint(), countryMessageNamespace());
        client.setTimeout(10);
        KDSoapPendingCall pendingCall =
            client.asyncCall(QLatin1String("getEmployeeCountry"), countryMessage(true)); // the server object sleeps for 100ms
        QTRY_VERIFY(pendingCall.isFinished());
        QVERIFY(pendingCall.returnMessage().isFault());
        QCOMPARE(pendingCall.returnMessage().faultAsString(), QString::fromLatin1("Fault code 4: Operation timed out"));
    }

public Q_SLOTS:
    void slotFinished(KDSoapPendingCallWatcher *watcher)
    {
        m_returnMessages.append(watcher->returnMessage());
        m_returnHeaders.append(watcher->returnHeaders());
        if (m_returnMessages.count() == m_expectedMessages) {
            m_eventLoop.quit();
        }
    }

    void slotStats()
    {
        qDebug() << m_server->totalConnectionCount() << "sockets seen." << m_server->numConnectedSockets() << "connected right now. Messages received"
                 << m_returnMessages.count();
    }

    void slotTimeout()
    {
        qDebug() << "Timeout!";
        m_eventLoop.quit();
    }

    void slotAuthRequired(QNetworkReply *reply, QAuthenticator *authenticator)
    {
        // QNAM will just try and try again....
        if (!reply->property("authAdded").toBool()) {
            authenticator->setUser(m_auth.user());
            authenticator->setPassword(m_auth.password());
            reply->setProperty("authAdded", true);
        }
    }

private:
    QEventLoop m_eventLoop;
    int m_expectedMessages;
    QList<KDSoapMessage> m_returnMessages;
    QList<KDSoapHeaders> m_returnHeaders;

    KDSoapServer *m_server;
    QAuthenticator m_auth;

private:
    void makeSimpleCall(const QString &endPoint)
    {
        KDSoapClientInterface client(endPoint, countryMessageNamespace());
        const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), countryMessage());
        QVERIFY2(!response.isFault(), qPrintable(response.toXml()));
        QCOMPARE(response.childValues().first().value().toString(), expectedCountry());
    }

    void makeFaultyCall(const QString &endPoint)
    {
        KDSoapClientInterface client(endPoint, countryMessageNamespace());
        KDSoapMessage message;
        message.addArgument(QLatin1String("employeeName"), QString());
        const KDSoapMessage response = client.call(QLatin1String("getEmployeeCountry"), message);
        QVERIFY(response.isFault());
        QCOMPARE(response.arguments().child(QLatin1String("faultcode")).value().toString(), QString::fromLatin1("Client.Data"));
    }

    QList<KDSoapPendingCallWatcher *> makeAsyncCalls(KDSoapClientInterface &client, int numRequests, bool slow = false)
    {
        QList<KDSoapPendingCallWatcher *> watchers;
        for (int i = 0; i < numRequests; ++i) {
            KDSoapPendingCall pendingCall = client.asyncCall(QLatin1String("getEmployeeCountry"), countryMessage(slow));
            KDSoapPendingCallWatcher *watcher = new KDSoapPendingCallWatcher(pendingCall, this);
            connect(watcher, &KDSoapPendingCallWatcher::finished, this, &ServerTest::slotFinished);
            watchers.append(watcher);
        }
        return watchers;
    }

    static QString countryMessageNamespace()
    {
        return QString::fromLatin1(myWsdlNamespace);
    }
    static KDSoapMessage countryMessage(bool slow = false)
    {
        KDSoapMessage message;
        message.addArgument(QLatin1String("employeeName"), QString::fromUtf8(slow ? "Slow" : "David Ä Faure"));
        return message;
    }
    static QString expectedCountry()
    {
        return QString::fromUtf8("David Ä Faure France");
    }

    void verifySocketResponse(ClientSocket &socket, const QByteArray &employeeName)
    {
        QVERIFY(socket.waitForReadyRead());
        const QByteArray response = socket.readAll();
        const QByteArray responseFirstLine = response.left(response.indexOf("\r\n"));
        QCOMPARE(QString::fromUtf8(responseFirstLine.constData()), QString("HTTP/1.1 200 OK"));
        const int xmlStart = response.indexOf("\r\n\r\n") + 4;
        QVERIFY(xmlStart > 5);
        const QByteArray xmlResponse = response.mid(xmlStart);
        QVERIFY(xmlBufferCompare(xmlResponse, expectedCountryResponse(employeeName)));
    }

    static KDSoapMessage getStuffMessage()
    {
        KDSoapMessage message;
        message.addArgument(QLatin1String("foo"), 4);
        message.addArgument(QLatin1String("bar"), float(3.2));
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(123456789);
        message.addArgument(QLatin1String("dateTime"), dt);
        return message;
    }
    static KDSoapHeaders getStuffRequestHeaders()
    {
        KDSoapMessage header1;
        header1.addArgument(QString::fromLatin1("header1"), QString::fromLatin1("headerValue"));
        KDSoapHeaders headers;
        headers << header1;
        return headers;
    }

    static QList<QByteArray> readLines(const QString &fileName)
    {
        Q_ASSERT(!fileName.isEmpty());
        Q_ASSERT(QFile::exists(fileName));
        QFile file(fileName);
        const bool opened = file.open(QIODevice::ReadOnly);
        Q_ASSERT(opened);
        Q_UNUSED(opened);
        QList<QByteArray> lines;
        QByteArray line;
        do {
            line = file.readLine();
            if (!line.isEmpty()) {
                lines.append(line);
            }
        } while (!line.isEmpty());
        return lines;
    }

    void compareLines(const QList<QByteArray> &expectedLines, const QString &fileName)
    {
        QList<QByteArray> lines = readLines(fileName);
        // qDebug() << lines;
        QCOMPARE(lines.count(), expectedLines.count());
        for (int i = 0; i < lines.count(); ++i) {
            QByteArray line = lines[i];
            QVERIFY(line.endsWith('\n'));
            line.chop(1);
            if (!line.endsWith(expectedLines[i])) {
                qDebug() << "line" << i << ":\n"
                         << line << "\nexpected\n"
                         << expectedLines[i];
                QVERIFY(line.endsWith(expectedLines[i]));
            }
        }
    }
};

QTEST_MAIN(ServerTest)

// TODO: generate this method (needs a .wsdl file)
void CountryServerObject::processRequest(const KDSoapMessage &request, KDSoapMessage &response, const QByteArray &soapAction)
{
    setResponseNamespace(QLatin1String(myWsdlNamespace));
    const QByteArray method = request.name().toLatin1();
    if (method == "getEmployeeCountry") {
        if (soapAction != "http://www.kdab.com/xml/MyWsdl/getEmployeeCountry") {
            setFault(QLatin1String("Server.UnknownSoapAction"), QLatin1String("Unknown soap action"), QLatin1String(""),
                     QLatin1String(soapAction.constData()));
            return;
        }
        const QString employeeName = request.childValues().child(QLatin1String("employeeName")).value().toString();
        const QString ret = this->getEmployeeCountry(employeeName);
        if (!hasFault()) {
            response.setValue(QLatin1String("getEmployeeCountryResponse"));
            response.addArgument(QLatin1String("employeeCountry"), ret);
        }
    } else if (method == "getStuff") {
        const KDSoapValueList &values = request.childValues();
        const KDSoapValue valueFoo = values.child(QLatin1String("foo"));
        const KDSoapValue valueBar = values.child(QLatin1String("bar"));
        const KDSoapValue valueDateTime = values.child(QLatin1String("dateTime"));
        if (valueFoo.isNull() || valueBar.isNull() || valueDateTime.isNull()) {
            response.setFault(true);
            response.addArgument(QLatin1String("faultcode"), QLatin1String("Server.RequiredArgumentMissing"));
            QStringList argNames;
            if (valueFoo.isNull()) {
                argNames << QLatin1String("foo");
            }
            if (valueBar.isNull()) {
                argNames << QLatin1String("bar");
            }
            if (valueDateTime.isNull()) {
                argNames << QLatin1String("dateTime");
            }
            response.addArgument(QLatin1String("faultstring"), argNames.join(QChar::fromLatin1(',')));
            return;
        }
        const int foo = valueFoo.value().toInt();
        const float bar = valueBar.value().toFloat();
        const QDateTime dateTime = valueDateTime.value().toDateTime();
        const double ret = this->getStuff(foo, bar, dateTime);
        if (!hasFault()) {
            response.setValue(ret);
        }
    } else if (method == "hexBinaryTest") {
        const KDSoapValueList &values = request.childValues();
        const QByteArray input1 = QByteArray::fromBase64(values.child(QLatin1String("a")).value().toByteArray());
        // qDebug() << "input1=" << input1;
        const QByteArray input2 = QByteArray::fromHex(values.child(QLatin1String("b")).value().toByteArray());
        // qDebug() << "input2=" << input2;
        const QByteArray hex = this->hexBinaryTest(input1, input2);
        if (!hasFault()) {
            response.setValue(QVariant(hex));
        }
    } else {
        KDSoapServerObjectInterface::processRequest(request, response, soapAction);
    }
}

#include "test_serverlib.moc"
