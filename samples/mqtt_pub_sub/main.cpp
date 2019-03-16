/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include <aws/crt/Api.h>

#include <aws/crt/StlAllocator.h>
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <algorithm>

using namespace Aws::Crt;

static void s_printHelp()
{
    fprintf(stdout, "Usage:\n");
    fprintf(stdout, "aws-crt-cpp-mqtt-pub-sub --endpoint <endpoint> --cert <path to cert>"
        " --key <path to key> --ca_file <optional: path to custom ca>\n\n");
    fprintf(stdout, "endpoint: the endpoint of the mqtt server not including a port\n");
    fprintf(stdout, "cert: path to your client certificate in PEM format\n");
    fprintf(stdout, "key: path to your key in PEM format\n");
    fprintf(stdout, "ca_file: Optional, if the mqtt server uses a certificate that's not already"
        " in your trust store, set this.\n");
    fprintf(stdout, "\tIt's the path to a CA file in PEM format\n");
}

bool s_cmdOptionExists(char** begin, char** end, const String& option)
{
    return std::find(begin, end, option) != end;
}

char* s_getCmdOption(char ** begin, char ** end, const String & option)
{
    char ** itr = std::find(begin, end, option);
    if (itr != end && ++itr != end)
    {
        return *itr;
    }
    return 0;
}

