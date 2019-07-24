//
// WebSocketImpl.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "WebSocketInterface.hh"
#include "Logging.hh"
#include "Stopwatch.hh"
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <set>

namespace uWS {
    template <const bool isServer> class WebSocketProtocol;
}

namespace litecore { namespace actor {
    class Timer;
}}

namespace litecore { namespace websocket {

    /** Transport-agnostic implementation of WebSocket protocol.
        It doesn't transfer data or run the handshake; it just knows how to encode and decode
        messages. */
    class WebSocketImpl : public WebSocket, Logging {
    public:
        WebSocketImpl(const URL &url,
                      Role role,
                      const fleece::AllocedDict &options,
                      bool framing);

        virtual void connect() override;
        virtual bool send(fleece::slice message, bool binary =true) override;
        virtual void close(int status =kCodeNormal, fleece::slice message =fleece::nullslice) override;

        // Concrete socket implementation needs to call these:
        void gotHTTPResponse(int status, const fleece::AllocedDict &headers);
        void onConnect();
        void onCloseRequested(int status, fleece::slice message);
        void onClose(int posixErrno);
        void onClose(CloseStatus);
        void onReceive(fleece::slice);
        void onWriteComplete(size_t);

        const fleece::AllocedDict& options() const   {return _options;}

    protected:
        virtual ~WebSocketImpl();
        virtual std::string loggingIdentifier() const override;

        // These methods have to be implemented in subclasses:
        virtual void closeSocket() =0;
        virtual void sendBytes(fleece::alloc_slice) =0;
        virtual void receiveComplete(size_t byteCount) =0;
        virtual void requestClose(int status, fleece::slice message) =0;

    private:
        template <const bool isServer>
        friend class uWS::WebSocketProtocol;
        friend class MessageImpl;

        using ClientProtocol = uWS::WebSocketProtocol<false>;
        using ServerProtocol = uWS::WebSocketProtocol<true>;

        bool sendOp(fleece::slice, int opcode);
        bool handleFragment(char *data,
                            size_t length,
                            unsigned int remainingBytes,
                            int opCode,
                            bool fin);
        bool receivedMessage(int opCode, fleece::alloc_slice message);
        bool receivedClose(fleece::slice);
        void deliverMessageToDelegate(fleece::slice data, bool binary);
        int heartbeatInterval() const;
        void schedulePing();
        void sendPing();
        void receivedPong();
        void startResponseTimer(std::chrono::seconds timeout);
        void timedOut();

        fleece::AllocedDict _options;
        bool _framing;
        std::unique_ptr<ClientProtocol> _clientProtocol;  // 3rd party class that does the framing
        std::unique_ptr<ServerProtocol> _serverProtocol;  // 3rd party class that does the framing
        std::mutex _mutex;                          //
        fleece::alloc_slice _curMessage;            // Message being received
        int _curOpCode;                             // Opcode of msg in _curMessage
        size_t _curMessageLength {0};                   // # of valid bytes in _curMessage
        size_t _bufferedBytes {0};                  // # bytes written but not yet completed
        size_t _deliveredBytes;                     // Temporary count of bytes sent to delegate
        bool _closeSent {false}, _closeReceived {false};    // Close message sent or received?
        fleece::alloc_slice _closeMessage;                  // The encoded close request message
        std::unique_ptr<actor::Timer> _pingTimer;
        std::unique_ptr<actor::Timer> _responseTimer;
        std::chrono::seconds _curTimeout;
        bool _timedOut {false};
        int _opToSend;
        fleece::alloc_slice _msgToSend;

        // Connection diagnostics, logged on close:
        fleece::Stopwatch _timeConnected {false};           // Time since socket opened
        uint64_t _bytesSent {0}, _bytesReceived {0};// Total byte count sent/received
    };

} }
