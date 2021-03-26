#include "serialization_transport.h"

#include "ble.h"
#include "ble_app.h"
#include "log_helper.h"
#include "nrf_error.h"

#include "ble_common.h"

#include <iterator>
#include <memory>
#include <sstream>

SerializationTransport::SerializationTransport(H5Transport *dataLinkLayer,
                                               uint32_t response_timeout)
    : statusCallback(nullptr)
    , eventCallback(nullptr)
    , logCallback(nullptr)
    , responseReceived(false)
    , responseBuffer(nullptr)
    , processEvents(false)
    , isOpen(false)
{
    // SerializationTransport takes ownership of dataLinkLayer provided object
    nextTransportLayer = std::shared_ptr<H5Transport>(dataLinkLayer);
    responseTimeout    = response_timeout;
}

SerializationTransport::~SerializationTransport()
{
    if (eventThread.joinable())
    {
        eventThread.join();
    }
}

uint32_t SerializationTransport::open(const status_cb_t &status_callback,
                                      const evt_cb_t &event_callback,
                                      const log_cb_t &log_callback) noexcept
{
    std::lock_guard<std::recursive_mutex> openLck(isOpenMutex);

    if (isOpen)
    {
        return NRF_ERROR_SD_RPC_SERIALIZATION_TRANSPORT_ALREADY_OPEN;
    }

    statusCallback = status_callback;
    eventCallback  = event_callback;
    logCallback    = log_callback;

    const auto dataCallback = std::bind(&SerializationTransport::readHandler, this,
                                        std::placeholders::_1, std::placeholders::_2);

    const auto errorCode = nextTransportLayer->open(status_callback, dataCallback, log_callback);

    if (errorCode != NRF_SUCCESS)
    {
        return errorCode;
    }

    isOpen = true;

    // Thread should not be running from before when calling this
    if (!eventThread.joinable())
    {
        // If ::close is called when this method (::open) returns
        // and eventThread is executing somewhere between while (isOpen)
        // and eventWaitCondition notification we could get a deadlock.
        //
        // To prevent this, lock eventMutex in this thread, let eventThread
        // wait until eventMutex is unlocked (.wait in this thread).
        // When eventThread is started and outside critical region,
        // let eventThread notify eventWaitCondition, making .wait
        // return
        std::unique_lock<std::mutex> eventLock(eventMutex);
        processEvents = true;
        eventThread   = std::thread([this] { eventHandlingRunner(); });
        eventWaitCondition.wait(eventLock);
    }
    else
    {
        return NRF_ERROR_SD_RPC_SERIALIZATION_TRANSPORT;
    }

    return NRF_SUCCESS;
}

uint32_t SerializationTransport::close() noexcept
{
    // Stop event processing thread before closing since
    // event callbacks may in application space invoke new calls to SerializationTransport
    {
        std::unique_lock<std::mutex> eventLock(eventMutex);
        processEvents = false;
        eventWaitCondition.notify_all();
    }

    if (eventThread.joinable())
    {
        if (std::this_thread::get_id() == eventThread.get_id())
        {
            // log "ser_app_hal_pc_event_handling_stop was called from an event callback, causing
            // the event thread to stop itself. This will cause a resource leak."
            return NRF_ERROR_SD_RPC_SERIALIZATION_TRANSPORT;
        }

        try
        {
            eventThread.join();
        }
        catch (const std::system_error &)
        {
            return NRF_ERROR_SD_RPC_SERIALIZATION_TRANSPORT_INVALID_STATE;
        }
    }

    // Close this and the underlying transport
    std::lock_guard<std::recursive_mutex> openLck(isOpenMutex);

    if (!isOpen)
    {
        return NRF_ERROR_SD_RPC_SERIALIZATION_TRANSPORT_ALREADY_CLOSED;
    }

    isOpen = false;

    return nextTransportLayer->close();
}

uint32_t SerializationTransport::send(const std::vector<uint8_t> &cmdBuffer,
                                      std::shared_ptr<std::vector<uint8_t>> rspBuffer,
                                      serialization_pkt_type_t pktType) noexcept
{
    std::lock_guard<std::recursive_mutex> openLck(isOpenMutex);

    if (!isOpen)
    {
        return NRF_ERROR_SD_RPC_SERIALIZATION_TRANSPORT_INVALID_STATE;
    }

    // Mutex to avoid multiple threads sending commands at the same time.
    std::lock_guard<std::mutex> sendGuard(sendMutex);
    responseReceived = false;
    responseBuffer   = rspBuffer;

    std::vector<uint8_t> commandBuffer(cmdBuffer.size() + 1);
    commandBuffer[0] = pktType;
    std::copy(cmdBuffer.begin(), cmdBuffer.end(), commandBuffer.begin() + 1);

    const auto errCode = nextTransportLayer->send(commandBuffer);

    if (errCode != NRF_SUCCESS)
    {
        return errCode;
    }

    if (!rspBuffer)
    {
        return NRF_SUCCESS;
    }

    std::unique_lock<std::mutex> responseGuard(responseMutex);

    const std::chrono::milliseconds timeout(responseTimeout);
    const auto wakeupTime = std::chrono::system_clock::now() + timeout;

    responseWaitCondition.wait_until(responseGuard, wakeupTime, [&] { return responseReceived; });

    if (!responseReceived)
    {
        getLogger()->warn("Failed to receive response for command");
        return NRF_ERROR_SD_RPC_SERIALIZATION_TRANSPORT_NO_RESPONSE;
    }

    return NRF_SUCCESS;
}

