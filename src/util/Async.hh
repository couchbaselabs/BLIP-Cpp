//
// Async.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
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
#include "Actor.hh"
#include "RefCounted.hh"
#include <functional>

namespace litecore { namespace actor {

    /*
     Async<T> represents a result of type T that may not be available yet. This concept is
     also referred to as a "future". You can create one by first creating an AsyncProvider<T>,
     which is also known as a "promise", then calling its `asyncValue` method:

        Async<int> getIntFromServer() {
            Retained<AsyncProvider<int>> intProvider = Async<int>::provider();
            sendServerRequestFor(intProvider);
            return intProvider->asyncValue();
        }

     You can simplify this somewhat:

        Async<int> getIntFromServer() {
            auto intProvider = Async<int>::provider();      // `auto` is your friend
            sendServerRequestFor(intProvider);
            return intProvider;                             // implicit conversion to Async
        }

     The AsyncProvider reference has to be stored somewhere until the result is available.
     Then you call its setResult() method:

        int result = valueReceivedFromServer();
        intProvider.setResult(result);

     Async<T> has a `ready` method that tells whether the result is available, and a `result`
     method that returns the result (or aborts if it's not available.) However, it does not
     provide any way to block and wait for the result. That's intentional: we don't want
     blocking! Instead, the way you work with async results is within an _asynchronous
     function_.

     ASYNCHRONOUS FUNCTIONS

     An asynchronous function is a function that can resolve Async values in a way that appears
     synchronous, but without actually blocking. It always returns an Async result (or void),
     since if the Async value it's resolving isn't available, the function itself has to return
     without (yet) providing a result. Here's what one looks like:

         Async<T> anAsyncFunction() {
            BEGIN_ASYNC_RETURNING(T)
            ...
            return t;
            END_ASYNC()
         }

     If the function doesn't return a result, it looks like this:

         void aVoidAsyncFunction() {
            BEGIN_ASYNC()
            ...
            END_ASYNC()
         }

     In between BEGIN and END you can "unwrap" Async values, such as those returned by other
     asynchronous functions, by calling asyncCall(). The first parameter is the variable to
     assign the result to, and the second is the expression returning the async result:

         asyncCall(int n, someOtherAsyncFunction());

     `asyncCall` is a macro that hides some very weird control flow. What happens is that, if
     the Async value isn't yet available, `asyncCall` causes the enclosing function to return.
     (Obviously it returns an unavailable Async value.) It also registers as an observer of
     the value, so when its result does become available, the enclosing function _resumes_
     right where it left off, assigns the result to the variable, and continues.

     ASYNC CALLS AND VARIABLE SCOPE

     `asyncCall()` places some odd restrictions on your code. Most importantly, it causes variables
     declared above it (in the BEGIN...END block) to end their scope. This is because the flow of
     control is likely to exit the function during evaluation of asyncCall, then return later.
     This means hat, after an `asyncCall` you cannot use any variables declared above it:

        int foo = ....;
        asyncCall(int n, someOtherAsyncFunction());
        foo += n;                  // ERROR: 'foo' is not declared

     If you want to use a variable across `asyncCall` scopes, you must declare it _before_ the
     BEGIN_ASYNC -- its scope then includes the entire async function:

        int foo;
        BEGIN_ASYNC_RETURNING(T)
        ...
        foo = ....;
        asyncCall(int n, someOtherAsyncFunction());
        foo += n;                  // OK!

     For similar reasons, an `asyncCall` cannot go inside an `if`, `for` or `while` block.
     Doing so will cause some unintuitive compiler errors.

     THREADING

     By default, an async method resumes immediately when the Async value it's waiting for becomes
     available. That means when the provider's `setResult` method is called, or when the
     async method returning that value finally returns a result. This is reasonable in single-
     threaded code.

     `asyncCall` is aware of Actors, however. So if an async Actor method waits, it will be resumed
     on that Actor's execution context. This ensures that the Actor's code runs single-threaded, as
     expected.

     */

#define BEGIN_ASYNC_RETURNING(T) \
    return _asyncBody<T>([=](AsyncState &_state) mutable -> T { \
        switch (_state._continueAt) { \
            case 0: { \

#define BEGIN_ASYNC() \
    _asyncBodyVoid([=](AsyncState &_state) mutable -> void { \
        switch (_state._continueAt) { \
            case 0: { \

#define asyncCall(VAR, CALL) \
                    if (_state.mustWaitFor(CALL, __LINE__)) return {}; \
                } \
                case __LINE__: \
                { \
                    VAR = _state.asyncResult<decltype(CALL)::ResultType>()

#define END_ASYNC() \
                    break; \
                } \
                default: /*fprintf(stderr,"Wrong label %d\n", _state._continueAt);*/ abort(); \
        } \
    });


    class AsyncBase;
    class AsyncProviderBase;
    template <class T> class Async;
    template <class T> class AsyncProvider;


    // base class of Async<T>
    class AsyncBase {
    public:
        explicit AsyncBase(const fleece::Retained<AsyncProviderBase> &provider)
        :_provider(provider)
        { }

        inline bool ready() const;

    protected:
        fleece::Retained<AsyncProviderBase> _provider;
        
        friend struct AsyncState;
    };
    

    /** The state data passed to the lambda of an async function. */
    struct AsyncState {
        fleece::Retained<AsyncProviderBase> _waitingOn;
        int _continueAt {0};

        bool mustWaitFor(const AsyncBase &a, int lineNo) {
            _waitingOn = a._provider;
            _continueAt = lineNo;
            return !a.ready();
        }

        template <class T>
        T asyncResult() {
            T result = ((AsyncProvider<T>*)_waitingOn.get())->result();
            _waitingOn = nullptr;
            return result;
        }
    };


    // base class of AsyncProvider<T> and AsyncVoidProvider.
    class AsyncProviderBase : public fleece::RefCounted {
    public:
        bool ready() const                      {return _ready;}

        void setObserver(AsyncProviderBase *p) {
            assert(!_observer);
            _observer = p;
        }

        void wakeUp(AsyncProviderBase *async) {
            assert(async == _state._waitingOn);
            if (_actor) {
                _actor->wakeAsyncProvider(this);
            } else {
                next();
            }
        }

    protected:
        void _wait() {
            _actor = Actor::currentActor();
            _state._waitingOn->setObserver(this);
        }

        void _gotResult() {
            _ready = true;
            auto observer = _observer;
            _observer = nullptr;
            if (observer)
                observer->wakeUp(this);
        }

        virtual void next() =0;

        AsyncState _state;
        bool _ready {false};
        Retained<Actor> _actor;
        AsyncProviderBase* _observer {nullptr};

        friend class Actor;
    };


    /** Special case of AsyncProvider for use in functions with no return value (void). */
    class AsyncVoidProvider : public AsyncProviderBase {
    public:
        explicit AsyncVoidProvider(std::function<void(AsyncState&)> body)
        :_body(body)
        {
            next();
        }

    private:
        void next() override {
            _body(_state);
            if (_state._waitingOn)
                _wait();
        }

        std::function<void(AsyncState&)> _body;
    };


    /** An asynchronously-provided result, seen from the producer side. */
    template <class T>
    class AsyncProvider : public AsyncProviderBase {
    public:
        explicit AsyncProvider(std::function<T(AsyncState&)> body)
        :_body(body)
        {
            next();
        }

        static Retained<AsyncProvider> create() {
            return new AsyncProvider;
        }

        Async<T> asyncValue() {
            return Async<T>(this);
        }

        void setResult(const T &result) {
            _result = result;
            _gotResult();
        }

        const T& result() const {
            assert(_ready);
            return _result;
        }

    private:
        AsyncProvider()
        { }

        void next() override {
            _result = _body(_state);
            if (_state._waitingOn)
                _wait();
            else
                _gotResult();
        }

        std::function<T(AsyncState&)> _body;
        T _result {};
    };


    /** An asynchronously-provided result, seen from the client side. */
    template <class T>
    class Async : public AsyncBase {
    public:
        Async(AsyncProvider<T> *provider)
        :AsyncBase(provider)
        { }

        Async(const fleece::Retained<AsyncProvider<T>> &provider)
        :AsyncBase(provider)
        { }

        bool ready() const {
            return _provider->ready();
        }

        const T& result() const {
            return ((AsyncProvider<T>*)_provider.get())->result();
        }

        static Retained<AsyncProvider<T>> provider() {
            return AsyncProvider<T>::create();
        }

        using ResultType = T;
    };


    /** Body of an async function: Creates an AsyncProvider from the lambda given,
        then returns an Async that refers to that provider. */
    template <class T>
    Async<T> _asyncBody(std::function<T(AsyncState&)> bodyFn) {
        auto provider = new AsyncProvider<T>(bodyFn);
        return Async<T>(provider);
    }

    /** Special case of _asyncBody for functions returning void. */
    static inline
    void _asyncBodyVoid(std::function<void(AsyncState&)> bodyFn) {
        (void) new AsyncVoidProvider(bodyFn);
    }


    bool AsyncBase::ready() const {
        return _provider->ready();
    }

} }
