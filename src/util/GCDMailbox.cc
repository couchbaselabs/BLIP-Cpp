//
// GCDMailbox.cc
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

#include "GCDMailbox.hh"
#include "Actor.hh"
#include "Logging.hh"
#include <algorithm>
#include <sstream>
#include "betterassert.hh"

using namespace std;

namespace litecore { namespace actor {


#if ACTORS_TRACK_STATS
#define beginLatency()  fleece::Stopwatch st
#define endLatency()    _maxLatency = max(_maxLatency, (double)st.elapsed())
#define beginBusy()     _busy.start()
#define endBusy()       _maxBusy = max(_maxBusy, _busy.lap())
#else
#define beginLatency()  ({})
#define endLatency()    ({})
#define beginBusy()     ({})
#define endBusy()       ({})
#endif

    
    static char kQueueMailboxSpecificKey;

    static const qos_class_t kQOS = QOS_CLASS_UTILITY;
    thread_local shared_ptr<ChannelManifest> GCDMailbox::sCurrentManifest;

    GCDMailbox::GCDMailbox(Actor *a, const std::string &name, GCDMailbox *parentMailbox)
    :_actor(a)
    {
        dispatch_queue_t targetQueue;
        if (parentMailbox)
            targetQueue = parentMailbox->_queue;
        else
            targetQueue = dispatch_get_global_queue(kQOS, 0);
        auto nameCstr = name.empty() ? nullptr : name.c_str();
        dispatch_queue_attr_t attr = DISPATCH_QUEUE_SERIAL;
        attr = dispatch_queue_attr_make_with_qos_class(attr, kQOS, 0);
        if (__builtin_available(iOS 10.0, macOS 10.12, tvos 10.0, watchos 3.0, *)) {
            attr = dispatch_queue_attr_make_with_autorelease_frequency(attr,
                                                        DISPATCH_AUTORELEASE_FREQUENCY_NEVER);
            _queue = dispatch_queue_create_with_target(nameCstr, attr, targetQueue);
        } else {
            _queue = dispatch_queue_create(nameCstr, attr);
            dispatch_set_target_queue(_queue, targetQueue);
        }
        dispatch_queue_set_specific(_queue, &kQueueMailboxSpecificKey, this, nullptr);
    }

    GCDMailbox::~GCDMailbox() {
        dispatch_release(_queue);
    }


    std::string GCDMailbox::name() const {
        return dispatch_queue_get_label(_queue);
    }


    Actor* GCDMailbox::currentActor() {
        auto mailbox = (GCDMailbox*) dispatch_get_specific(&kQueueMailboxSpecificKey);
        return mailbox ? mailbox->_actor : nullptr;
    }


    void GCDMailbox::safelyCall(void (^block)()) const {
        try {
            block();
        } catch (const std::exception &x) {
            _actor->caughtException(x);
            stringstream manifest;
            sCurrentManifest->dump(manifest);
            const auto dumped = manifest.str();
            Warn("%s", dumped.c_str());
        }
    }

    
    void GCDMailbox::enqueue(const string& methodName, void (^block)()) {
        beginLatency();
        ++_eventCount;
        retain(_actor);
        shared_ptr<ChannelManifest> currentManifest = sCurrentManifest ? sCurrentManifest : std::make_shared<ChannelManifest>();
        currentManifest->addEnqueueCall(methodName);
        auto wrappedBlock = ^{
            currentManifest->addExecution(methodName);
            sCurrentManifest = currentManifest;
            endLatency();
            beginBusy();
            safelyCall(block);
            afterEvent();
            sCurrentManifest.reset();
        };
        dispatch_async(_queue, wrappedBlock);
    }


    void GCDMailbox::enqueueAfter(delay_t delay, const string& methodName, void (^block)()) {
        beginLatency();
        ++_eventCount;
        retain(_actor);
        shared_ptr<ChannelManifest> currentManifest = sCurrentManifest ? sCurrentManifest : std::make_shared<ChannelManifest>();
        currentManifest->addEnqueueCall(methodName, delay.count());
        auto wrappedBlock = ^{
            currentManifest->addExecution(methodName);
            sCurrentManifest = currentManifest;
            endLatency();
            beginBusy();
            safelyCall(block);
            afterEvent();
            sCurrentManifest.reset();
        };
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
        if (ns > 0)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ns), _queue, wrappedBlock);
        else
            dispatch_async(_queue, wrappedBlock);
    }

    void GCDMailbox::afterEvent() {
        _actor->afterEvent();
        endBusy();
#if ACTORS_TRACK_STATS
        ++_callCount;
        if (_eventCount > _maxEventCount) {
            _maxEventCount = _eventCount;
        }
#endif
        --_eventCount;
        release(_actor);
    }


    void GCDMailbox::logStats() const {
#if ACTORS_TRACK_STATS
        LogTo(ActorLog, "%s handled %d events; max queue depth was %d; max latency was %s; busy total %s (%.1f%%), max %s",
              _actor->actorName().c_str(), _callCount, _maxEventCount,
              fleece::Stopwatch::formatTime(_maxLatency).c_str(),
              fleece::Stopwatch::formatTime(_busy.elapsed()).c_str(),
              (_busy.elapsed() / _createdAt.elapsed())*100.0,
              fleece::Stopwatch::formatTime(_maxBusy).c_str());
#endif
    }

} }