int main(int argc, char* argv[])
{

    /************************ Setup the Lib ****************************/
    /*
     * These make debug output via ErrorDebugString() work.
     */
    LoadErrorStrings();

    /*
     * Do the global initialization for the API.
     */
    ApiHandle apiHandle;

    String endpoint;
    String certificatePath;
    String keyPath;
    String caFile;

    /*********************** Parse Arguments ***************************/
    if (!(s_cmdOptionExists(argv, argv + argc, "--endpoint") &&
        s_cmdOptionExists(argv, argv + argc, "--cert") &&
        s_cmdOptionExists(argv, argv + argc, "--key")))
    {
        s_printHelp();
        return 0;
    }

    endpoint = s_getCmdOption(argv, argv + argc, "--endpoint");
    certificatePath = s_getCmdOption(argv, argv + argc, "--cert");
    keyPath = s_getCmdOption(argv, argv + argc, "--key");

    if (s_cmdOptionExists(argv, argv + argc, "--ca_file"))
    {
        caFile = s_getCmdOption(argv, argv + argc, "--ca_file");
    }

    /********************** Now Setup an Mqtt Client ******************/
    /*
     * You need an event loop group to process IO events.
     * If you only have a few connections, 1 thread is ideal
     */
    Io::EventLoopGroup eventLoopGroup(1);
    if (!eventLoopGroup)
    {
        fprintf(stderr, "Event Loop Group Creation failed with error %s\n",
            ErrorDebugString(eventLoopGroup.LastError()));
        exit(-1);
    }
    /*
     * We're using Mutual TLS for Mqtt, so we need to load our client certificates
     */
    Io::TlsContextOptions tlsCtxOptions =
        Io::TlsContextOptions::InitClientWithMtls(certificatePath.c_str(), keyPath.c_str());
    /*
     * If we have a custom CA, set that up here.
     */
    if (!caFile.empty())
    {
        tlsCtxOptions.OverrideDefaultTrustStore(nullptr, caFile.c_str());
    }

    uint16_t port = 8883;
    if (Io::TlsContextOptions::IsAlpnSupported())
    {
        /*
        * Use ALPN to negotiate the mqtt protocol on a normal
        * TLS port if possible.
        */
        tlsCtxOptions.SetAlpnList("x-amzn-mqtt-ca");
        port = 443;
    }

    Io::TlsContext tlsCtx(tlsCtxOptions, Io::TlsMode::CLIENT);

    if (!tlsCtx)
    {
        fprintf(stderr, "Tls Context creation failed with error %s\n",
            ErrorDebugString(tlsCtx.LastError()));
        exit(-1);
    }

    /*
     * Default Socket options to use. IPV4 will be ignored based on what DNS
     * tells us.
     */
    Io::SocketOptions socketOptions;
    socketOptions.connect_timeout_ms = 3000;
    socketOptions.domain = AWS_SOCKET_IPV4;
    socketOptions.type = AWS_SOCKET_STREAM;
    socketOptions.keep_alive_interval_sec = 0;
    socketOptions.keep_alive_timeout_sec = 0;
    socketOptions.keepalive = false;

    Io::ClientBootstrap bootstrap(eventLoopGroup);

    if (!bootstrap)
    {
        fprintf(stderr, "ClientBootstrap failed with error %s\n",
            ErrorDebugString(bootstrap.LastError()));
        exit(-1);
    }

    /*
     * Now Create a client. This can not throw.
     * An instance of a client must outlive its connections.
     * It is the users responsibility to make sure of this.
     */
    Mqtt::MqttClient mqttClient(bootstrap);

    /*
     * Since no exceptions are used, always check the bool operator
     * when an error could have occured.
     */
    if (!mqttClient)
    {
        fprintf(stderr, "MQTT Client Creation failed with error %s\n",
            ErrorDebugString(mqttClient.LastError()));
        exit(-1);
    }

    auto connectionOptions = tlsCtx.NewConnectionOptions();
    /*
     * Now create a connection object. Note: This type is move only
     * and its underlying memory is managed by the client.
     */
    auto connection =
        mqttClient.NewConnection(endpoint.c_str(), port, socketOptions, connectionOptions);

    if (!*connection)
    {
        fprintf(stderr, "MQTT Connection Creation failed with error %s\n",
            ErrorDebugString(connection->LastError()));
        exit(-1);
    }

    /*
     * In a real world application you probably don't want to enforce synchronous behavior
     * but this is a sample console application, so we'll just do that with a condition variable.
     */
    std::mutex mutex;
    std::condition_variable conditionVariable;
    bool connectionSucceeded = false;
    bool connectionClosed = false;
    bool connectionCompleted = false;

    /*
     * This will execute when an mqtt connect has completed or failed.
     */
    auto onConnectionCompleted = [&](Mqtt::MqttConnection&, int errorCode,
        Mqtt::ReturnCode returnCode, bool)
    {
        if (errorCode)
        {
            fprintf(stdout, "Connection failed with error %s\n", ErrorDebugString(errorCode));
            std::lock_guard<std::mutex> lockGuard(mutex);
            connectionSucceeded = false;
        }
        else
        {
            fprintf(stdout, "Connection completed with return code %d\n", returnCode);
            fprintf(stdout, "Connection state %d\n", static_cast<int>(connection->GetConnectionState()));
            connectionSucceeded = true;
        }
        {
            std::lock_guard<std::mutex> lockGuard(mutex);
            connectionCompleted = true;
        }
        conditionVariable.notify_one();
    };

    /*
     * Invoked when a disconnect message has completed.
     */
    auto onDisconnect = [&](Mqtt::MqttConnection& conn)
    {
        {
            fprintf(stdout, "Connection state %d\n", static_cast<int>(conn.GetConnectionState()));
            std::lock_guard<std::mutex> lockGuard(mutex);
            connectionClosed = true;
        }
        conditionVariable.notify_one();
    };

    connection->OnConnectionCompleted = std::move(onConnectionCompleted);
    connection->OnDisconnect = std::move(onDisconnect);

    /*
     * Actually perform the connect dance.
     */
    if (!connection->Connect("client_id12335456", true, 0))
    {
        fprintf(stderr, "MQTT Connection failed with error %s\n",
            ErrorDebugString(connection->LastError()));
        exit(-1);
    }

    std::unique_lock<std::mutex> uniqueLock(mutex);
    conditionVariable.wait(uniqueLock, [&]() {return connectionCompleted; });

    if (connectionSucceeded)
    {
        ByteBuf helloWorldPayload = ByteBufFromCString("Hello From aws-crt-cpp!");
        bool waitForSub = false;

        /*
         * This will be invoked upon the completion of Publish, Subscribe, and Unsubscribe.
         */
        auto onOpComplete = [&](Mqtt::MqttConnection&, uint16_t packetId, int errorCode)
        {
            if (packetId)
            {
                fprintf(stdout, "Operation on packetId %d Succeeded\n", (int)packetId);
            }
            else
            {
                fprintf(stdout, "Operation failed with error %s\n", aws_error_debug_str(errorCode));

            }
            std::lock_guard<std::mutex> lockGuard(mutex);

            if (!waitForSub)
            {
                conditionVariable.notify_one();
            }
        };

        /*
         * Publish our message.
         */
        auto packetId = connection->Publish("a/b", AWS_MQTT_QOS_AT_LEAST_ONCE,
            false, helloWorldPayload, onOpComplete);
        (void)packetId;
        conditionVariable.wait(uniqueLock);

        /*
         * This is invoked upon the receipt of a Publish on a subscribed topic.
         */
        auto onPublish = [&](Mqtt::MqttConnection&, const String& topic, const ByteBuf& byteBuf)
        {
             fprintf(stdout, "Publish recieved on topic %s\n", topic.c_str());               
             fprintf(stdout, "\n Message:\n");
             fwrite(byteBuf.buffer, 1, byteBuf.len, stdout);
             fprintf(stdout, "\n");
          
             conditionVariable.notify_one();
        };

        waitForSub = true;
        /*
         * Subscribe for incoming publish messages on topic.
         */
        auto onSubAck = [&](Mqtt::MqttConnection&, uint16_t packetId,
            const String& topic, Mqtt::QOS, int errorCode)
        {
            if (packetId)
            {
                fprintf(stdout, "Subscribe on topic %s on packetId %d Succeeded\n", topic.c_str(), (int)packetId);
            }
            else
            {
                fprintf(stdout, "Subscribe failed with error %s\n", aws_error_debug_str(errorCode));

            }
            std::lock_guard<std::mutex> lockGuard(mutex);

            if (!waitForSub)
            {
                conditionVariable.notify_one();
            }
        };

        packetId = connection->Subscribe("a/b", AWS_MQTT_QOS_AT_LEAST_ONCE, onPublish, onSubAck);
        waitForSub = false;
        conditionVariable.wait(uniqueLock);
        /*
         * Unsubscribe from the topic.
         */
        connection->Unsubscribe("a/b", onOpComplete);
        conditionVariable.wait(uniqueLock);
    }

    /* Disconnect */
    if (connection->Disconnect())
    {
       conditionVariable.wait(uniqueLock, [&]() { return connectionClosed; });
    }
    return 0;
}