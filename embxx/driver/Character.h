//
// Copyright 2013 (C). Alex Robenko. All rights reserved.
//

// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

/// @file embxx/driver/Character.h
/// Put file contains definition of "Character" device driver class.

#pragma once

#include <functional>
#include "embxx/util/StaticFunction.h"
#include "embxx/util/Assert.h"
#include "embxx/error/ErrorStatus.h"
#include "embxx/device/context.h"
#include "embxx/container/StaticQueue.h"

namespace embxx
{

namespace driver
{

namespace details
{


template <typename TEventLoop, typename TInfo>
void invokeHandler(
    TEventLoop& eventLoop,
    TInfo& info,
    const embxx::error::ErrorStatus& es,
    bool interruptCtx)
{
    auto sizeToReport = static_cast<std::size_t>(info.current_ - info.start_);
    GASSERT(info.handler_);
    auto boundHandler =
        std::bind(
            std::move(info.handler_),
            es,
            sizeToReport);

    bool postResult = false;
    if (interruptCtx) {
        postResult = eventLoop.postInterruptCtx(std::move(boundHandler));
    }
    else {
        postResult = eventLoop.post(std::move(boundHandler));
    }
    static_cast<void>(postResult);
    GASSERT(postResult);
    GASSERT(!info.handler_);
}

template <typename TDevice,
          typename TEventLoop,
          typename THandler,
          typename TReadUntilPred>
class CharacterReadSupportBase
{
public:

    ~CharacterReadSupportBase()
    {
        device_.setCanReadHandler(nullptr);
        device_.setReadCompleteHandler(nullptr);
    }

    CharacterReadSupportBase(const CharacterReadSupportBase&) = default;
    CharacterReadSupportBase(CharacterReadSupportBase&&) = default;
    CharacterReadSupportBase& operator=(const CharacterReadSupportBase&) = default;
    CharacterReadSupportBase& operator=(CharacterReadSupportBase&&) = default;

protected:
    typedef TDevice Device;
    typedef TEventLoop EventLoop;
    typedef typename Device::CharType CharType;
    typedef THandler ReadHandler;
    typedef TReadUntilPred ReadUntilPred;

    struct ReadUntilValid {};
    struct ReadUntilInvalid {};

    typedef typename std::conditional<
        std::is_same<ReadUntilPred, std::nullptr_t>::value,
        ReadUntilInvalid,
        ReadUntilValid
    >::type ReadUntilStatus;

    struct ReadInfo
    {
        ReadInfo()
            : start_(nullptr),
              current_(nullptr),
              bufSize_(0)
        {
        }

        template <typename THandlerParam, typename TPredParam>
        ReadInfo(
            CharType* buf,
            std::size_t bufSize,
            THandlerParam&& handler,
            TPredParam&& pred = ReadUntilPred())
            : start_(buf),
              current_(buf),
              bufSize_(bufSize),
              handler_(std::forward<THandlerParam>(handler)),
              readUntilPred_(std::forward<TPredParam>(pred))
        {
        }

        CharType* start_;
        CharType* current_;
        std::size_t bufSize_;
        ReadHandler handler_;
        ReadUntilPred readUntilPred_;
    };

    CharacterReadSupportBase(Device& device, EventLoop& el)
      : device_(device),
        el_(el)
    {
    }

    static bool seekedCharFound(CharType ch, ReadInfo& info)
    {
        return seekedCharFound(ch, info, ReadUntilStatus());
    }

    static bool seekedCharFound(CharType ch, ReadInfo& info, ReadUntilValid)
    {
        return info.readUntilPred_ && info.readUntilPred_(ch);
    }

    static bool seekedCharFound(CharType ch, ReadInfo& info, ReadUntilInvalid)
    {
        static_cast<void>(ch);
        static_cast<void>(info);
        return false;
    }


    Device& device_;
    EventLoop& el_;
};

template <typename TDevice,
          typename TEventLoop,
          typename THandler,
          typename TReadUntilPred,
          std::size_t TMaxPendingReads>
class CharacterReadSupport : public CharacterReadSupportBase<TDevice, TEventLoop, THandler, TReadUntilPred>
{
    typedef CharacterReadSupportBase<TDevice, TEventLoop, THandler, TReadUntilPred> Base;

public:

