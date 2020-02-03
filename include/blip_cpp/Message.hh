//
// Message.hh
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
#include "BLIPProtocol.hh"
#include "RefCounted.hh"
#include "fleece/Fleece.hh"
#include <functional>
#include <ostream>
#include <memory>
#include <mutex>

namespace fleece {
    class Value;
}

namespace litecore { namespace blip {
    using fleece::RefCounted;
    using fleece::Retained;

    class Connection;
    class MessageBuilder;
    class MessageIn;
    class InflaterWriter;
    class Codec;


    /** Progress notification for an outgoing request. */
    struct MessageProgress {
        enum State {
            kQueued,                // Outgoing request has been queued for delivery
            kSending,               // First bytes of message have been sent
            kAwaitingReply,         // Message sent; waiting for a reply (unless noreply)
            kReceivingReply,        // Reply is being received
            kComplete,              // Delivery (and receipt, if not noreply) complete.
            kDisconnected           // Socket disconnected before delivery or receipt completed
        } state;
        MessageSize bytesSent;
        MessageSize bytesReceived;
        Retained<MessageIn> reply;
    };

    using MessageProgressCallback = std::function<void(const MessageProgress&)>;


    struct Error {
        const fleece::slice domain;
        const int code {0};
        const fleece::slice message;

        Error()  { }
        Error(fleece::slice domain_, int code_, fleece::slice msg =fleece::nullslice)
        :domain(domain_), code(code_), message(msg)
        { }
    };

    // Like Error but with an allocated message string
    struct ErrorBuf : public Error {
        ErrorBuf()  { }

        ErrorBuf(fleece::slice domain, int code, fleece::alloc_slice msg)
        :Error(domain, code, msg)
        ,_messageBuf(msg)
        { }

    private:
        const fleece::alloc_slice _messageBuf;
    };


    /** Abstract base class of messages */
    class Message : public RefCounted {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;
        
        bool isResponse() const PURE             {return type() >= kResponseType;}
        bool isError() const PURE                {return type() == kErrorType;}
        bool urgent() const PURE                 {return hasFlag(kUrgent);}
        bool noReply() const PURE                {return hasFlag(kNoReply);}

        MessageNo number() const PURE            {return _number;}

    protected:
        friend class BLIPIO;
        
        Message(FrameFlags f, MessageNo n)
        :_flags(f), _number(n)
        {
            /*Log("NEW Message<%p, %s #%llu>", this, typeName(), _number);*/
        }

        virtual ~Message() {
            //Log("DELETE Message<%p, %s #%llu>", this, typeName(), _number);
        }

        FrameFlags flags() const PURE            {return _flags;}
        bool hasFlag(FrameFlags f) const PURE    {return (_flags & f) != 0;}
        bool isAck() const PURE                  {return type() == kAckRequestType ||
                                                    type() == kAckResponseType;}
        virtual bool isIncoming() const PURE     {return false;}
        MessageType type() const PURE            {return (MessageType)(_flags & kTypeMask);}
        const char* typeName() const PURE        {return kMessageTypeNames[type()];}

        void sendProgress(MessageProgress::State state,
                          MessageSize bytesSent, MessageSize bytesReceived,
                          MessageIn *reply);
        void disconnected();

        void dump(slice payload, slice body, std::ostream&);
        void dumpHeader(std::ostream&);
        void writeDescription(slice payload, std::ostream&);

        static const char* findProperty(slice payload, const char *propertyName) PURE;

        FrameFlags _flags;
        MessageNo _number;
        MessageProgressCallback _onProgress;
    };


    /** An incoming message. */
    class MessageIn : public Message {
    public:
        /** Gets a property value */
        slice property(slice property) const PURE;
        long intProperty(slice property, long defaultValue =0) const PURE;
        bool boolProperty(slice property, bool defaultValue =false) const PURE;

        /** Returns information about an error (if this message is an error.) */
        Error getError() const PURE;

        void setProgressCallback(MessageProgressCallback callback);

        /** Returns true if the message has been completely received including the body. */
        bool isComplete() const;

        /** The body of the message. */
        alloc_slice body() const;

        /** Returns the body, removing it from the message. The next call to extractBody() or
            body() will return only the data that's been read since this call. */
        alloc_slice extractBody();

        /** Converts the body from JSON to Fleece and returns a pointer to the root object. */
        fleece::Value JSONBody();

        /** Sends a response. (The message must be complete.) */
        void respond(MessageBuilder&);

        /** Sends an empty default response, unless the request was sent noreply.
            (The message must be complete.) */
        void respond();

        /** Sends an error as a response. (The message must be complete.) */
        void respondWithError(Error);

        /** Responds with an error saying that the message went unhandled.
            Call this if you don't know what to do with a request.
            (The message must be complete.) */
        void notHandled();

        void dump(std::ostream& out, bool withBody) {
            Message::dump(_properties, (withBody ? _body : fleece::alloc_slice()), out);
        }

    protected:
        friend class MessageOut;
        friend class BLIPIO;

        enum ReceiveState {
            kOther,
            kBeginning,
            kEnd
        };

        MessageIn(Connection*, FrameFlags, MessageNo,
                  MessageProgressCallback =nullptr,
                  MessageSize outgoingSize =0);
        virtual ~MessageIn();
        virtual bool isIncoming() const     {return true;}
        ReceiveState receivedFrame(Codec&, slice frame, FrameFlags);

        std::string description();

    private:
        void readFrame(Codec&, int mode, slice &frame, bool finalFrame);
        void acknowledge(uint32_t frameSize);

        Retained<Connection> _connection;       // The owning BLIP connection     
        mutable std::mutex _receiveMutex;
        MessageSize _rawBytesReceived {0};
        std::unique_ptr<fleece::JSONEncoder> _in; // Accumulates body data (not JSON)
        uint32_t _propertiesSize {0};           // Length of properties in bytes
        slice _propertiesRemaining;             // Subrange of _properties still to be read
        uint32_t _unackedBytes {0};             // # bytes received that haven't been ACKed yet
        alloc_slice _properties;                // Just the (still encoded) properties
        alloc_slice _body;                      // Just the body
        alloc_slice _bodyAsFleece;              // Body re-encoded into Fleece [lazy]
        const MessageSize _outgoingSize {0};
        bool _complete {false};
        bool _responded {false};
    };

} }