void SerializationTransport::drainEventQueue()
{
    std::unique_lock<std::mutex> eventLock(eventMutex);

    // Drain the queue for any old events
    try
    {
        while (!eventQueue.empty())
        {
            eventQueue.pop();
        }
    }
    catch (const std::exception &ex)
    {
        LogHelper::tryToLogException(spdlog::level::err, ex,
                                     "Error in SerializationTransport::drainEventQueue");
    }
}

// Event Thread
void SerializationTransport::eventHandlingRunner() noexcept
{
    try
    {
        drainEventQueue();

        std::unique_lock<std::mutex> eventLock(eventMutex);

        while (processEvents)
        {
            // Suspend this thread until event wait condition
            // is notified. This can happen from ::close and
            // ::readHandler (thread in H5Transport)
            eventWaitCondition.notify_all();
            eventWaitCondition.wait(eventLock);

            while (!eventQueue.empty() && processEvents)
            {
                // Get oldest event received from UART thread
                const auto eventData     = eventQueue.front();
                const auto eventDataSize = static_cast<uint32_t>(eventData.size());

                // Remove oldest event received from H5Transport thread
                eventQueue.pop();

                // Let UART thread add events to eventQueue
                // while popped event is processed
                eventLock.unlock();

                // Set codec context
                EventCodecContext context(this);

                // Allocate memory to store decoded event including an unknown quantity of padding
                auto possibleEventLength = static_cast<uint32_t>(MaxPossibleEventLength);
                std::vector<uint8_t> eventDecodeBuffer;
                eventDecodeBuffer.resize(MaxPossibleEventLength);
                const auto event = reinterpret_cast<ble_evt_t *>(eventDecodeBuffer.data());

                // Decode event
                const auto errCode =
                    ble_event_dec(eventData.data(), eventDataSize, event, &possibleEventLength);

                if (eventCallback && errCode == NRF_SUCCESS)
                {
                    eventCallback(event);
                }

                if (errCode != NRF_SUCCESS)
                {
                    const auto errmsg = fmt::format(
                        "Failed to decode event, error code is {}/{:#04x}", errCode, errCode);
                    getLogger()->error(errmsg);
                    statusCallback(PKT_DECODE_ERROR, errmsg);
                }

                // Prevent UART from adding events to eventQueue
                eventLock.lock();
            }
        }

        eventWaitCondition.notify_all();
    }
    catch (const std::exception &e)
    {
        LogHelper::tryToLogException(spdlog::level::critical, e,
                                     "Error in SerializationTransport::eventHandlingRunner");
    }
}

void SerializationTransport::readHandler(const uint8_t *data, const size_t length)
{
    const auto eventType = static_cast<serialization_pkt_type_t>(data[0]);

    const auto startOfData  = data + 1;
    const size_t dataLength = length - 1;

    if (eventType == SERIALIZATION_RESPONSE)
    {
        if (responseBuffer && !responseBuffer->empty())
        {
            if (responseBuffer->size() >= dataLength)
            {
                std::copy(startOfData, startOfData + dataLength, responseBuffer->begin());
                responseBuffer->resize(dataLength);
            }
            else
            {
                getLogger()->error("Received SERIALIZATION_RESPONSE with a packet that is larger "
                                   "than the allocated buffer.");
            }
        }
        else
        {
            getLogger()->error("Received SERIALIZATION_RESPONSE but command did not provide a "
                               "buffer for the reply.");
        }

        std::lock_guard<std::mutex> responseGuard(responseMutex);
        responseReceived = true;
        responseWaitCondition.notify_one();
    }
    else if (eventType == SERIALIZATION_EVENT)
    {
        std::vector<uint8_t> event;
        event.reserve(dataLength);
        std::copy(startOfData, startOfData + dataLength, std::back_inserter(event));
        std::lock_guard<std::mutex> eventLock(eventMutex);
        eventQueue.push(std::move(event));
        eventWaitCondition.notify_one();
    }
    else
    {
        getLogger()->warn("Unknown Nordic Semiconductor vendor specific packet received");
    }
}