    ~CharacterReadSupport() = default;
    CharacterReadSupport(const CharacterReadSupport&) = default;
    CharacterReadSupport(CharacterReadSupport&&) = default;
    CharacterReadSupport& operator=(const CharacterReadSupport&) = default;
    CharacterReadSupport& operator=(CharacterReadSupport&&) = default;

protected:
    typedef typename Base::Device Device;
    typedef typename Base::EventLoop EventLoop;
    typedef typename Base::CharType CharType;

    CharacterReadSupport(Device& device, EventLoop& el)
      : Base(device, el)
    {
        Base::device_.setCanReadHandler(
            std::bind(
                &CharacterReadSupport::canReadInterruptHandler, this));

        Base::device_.setReadCompleteHandler(
            std::bind(
                &CharacterReadSupport::readCompleteInterruptHandler,
                this,
                std::placeholders::_1));
    }

    template <typename TFunc>
    void asyncRead(
        CharType* buf,
        std::size_t size,
        TFunc&& func)
    {
        asyncReadUntil(buf, size, typename Base::ReadUntilPred(), std::forward<TFunc>(func));
    }

    template <typename TPred, typename TFunc>
    void asyncReadUntil(
        CharType* buf,
        std::size_t size,
        TPred&& pred,
        TFunc&& func)
    {
        bool suspended = Base::device_.suspend(EventLoopContext());
        GASSERT(!queue_.full());
        queue_.emplaceBack(buf, size, std::forward<TFunc>(func), std::forward<TPred>(pred));

        if (suspended) {
            Base::device_.resume(EventLoopContext());
            return;
        }

        GASSERT(queue_.size() == 1U);
        startNextRead(false);
    }

    bool cancelRead()
    {
        if (!Base::device_.cancelRead(EventLoopContext())) {
            GASSERT(queue_.empty());
            return false;
        }

        std::for_each(queue_.begin(), queue_.end(),
            [this](typename InfoQueue::Reference info)
            {
                GASSERT(info.current_ < (info.start_ + info.bufSize_));
                invokeHandler(Base::el_, info, embxx::error::ErrorCode::Aborted, false);
            });
        queue_.clear();
        return true;
    }

private:

    typedef embxx::device::context::EventLoop EventLoopContext;
    typedef embxx::device::context::Interrupt InterruptContext;
    typedef typename Base::ReadInfo ReadInfo;
    typedef embxx::container::StaticQueue<ReadInfo, TMaxPendingReads> InfoQueue;

    void startNextRead(bool interruptCtx)
    {
        while (!queue_.empty()) {
            auto& info = queue_.front();
            if (info.bufSize_ == 0) {
                auto code = embxx::error::ErrorCode::Success;
                if (info.readUntilPred_) {
                    code = embxx::error::ErrorCode::BufferOverflow;
                }
                invokeHandler(Base::el_, info, code, interruptCtx);
                queue_.popFront();
                continue;
            }

            if (interruptCtx) {
                Base::device_.startRead(info.bufSize_, InterruptContext());
            }
            else {
                Base::device_.startRead(info.bufSize_, EventLoopContext());
            }
            break;
        }
    }

    void canReadInterruptHandler()
    {
        GASSERT(!queue_.empty());
        auto& info = queue_.front();
        while(Base::device_.canRead(InterruptContext())) {
            if ((info.start_ + info.bufSize_) <= info.current_) {
                // The device control object mustn't allow it.
                GASSERT(0);
                break;
            }

            auto ch = Base::device_.read(InterruptContext());
            *info.current_ = ch;
            ++info.current_;

            if (Base::seekedCharFound(ch, info)) {
                if (Base::device_.cancelRead(InterruptContext())) {
                    invokeHandler(Base::el_, info, embxx::error::ErrorCode::Success, true);
                    queue_.popFront();
                    startNextRead(true);
                }
            }
        }
    }

    void readCompleteInterruptHandler(const embxx::error::ErrorStatus& es)
    {
        GASSERT(!queue_.empty());
        auto& info = queue_.front();
        GASSERT(info.start_ < info.current_);
        if (es || (!Base::seekedCharFound(*(info.current_ - 1), info))) {
            invokeHandler(Base::el_, info, es, true);
        }
        else {
            invokeHandler(Base::el_, info, embxx::error::ErrorCode::BufferOverflow, true);
        }
        queue_.popFront();
        startNextRead(true);
    }

    InfoQueue queue_;
};

template <typename TDevice,
          typename TEventLoop,
          typename THandler,
          typename TReadUntilPred>
class CharacterReadSupport<TDevice, TEventLoop, THandler, TReadUntilPred, 1U> :
    public CharacterReadSupportBase<TDevice, TEventLoop, THandler, TReadUntilPred>
{
    typedef CharacterReadSupportBase<TDevice, TEventLoop, THandler, TReadUntilPred> Base;
public:

    ~CharacterReadSupport() = default;
    CharacterReadSupport(const CharacterReadSupport&) = default;
    CharacterReadSupport(CharacterReadSupport&&) = default;
    CharacterReadSupport& operator=(const CharacterReadSupport&) = default;
    CharacterReadSupport& operator=(CharacterReadSupport&&) = default;

protected:
    typedef typename Base::Device Device;
    typedef typename Base::EventLoop EventLoop;
    typedef typename Base::CharType CharType;

    CharacterReadSupport(Device& device, EventLoop& el)
      : Base(device, el)
    {
        Base::device_.setCanReadHandler(
            std::bind(
                &CharacterReadSupport::canReadInterruptHandler, this));
        Base::device_.setReadCompleteHandler(
            std::bind(
                &CharacterReadSupport::readCompleteInterruptHandler,
                this,
                std::placeholders::_1));
    }

    template <typename TFunc>
    void asyncRead(
        CharType* buf,
        std::size_t size,
        TFunc&& func)
    {
        GASSERT(!info_.handler_); // No read in progress
        info_.handler_ = std::forward<TFunc>(func);
        info_.readUntilPred_ = nullptr;
        initRead(buf, size);
    }

    template <typename TPred, typename TFunc>
    void asyncReadUntil(
        CharType* buf,
        std::size_t size,
        TPred&& pred,
        TFunc&& func)
    {
        GASSERT(!info_.handler_); // No read in progress
        info_.handler_ = std::forward<TFunc>(func);
        info_.readUntilPred_ = std::forward<TPred>(pred);
        initRead(buf, size);
    }

    bool cancelRead()
    {
        if (!Base::device_.cancelRead(EventLoopContext())) {
            GASSERT(!info_.handler_);
            return false;
        }

        GASSERT(info_.handler_);
        GASSERT(info_.current_ < (info_.start_ + info_.bufSize_));
        invokeHandler(Base::el_, info_, embxx::error::ErrorCode::Aborted, false);
        return true;
    }

private:
    typedef embxx::device::context::EventLoop EventLoopContext;
    typedef embxx::device::context::Interrupt InterruptContext;
    typedef typename Base::ReadInfo ReadInfo;

    void canReadInterruptHandler()
    {
        while(Base::device_.canRead(InterruptContext())) {
            if ((info_.start_ + info_.bufSize_) <= info_.current_) {
                // The device control object mustn't allow it.
                GASSERT(0);
                break;
            }

            auto ch = Base::device_.read(InterruptContext());
            *info_.current_ = ch;
            ++info_.current_;

            if (Base::seekedCharFound(ch, info_)) {
                if (Base::device_.cancelRead(InterruptContext())) {
                    invokeHandler(Base::el_, info_, embxx::error::ErrorCode::Success, true);
                }
            }
        }
    }

    void readCompleteInterruptHandler(const embxx::error::ErrorStatus& es)
    {
        if (es || (!Base::seekedCharFound(*(info_.current_ - 1), info_))) {
            invokeHandler(Base::el_, info_, es, true);
            return;
        }

        invokeHandler(Base::el_, info_, embxx::error::ErrorCode::BufferOverflow, true);
    }

    void initRead(
        CharType* buf,
        std::size_t size)
    {
        info_.start_ = buf;
        info_.current_ = buf;
        info_.bufSize_ = size;

        if (size == 0) {
            auto code = embxx::error::ErrorCode::Success;
            if (info_.readUntilPred_) {
                code = embxx::error::ErrorCode::BufferOverflow;
            }
            invokeHandler(Base::el_, info_, code, false);
            return;
        }

        Base::device_.startRead(size, EventLoopContext());
    }

    ReadInfo info_;
};

template <typename TDevice,
          typename TEventLoop,
          typename THandler,
          typename TReadUntilPred>
class CharacterReadSupport<TDevice, TEventLoop, THandler, TReadUntilPred, 0U>
{
protected:
    template <typename... TArgs>
    CharacterReadSupport(TArgs&&...)
    {
    }
};


template <typename TDevice,
          typename TEventLoop,
          typename THandler>
class CharacterWriteSupportBase
{
public:

    ~CharacterWriteSupportBase()
    {
        device_.setCanWriteHandler(nullptr);
        device_.setWriteCompleteHandler(nullptr);
    }

    CharacterWriteSupportBase(const CharacterWriteSupportBase&) = default;
    CharacterWriteSupportBase(CharacterWriteSupportBase&&) = default;
    CharacterWriteSupportBase& operator=(const CharacterWriteSupportBase&) = default;
    CharacterWriteSupportBase& operator=(CharacterWriteSupportBase&&) = default;

protected:
    typedef TDevice Device;
    typedef TEventLoop EventLoop;
    typedef typename Device::CharType CharType;
    typedef THandler WriteHandler;

    struct WriteInfo
    {
        WriteInfo()
            : start_(nullptr),
              current_(nullptr),
              bufSize_(0)
        {
        }

        const CharType* start_;
        const CharType* current_;
        std::size_t bufSize_;
        WriteHandler handler_;
    };

    CharacterWriteSupportBase(Device& device, EventLoop& el)
      : device_(device),
        el_(el)
    {
    }

    Device& device_;
    EventLoop& el_;
};

template <typename TDevice,
          typename TEventLoop,
          typename THandler,
          std::size_t TMaxPendingWrites>
class CharacterWriteSupport;

template <typename TDevice,
          typename TEventLoop,
          typename THandler>
class CharacterWriteSupport<TDevice, TEventLoop, THandler, 1U> :
    public CharacterWriteSupportBase<TDevice, TEventLoop, THandler>
{
    typedef CharacterWriteSupportBase<TDevice, TEventLoop, THandler> Base;
public:

    ~CharacterWriteSupport() = default;
    CharacterWriteSupport(const CharacterWriteSupport&) = default;
    CharacterWriteSupport(CharacterWriteSupport&&) = default;
    CharacterWriteSupport& operator=(const CharacterWriteSupport&) = default;
    CharacterWriteSupport& operator=(CharacterWriteSupport&&) = default;

protected:
    typedef typename Base::Device Device;
    typedef typename Base::EventLoop EventLoop;
    typedef typename Base::CharType CharType;

    CharacterWriteSupport(Device& device, EventLoop& el)
      : Base(device, el)
    {
        Base::device_.setCanWriteHandler(
            std::bind(
                &CharacterWriteSupport::canWriteInterruptHandler, this));
        Base::device_.setWriteCompleteHandler(
            std::bind(
                &CharacterWriteSupport::writeCompleteInterruptHandler,
                this,
                std::placeholders::_1));
    }

    template <typename TFunc>
    void asyncWrite(
        const CharType* buf,
        std::size_t size,
        TFunc&& func)
    {
        GASSERT(!info_.handler_); // No write in progress
        info_.handler_ = std::forward<TFunc>(func);
        initWrite(buf, size);
    }

    bool cancelWrite()
    {
        if (!Base::device_.cancelWrite(EventLoopContext())) {
            GASSERT(!info_.handler_);
            return false;
        }

        GASSERT(info_.handler_);
        GASSERT(info_.current_ < (info_.start_ + info_.bufSize_));
        invokeHandler(Base::el_, info_, embxx::error::ErrorCode::Aborted, false);
        return true;
    }

private:
    typedef embxx::device::context::EventLoop EventLoopContext;
    typedef embxx::device::context::Interrupt InterruptContext;
    typedef typename Base::WriteInfo WriteInfo;

    void canWriteInterruptHandler()
    {
        while(Base::device_.canWrite(InterruptContext())) {
            if ((info_.start_ + info_.bufSize_) <= info_.current_) {
                // The device control object mustn't allow it.
                GASSERT(0);
                break;
            }

            Base::device_.write(*info_.current_, InterruptContext());
            ++info_.current_;
        }
    }

    void writeCompleteInterruptHandler(const embxx::error::ErrorStatus& es)
    {
        invokeHandler(Base::el_, info_, es, true);
    }

    void initWrite(
        const CharType* buf,
        std::size_t size)
    {
        info_.start_ = buf;
        info_.current_ = buf;
        info_.bufSize_ = size;

        if (size == 0) {
            invokeHandler(Base::el_, info_, embxx::error::ErrorCode::Success, false);
            return;
        }

        Base::device_.startWrite(size, EventLoopContext());
    }

    WriteInfo info_;
};

template <typename TDevice,
          typename TEventLoop,
          typename THandler>
class CharacterWriteSupport<TDevice, TEventLoop, THandler, 0U>
{
protected:
    template <typename... TArgs>
    CharacterWriteSupport(TArgs&&...)
    {
    }
};

}  // namespace details

struct DefaultCharacterTraits
{
    typedef embxx::util::StaticFunction<void(const embxx::error::ErrorStatus&, std::size_t)> ReadHandler;
    typedef embxx::util::StaticFunction<void(const embxx::error::ErrorStatus&, std::size_t)> WriteHandler;
    typedef std::nullptr_t ReadUntilPred;
    static const std::size_t ReadQueueSize = 1;
    static const std::size_t WriteQueueSize = 1;
};

/// @addtogroup driver
/// @{

/// @brief Character device driver
/// @details Manages read/write operations on the character device (peripheral)
///          such as RS-232.
/// @tparam TDevice Platform specific device (peripheral) control class. It
///         must expose the following interface:
///         @code
///         // Define each character type as CharType
///         typedef ... CharType;
///
///         // Set the "can read" interrupt callback which has "void ()"
///         // signature. The callback must be called when there is at least
///         // one byte available. The callback will perform multiple canRead()
///         // and read() calls until canRead() returns false. This function is
///         // called in NON-interrupt (event loop) context when the driver
///         // object is constructed, but the callback itself is expected to
///         // be called by the device control object in interrupt context.
///         template <typename TFunc>
///         void setCanReadHandler(TFunc&& func);
///
///         // Set the "can write" interrupt callback which has "void ()"
///         // signature. The callback must be called when there is a space for
///         // at least one byte to be written. The callback will perform multiple
///         // canWrite() and write() calls until canWrite() returns false. This
///         // function is called in NON-interrupt (event loop) context when
///         // the driver object is constructed, but the callback itself is
///         // expected to be called by the device control object in interrupt
///         // context.
///         template <typename TFunc>
///         void setCanReadHandler(TFunc&& func);
///
///         // Set the "read complete" interrupt callback which has
///         // "void (const embxx::error::ErrorStatus&)" signature. The callback
///         // must be called when read operation is complete (either successfully
///         // or with errors) and read interrupts are disabled, i.e. no more
///         // "can read" callback calls will follow until next call to startRead().
///         // The error status passed as a parameter must indicate the status
///         // of the read operation. This function is called in NON-interrupt
///         // (event loop) context, but the callback itself is expected
///         // to be called by the device control object in interrupt context.
///         template <typename TFunc>
///         void setReadCompleteHandler(TFunc&& func);
///
///         // Set the "write complete" interrupt callback which has
///         // "void (const embxx::error::ErrorStatus&)" signature. The callback
///         // must be called when write operation is complete (either successfully
///         // or with errors) and write interrupts are disabled, i.e. no more
///         // "can write" callback calls will follow until next call to startWrite().
///         // The error status passed as a parameter must indicate the status
///         // of the write operation. This function is called in NON-interrupt
///         // (event loop) context, but the callback itself is expected
///         // to be called by the device control object in interrupt context.
///         template <typename TFunc>
///         void setWriteCompleteHandler(TFunc&& func);
///
///         // Start read operation. The device will perform some configuration
///         // if needed and enable read interrupts, i.e. interrupt when there
///         // is at least one character to read. The "context" is a dummy
///         // parameter that indicates whether the function is executed in
///         // NON-interrupt (event loop) or interrupt contexts. The function
///         // is called only in event loop context.
///         void startRead(std::size_t length, embxx::device::context::EventLoop context);
///
///         // Cancel read operation. The return value indicates whether the
///         // read operation was cancelled. The "context" is a dummy
///         // parameter that indicates whether the function is executed in
///         // NON-interrupt (event loop) or interrupt contexts. The read
///         // canellation in interrupt context may happen only when readUntil()
///         // request is used.
///         bool cancelRead(embxx::device::context::EventLoop context);
///         bool cancelRead(embxx::device::context::Interrupt context)
///
///         // Start write operation. The device will perform some configuration
///         // if needed and enable write interrupts, i.e. interrupt when there
///         // is space for at least one character to be written. The "context"
///         // is a dummy parameter that indicates whether the function is
///         // executed in NON-interrupt (event loop) or interrupt contexts.
///         // The function is called only in event loop context.
///         void startWrite(std::size_t length, embxx::device::context::EventLoop context);
///
///         // Cancel write operation. The return value indicates whether the
///         // read operation was cancelled. The "context" is a dummy
///         // parameter that indicates whether the function is executed in
///         // NON-interrupt (event loop) or interrupt contexts.
///         // The function is called only in event loop context.
///         bool cancelWrite(embxx::device::context::EventLoop context);
///
///         // Inquiry whether there is at least one character to be
///         // read. Will be called in the interrupt context. May be called
///         // multiple times in the same interrupt.
///         bool canRead(embxx::device::context::Interrupt context);
///
///         // Inquiry whether there is a space for at least one character to
///         // be written. Will be called in the interrupt context. May be called
///         // multiple times in the same interrupt.
///         bool canWrite(embxx::device::context::Interrupt context);
///
///         // Read one character. Precondition to this call: canRead() returns
///         // true. Will be called in the interrupt context. May be called
///         // multiple times in the same interrupt.
///         CharType read(embxx::device::context::Interrupt context);
///
///         // Write one character. Precondition to this call: canWrite() returns
///         // true. Will be called in the interrupt context. May be called
///         // multiple times in the same interrupt.
///         void write(CharType value, embxx::device::context::Interrupt context);
///         @endcode
///
/// @tparam TEventLoop A variant of embxx::util::EventLoop object that is used
///         to execute posted handlers in regular thread context.
/// @tparam TReadHandler A function class that is supposed to store "read"
///         complete callback. Must be either std::function or embxx::util::StaticFunction
///         and provide "void(const embxx::error::ErrorStatus&, std::size_t)"
///         calling interface, where the first parameter is error status of the
///         operation and second one is how many bytes were actually read in
///         the operation.
/// @tparam TWriteHandler A function class that is supposed to store "write"
///         complete callback. Must be either std::function or embxx::util::StaticFunction
///         and provide "void(const embxx::error::ErrorStatus&, std::size_t)"
///         calling interface, where the first parameter is error status of the
///         operation and second one is how many bytes were actually written in
///         the operation.
/// @tparam TReadUntilPred A function class that is supposed to store predicate
///         that evaluates the termination condition in case readUntil()
///         member function is called. It must be either std::function or
///         embxx::util::StaticFunction and provide "bool(typename TDevice::CharType)"
///         calling interface. It must return true if and only if the asynchronous
///         read request must be terminated with "Success" error status.
/// @headerfile embxx/driver/Character.h
template <typename TDevice,
          typename TEventLoop,
          typename TTraits = DefaultCharacterTraits>
class Character :
    public details::CharacterReadSupport<TDevice, TEventLoop, typename TTraits::ReadHandler, typename TTraits::ReadUntilPred, TTraits::ReadQueueSize>,
    public details::CharacterWriteSupport<TDevice, TEventLoop, typename TTraits::WriteHandler, TTraits::WriteQueueSize>
{
    typedef details::CharacterReadSupport<TDevice, TEventLoop, typename TTraits::ReadHandler, typename TTraits::ReadUntilPred, TTraits::ReadQueueSize> ReadBase;
    typedef details::CharacterWriteSupport<TDevice, TEventLoop, typename TTraits::WriteHandler, TTraits::WriteQueueSize> WriteBase;
public:

    /// @brief Device (peripheral) control class
    typedef TDevice Device;

    /// @brief Event loop class
    typedef TEventLoop EventLoop;

    /// @brief Traits class
    typedef TTraits Traits;

    /// @brief type of single character
    typedef typename Device::CharType CharType;

    /// @brief Type of read handler holder class
    typedef typename Traits::ReadHandler ReadHandler;

    /// @brief Type of write handler holder class
    typedef typename Traits::WriteHandler WriteHandler;

    /// @brief Type of read until predicate class
    typedef typename Traits::ReadUntilPred ReadUntilPred;

    /// @brief Maximum number of pending asynchronous read requests.
    static const std::size_t ReadQueueSize = Traits::ReadQueueSize;

    /// @brief Maximum number of pending asynchronous write requests.
    static const std::size_t WriteQueueSize = Traits::WriteQueueSize;

    /// @brief Constructor
    /// @param device Reference to device (peripheral) control object
    /// @param el Reference to event loop object
    Character(Device& device, EventLoop& el)
    : ReadBase(device, el),
      WriteBase(device, el)
    {
    }

    /// @brief Copy constructor is deleted.
    Character(const Character&) = delete;

    /// @brief MoveConstrutor is deleted
    Character(Character&&) = delete;

    /// @brief Destructor
    ~Character()
    {
    }

    /// @brief Copy assignment is deleted
    Character& operator=(const Character&) = delete;

    /// @brief Move assignment is deleted
    Character& operator=(Character&&) = delete;

    /// @brief Get reference to device (peripheral) control object.
    Device& device()
    {
        return deviceInternal(FromBase());
    }

    /// @brief Get referent to event loop object.
    EventLoop& eventLoop()
    {
        return eventLoopInternal(FromBase());
    }

    /// @brief Asynchronous read request
    /// @details The function returns immediately. The callback will be called
    ///          with operation results only when the provided buffer is full
    ///          or operation has been cancelled.
    /// @param buf Pointer to the output buffer. The buffer mustn't be
    ///            used or destructed until the callback has been called.
    /// @param size Size of the buffer.
    /// @param func Callback functor that must have following signature:
    ///        @code void callback(const embxx::error::ErrorStatus& status, std::size_t bytesRead); @endcode
    /// @pre The callback for any previous asyncRead() request has been called,
    ///      i.e. there is no outstanding asyncRead() request.
    /// @pre The provided buffer must stay valid and unused until the provided
    ///      callback function is called.
    template <typename TFunc>
    void asyncRead(
        CharType* buf,
        std::size_t size,
        TFunc&& func)
    {
        ReadBase::asyncRead(buf, size, std::forward<TFunc>(func));
    }

    /// @brief Asynchronous read until provided predicate is evaluated to true
    /// @details The function returns immediately. The callback will be called
    ///          with operation results if one of the three following conditions
    ///          is true:
    ///          @li the requested character was read. Error code will be equal
    ///              to embxx::error::ErrorCode::Success.
    ///          @li the buffer is full. Error code will be eqeal to
    ///              embxx::error::ErrorCode::BufferOverflow.
    ///          @li the read operation was cancelled by cancelRead(). Error code
    ///              will be equal to embxx::error::ErrorCode::Aborted.
    /// @param buf Pointer to the output buffer. The buffer mustn't be
    ///            used or destructed until the callback has been called.
    /// @param size Size of the buffer.
    /// @param pred Predicate object that must have the following signature:
    ///        @code bool predicate(typename TDevice::CharType);@endcode
    /// @param func Callback functor that must have following signature:
    ///        @code void callback(const embxx::error::ErrorStatus& status, std::size_t bytesRead);@endcode
    /// @pre The callback for any previous asyncRead() or asyncReadUntil() request
    ///      has been called, i.e. there is no outstanding asynchronous read request.
    /// @pre The provided buffer must stay valid and unused until the provided
    ///      callback function is called.
    template <typename TPred, typename TFunc>
    void asyncReadUntil(
        CharType* buf,
        std::size_t size,
        TPred&& pred,
        TFunc&& func)
    {
        ReadBase::asyncReadUntil(
            buf,
            size,
            std::forward<TPred>(pred),
            std::forward<TFunc>(func));
    }

    /// @brief Asynchronous read until specific character.
    /// @details Equivalent to:
    ///          @code
    ///          asyncReadUntil(
    ///              buf,
    ///              size,
    ///              [untilChar](CharType ch)->bool
    ///              {
    ///                  return ch == untilChar;
    ///              },
    ///              std::forward<TFunc>(func));
    ///           @endcode
    template <typename TFunc>
    void asyncReadUntil(
        CharType* buf,
        std::size_t size,
        CharType untilChar,
        TFunc&& func)
    {
        asyncReadUntil(
            buf,
            size,
            [untilChar](CharType ch)->bool
            {
                return ch == untilChar;
            },
            std::forward<TFunc>(func));
    }

    /// @brief Cancel previous asynchronous read request (asyncRead())
    /// @details If there is no unfinished asyncRead() operation in progress
    ///          the call to this function will have no effect. Otherwise the
    ///          callback will be called with embxx::error::ErrorCode::Aborted
    ///          as status value.
    /// @return true in case the previous asyncRead() operation was really
    ///         cancelled, false in case there was no unfinished asynchronous
    ///         read request.
    bool cancelRead()
    {
        return ReadBase::cancelRead();
    }

    /// @brief Asynchronous write request
    /// @details The function returns immediately. The callback will be called
    ///          with operation results only when all the data from provided
    ///          buffer has been sent or operation has been cancelled.
    /// @param buf Pointer to the read-only buffer. The buffer mustn't be
    ///            changed or destructed until the callback has been called.
    /// @param size Size of the buffer.
    /// @param func Callback functor that must have following signature:
    ///        @code void callback(const embxx::error::ErrorStatus& status, std::size_t bytesWritten); @endcode
    /// @pre The callback for any previous asyncWrite() request has been called,
    ///      i.e. there is no outstanding asyncWrite() request.
    /// @pre The provided buffer must stay valid and unused until the provided
    ///      callback function is called.
    template <typename TFunc>
    void asyncWrite(
        const CharType* buf,
        std::size_t size,
        TFunc&& func)
    {
        WriteBase::asyncWrite(buf, size, std::forward<TFunc>(func));
    }

    /// @brief Cancel previous asynchronous write request (asyncWrite())
    /// @details If there is no unfinished asyncWrite() operation in progress
    ///          the call to this function will have no effect. Otherwise the
    ///          callback will be called with embxx::error::ErrorCode::Aborted
    ///          as status value.
    /// @return true in case the previous asyncWrite() operation was really
    ///         cancelled, false in case there was no unfinished asynchronous
    ///         write request.
    bool cancelWrite()
    {
        return WriteBase::cancelWrite();
    }

private:

    struct FromReadBase {};
    struct FromWriteBase {};
    typedef typename
        std::conditional<
            0U < ReadQueueSize,
            FromReadBase,
            FromWriteBase
        >::type FromBase;

    Device& deviceInternal(FromReadBase)
    {
        return ReadBase::device_;
    }

    Device& deviceInternal(FromWriteBase)
    {
        return WriteBase::device_;
    }

    EventLoop& eventLoopInternal(FromReadBase)
    {
        return ReadBase::el_;
    }

    EventLoop& eventLoopInternal(FromWriteBase)
    {
        return WriteBase::el_;
    }
};

/// @}

}  // namespace driver

}  // namespace embxx




