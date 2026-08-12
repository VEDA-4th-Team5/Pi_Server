#include "mqtt/MqttTransport.hpp"

#include <mosquitto.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace mqtt {
namespace {

using namespace std::chrono_literals;

constexpr int kNetworkLoopTimeoutMs = 100;
constexpr int kSubscriptionRejected = 0x80;
constexpr auto kReconnectDelay = 250ms;
constexpr std::size_t kPreReadyMessageCapacity = 256;
constexpr std::size_t kPreReadyByteCapacity = 2 * 1024 * 1024;
constexpr std::size_t kMqttMidSlots = 65536;
constexpr std::size_t kRegularPublishInFlightCapacity = 32;
constexpr std::uint8_t kPublishNone = 0;
constexpr std::uint8_t kPublishRegular = 1;
constexpr std::uint8_t kPublishTracked = 2;

std::mutex g_libraryMutex;
std::size_t g_libraryUsers{};

bool acquireMosquittoLibrary() {
    std::lock_guard lock(g_libraryMutex);
    if (g_libraryUsers == 0 && mosquitto_lib_init() != MOSQ_ERR_SUCCESS)
        return false;
    ++g_libraryUsers;
    return true;
}

void releaseMosquittoLibrary() noexcept {
    std::lock_guard lock(g_libraryMutex);
    if (g_libraryUsers == 0) return;
    --g_libraryUsers;
    if (g_libraryUsers == 0) mosquitto_lib_cleanup();
}

class MosquittoTransport final : public IMqttTransport {
public:
    ~MosquittoTransport() override {
        if (!stopAndJoin()) std::terminate();
    }

    bool start(const MqttConnectionOptions& options,
               const std::vector<MqttSubscription>& subscriptions,
               MessageCallback messageCallback,
               ObserverCallbacks observerCallbacks) override {
        std::lock_guard stopLock(stopMutex_);
        {
            std::lock_guard lock(mutex_);
            if (started_ || libraryAcquired_ || supervisor_.joinable())
                return false;
        }
        if (!acquireMosquittoLibrary()) return false;

        {
            std::lock_guard lock(mutex_);
            libraryAcquired_ = true;
            options_ = options;
            subscriptions_ = subscriptions;
            messageCallback_ = std::move(messageCallback);
            observerCallbacks_ = std::move(observerCallbacks);
            stopping_ = false;
            started_ = true;
            epochCounter_ = 0;
        }

        try {
            supervisor_ = std::thread([this] { supervisorLoop(); });
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                resetStoppedStateUnlocked();
                libraryAcquired_ = false;
            }
            releaseMosquittoLibrary();
            return false;
        }
        return true;
    }

    MqttPublishResult publish(const std::string& topic,
                              const std::string& payload,
                              const int qos,
                              const bool retain) override {
        std::lock_guard lock(mutex_);
        // Every publisher sharing this handle must use a protocol packet ID.
        // QoS0 local completion callbacks can reuse an outstanding QoS1 MID
        // and therefore cannot coexist with exact Fire PUBACK correlation.
        if (!canPublishUnlocked() || qos != 1 ||
            regularPublishInFlight_ >= kRegularPublishInFlightCapacity) {
            return {};
        }
        int messageId{-1};
        const int result = mosquitto_publish(
            mosq_, &messageId, topic.c_str(),
            static_cast<int>(payload.size()), payload.data(), qos, retain);
        if (result != MOSQ_ERR_SUCCESS || !registerPublishUnlocked(
                messageId, kPublishRegular)) {
            abortEpochUnlocked();
            return {};
        }
        return {true, messageId};
    }

    MqttPublishResult publishInEpoch(
        const std::string& topic,
        const std::string& payload,
        const int qos,
        const bool retain,
        const MqttConnectionEpoch expectedEpoch) override {
        std::lock_guard lock(mutex_);
        if (!canPublishUnlocked() || !subscriptionsReady_ || qos != 1 ||
            expectedEpoch == 0 || activeEpoch_ != expectedEpoch ||
            regularPublishInFlight_ >= kRegularPublishInFlightCapacity) {
            return {};
        }
        int messageId{-1};
        const int result = mosquitto_publish(
            mosq_, &messageId, topic.c_str(),
            static_cast<int>(payload.size()), payload.data(), qos, retain);
        if (result != MOSQ_ERR_SUCCESS || !registerPublishUnlocked(
                messageId, kPublishRegular)) {
            abortEpochUnlocked();
            return {};
        }
        return {true, messageId};
    }

    MqttTrackedPublishResult publishTracked(
        const std::string& topic,
        const std::string& payload,
        const bool retain,
        MqttPublishCorrelation correlation,
        const MqttConnectionEpoch expectedEpoch) override {
        std::lock_guard lock(mutex_);
        if (!canPublishUnlocked() || !subscriptionsReady_ ||
            expectedEpoch == 0 || activeEpoch_ != expectedEpoch ||
            correlation.deliveryKey.empty() || correlation.revision == 0 ||
            correlation.attemptId.empty()) {
            return {};
        }

        int messageId{-1};
        const int result = mosquitto_publish(
            mosq_, &messageId, topic.c_str(),
            static_cast<int>(payload.size()), payload.data(), 1, retain);
        if (result != MOSQ_ERR_SUCCESS || !registerPublishUnlocked(
                messageId, kPublishTracked)) {
            abortEpochUnlocked();
            return {};
        }

        return {true,
                {activeEpoch_, messageId, std::move(correlation)}};
    }

    bool abortActiveEpoch(
        const MqttConnectionEpoch expectedEpoch) noexcept override {
        std::lock_guard lock(mutex_);
        if (!canPublishUnlocked() || activeEpoch_ != expectedEpoch)
            return false;
        abortEpochUnlocked();
        return true;
    }

    bool stopAndJoin() noexcept override {
        std::lock_guard stopLock(stopMutex_);
        bool releaseLibrary{};
        {
            std::lock_guard lock(mutex_);
            if (!libraryAcquired_) return true;
            stopping_ = true;
            started_ = false;
            connected_ = false;
            subscriptionsReady_ = false;
            if (mosq_) (void)mosquitto_disconnect(mosq_);
        }
        wakeCondition_.notify_all();

        if (supervisor_.joinable()) supervisor_.join();

        {
            std::lock_guard lock(mutex_);
            releaseLibrary = libraryAcquired_;
            libraryAcquired_ = false;
            resetStoppedStateUnlocked();
        }
        if (releaseLibrary) releaseMosquittoLibrary();
        return true;
    }

private:
    struct EpochContext {
        MosquittoTransport* owner{};
        MqttConnectionEpoch epoch{};
    };

    struct BufferedMessage {
        std::string topic;
        std::string payload;
    };

    bool registerPublishUnlocked(const int messageId,
                                 const std::uint8_t kind) noexcept {
        if (messageId <= 0 ||
            static_cast<std::size_t>(messageId) >= pendingPublishKinds_.size() ||
            pendingPublishKinds_[static_cast<std::size_t>(messageId)] !=
                kPublishNone) {
            return false;
        }
        pendingPublishKinds_[static_cast<std::size_t>(messageId)] = kind;
        if (kind == kPublishRegular) ++regularPublishInFlight_;
        return true;
    }

    void abortEpochUnlocked() noexcept {
        if (activeEpoch_ == 0) return;
        epochAbort_ = true;
        connected_ = false;
        subscriptionsReady_ = false;
        if (mosq_) (void)mosquitto_disconnect(mosq_);
        wakeCondition_.notify_all();
    }

    void supervisorLoop() noexcept {
        while (true) {
            {
                std::lock_guard lock(mutex_);
                if (stopping_) break;
            }

            const MqttConnectionEpoch epoch = nextEpoch();
            auto context = std::make_unique<EpochContext>(
                EpochContext{this, epoch});
            mosquitto* handle = mosquitto_new(
                options_.clientId.c_str(), true, context.get());
            if (!handle) {
                emitFact({MqttTransportFactKind::EpochFaulted, epoch, -1,
                          MOSQ_ERR_NOMEM, {}, {}});
                if (!waitBeforeReconnect()) break;
                continue;
            }

            mosquitto_message_callback_set(handle,
                                           &MosquittoTransport::onMessage);
            mosquitto_connect_callback_set(handle,
                                           &MosquittoTransport::onConnect);
            mosquitto_disconnect_callback_set(
                handle, &MosquittoTransport::onDisconnect);
            mosquitto_subscribe_callback_set(
                handle, &MosquittoTransport::onSubscribe);
            mosquitto_publish_callback_set(handle,
                                           &MosquittoTransport::onPublish);
            if (mosquitto_threaded_set(handle, true) != MOSQ_ERR_SUCCESS) {
                mosquitto_destroy(handle);
                emitFact({MqttTransportFactKind::EpochFaulted, epoch, -1,
                          MOSQ_ERR_INVAL, {}, {}});
                if (!waitBeforeReconnect()) break;
                continue;
            }

            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    mosquitto_destroy(handle);
                    break;
                }
                mosq_ = handle;
                epochContext_ = std::move(context);
                activeEpoch_ = epoch;
                connected_ = false;
                subscriptionsSubmitted_ = false;
                subscriptionsReady_ = false;
                epochAbort_ = false;
                disconnectFactEmitted_ = false;
                pendingSubscriptions_.clear();
                pendingPublishKinds_.fill(kPublishNone);
                regularPublishInFlight_ = 0;
                preReadyMessages_.clear();
                preReadyMessageBytes_ = 0;
            }

            // mosquitto_connect_async() requires libmosquitto's own
            // loop_start() interface. We intentionally use the synchronous
            // connect plus a manually owned loop so libmosquitto cannot reuse
            // this handle through its automatic reconnect path.
            int loopResult = mosquitto_connect(
                handle, options_.host.c_str(), options_.port,
                options_.keepAliveSeconds);
            if (loopResult != MOSQ_ERR_SUCCESS) {
                emitFact({MqttTransportFactKind::EpochFaulted, epoch, -1,
                          loopResult, {}, {}});
            } else {
                while (true) {
                    {
                        std::lock_guard lock(mutex_);
                        if (stopping_ || epochAbort_) break;
                    }

                    loopResult = mosquitto_loop(
                        handle, kNetworkLoopTimeoutMs, 1);
                    if (loopResult != MOSQ_ERR_SUCCESS) break;
                    submitSubscriptionsIfConnected(handle, epoch);
                }
            }

            bool emitSyntheticDisconnect{};
            int disconnectReason = loopResult;
            std::unique_ptr<EpochContext> retiredContext;
            {
                std::lock_guard lock(mutex_);
                if (mosq_ == handle && activeEpoch_ == epoch) {
                    connected_ = false;
                    subscriptionsReady_ = false;
                    emitSyntheticDisconnect = !disconnectFactEmitted_;
                    if (epochAbort_ && disconnectReason == MOSQ_ERR_SUCCESS)
                        disconnectReason = MOSQ_ERR_CONN_LOST;
                    mosq_ = nullptr;
                    activeEpoch_ = 0;
                    pendingSubscriptions_.clear();
                    pendingPublishKinds_.fill(kPublishNone);
                    regularPublishInFlight_ = 0;
                    preReadyMessages_.clear();
                    preReadyMessageBytes_ = 0;
                    retiredContext = std::move(epochContext_);
                }
            }
            if (emitSyntheticDisconnect) {
                emitFact({MqttTransportFactKind::Disconnected, epoch, -1,
                          disconnectReason, {}, {}});
            }

            mosquitto_destroy(handle);
            retiredContext.reset();

            if (!waitBeforeReconnect()) break;
        }
    }

    MqttConnectionEpoch nextEpoch() noexcept {
        std::lock_guard lock(mutex_);
        ++epochCounter_;
        if (epochCounter_ == 0) ++epochCounter_;
        return epochCounter_;
    }

    bool waitBeforeReconnect() noexcept {
        std::unique_lock lock(mutex_);
        return !wakeCondition_.wait_for(
            lock, kReconnectDelay, [this] { return stopping_; });
    }

    void submitSubscriptionsIfConnected(mosquitto* handle,
                                        const MqttConnectionEpoch epoch) {
        std::vector<MqttSubscription> subscriptions;
        {
            std::lock_guard lock(mutex_);
            if (mosq_ != handle || activeEpoch_ != epoch || !connected_ ||
                subscriptionsSubmitted_ || stopping_ || epochAbort_) {
                return;
            }
            subscriptionsSubmitted_ = true;
            subscriptions = subscriptions_;
        }

        if (subscriptions.empty()) {
            {
                std::lock_guard lock(mutex_);
                if (mosq_ != handle || activeEpoch_ != epoch || !connected_)
                    return;
                subscriptionsReady_ = true;
            }
            emitFact({MqttTransportFactKind::SubscriptionsReady, epoch, -1,
                      0, {}, {}});
            return;
        }

        for (const auto& subscription : subscriptions) {
            int messageId{-1};
            const int result = mosquitto_subscribe(
                handle, &messageId, subscription.topic.c_str(),
                subscription.qos);
            if (result != MOSQ_ERR_SUCCESS) {
                {
                    std::lock_guard lock(mutex_);
                    if (mosq_ == handle && activeEpoch_ == epoch) {
                        epochAbort_ = true;
                        connected_ = false;
                    }
                }
                emitFact({MqttTransportFactKind::EpochFaulted, epoch,
                          messageId, result, subscription.topic, {}});
                return;
            }

            bool duplicateMessageId{};
            {
                std::lock_guard lock(mutex_);
                if (mosq_ != handle || activeEpoch_ != epoch || stopping_ ||
                    epochAbort_) {
                    return;
                }
                const auto inserted = pendingSubscriptions_.emplace(
                    messageId, subscription);
                if (!inserted.second) {
                    epochAbort_ = true;
                    connected_ = false;
                    duplicateMessageId = true;
                }
            }
            if (duplicateMessageId) {
                emitFact({MqttTransportFactKind::EpochFaulted, epoch,
                          messageId, MOSQ_ERR_INVAL,
                          subscription.topic, {}});
                return;
            }
        }
    }

    static void onMessage(mosquitto* handle,
                          void* userdata,
                          const mosquitto_message* message) noexcept {
        auto* context = static_cast<EpochContext*>(userdata);
        if (!context || !context->owner || !message || !message->topic) return;
        context->owner->handleMessage(handle, *context, *message);
    }

    static void onConnect(mosquitto* handle,
                          void* userdata,
                          const int result) noexcept {
        auto* context = static_cast<EpochContext*>(userdata);
        if (!context || !context->owner) return;
        context->owner->handleConnect(handle, *context, result);
    }

    static void onDisconnect(mosquitto* handle,
                             void* userdata,
                             const int reason) noexcept {
        auto* context = static_cast<EpochContext*>(userdata);
        if (!context || !context->owner) return;
        context->owner->handleDisconnect(handle, *context, reason);
    }

    static void onSubscribe(mosquitto* handle,
                            void* userdata,
                            const int messageId,
                            const int qosCount,
                            const int* grantedQos) noexcept {
        auto* context = static_cast<EpochContext*>(userdata);
        if (!context || !context->owner) return;
        context->owner->handleSubscribe(
            handle, *context, messageId, qosCount, grantedQos);
    }

    static void onPublish(mosquitto* handle,
                          void* userdata,
                          const int messageId) noexcept {
        auto* context = static_cast<EpochContext*>(userdata);
        if (!context || !context->owner) return;
        context->owner->handlePublish(handle, *context, messageId);
    }

    void handleMessage(mosquitto* handle,
                       const EpochContext& context,
                       const mosquitto_message& message) noexcept {
        try {
            const std::size_t topic_size = std::strlen(message.topic);
            const std::size_t payload_size = message.payloadlen > 0
                ? static_cast<std::size_t>(message.payloadlen)
                : 0;
            if (topic_size > kPreReadyByteCapacity ||
                payload_size > kPreReadyByteCapacity - topic_size) {
                {
                    std::lock_guard lock(mutex_);
                    if (isActiveEpochUnlocked(handle, context))
                        abortEpochUnlocked();
                }
                emitFact({MqttTransportFactKind::EpochFaulted,
                          context.epoch, -1, MOSQ_ERR_PAYLOAD_SIZE, {}, {}});
                return;
            }
            const std::size_t message_size = topic_size + payload_size;
            BufferedMessage owned;
            owned.topic = message.topic;
            if (message.payload && message.payloadlen > 0) {
                owned.payload.assign(
                    static_cast<const char*>(message.payload),
                    static_cast<std::size_t>(message.payloadlen));
            }

            MessageCallback callback;
            bool overflow{};
            {
                std::lock_guard lock(mutex_);
                if (!isActiveEpochUnlocked(handle, context) || !connected_ ||
                    stopping_) {
                    return;
                }
                if (!subscriptionsReady_) {
                    if (preReadyMessages_.size() >=
                            kPreReadyMessageCapacity ||
                        preReadyMessageBytes_ >
                            kPreReadyByteCapacity - message_size) {
                        overflow = true;
                        abortEpochUnlocked();
                    } else {
                        preReadyMessages_.push_back(std::move(owned));
                        preReadyMessageBytes_ += message_size;
                    }
                } else {
                    callback = messageCallback_;
                }
            }
            if (overflow) {
                emitFact({MqttTransportFactKind::EpochFaulted,
                          context.epoch, -1, MOSQ_ERR_NOMEM, {}, {}});
                return;
            }
            if (callback) callback(owned.topic, owned.payload);
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                if (isActiveEpochUnlocked(handle, context))
                    abortEpochUnlocked();
            }
            emitFact({MqttTransportFactKind::EpochFaulted,
                      context.epoch, -1, MOSQ_ERR_NOMEM, {}, {}});
        }
    }

    void handleConnect(mosquitto* handle,
                       const EpochContext& context,
                       const int result) noexcept {
        bool connected{};
        {
            std::lock_guard lock(mutex_);
            if (!isActiveEpochUnlocked(handle, context) || stopping_) return;
            connected = result == 0;
            connected_ = connected;
            if (!connected) epochAbort_ = true;
        }

        if (connected) {
            emitFact({MqttTransportFactKind::Connected, context.epoch, -1,
                      0, {}, {}});
        } else {
            emitFact({MqttTransportFactKind::EpochFaulted, context.epoch, -1,
                      result, {}, {}});
        }
    }

    void handleDisconnect(mosquitto* handle,
                          const EpochContext& context,
                          const int reason) noexcept {
        {
            std::lock_guard lock(mutex_);
            if (!isActiveEpochUnlocked(handle, context) ||
                disconnectFactEmitted_) {
                return;
            }
            connected_ = false;
            subscriptionsReady_ = false;
            epochAbort_ = true;
            disconnectFactEmitted_ = true;
        }
        emitFact({MqttTransportFactKind::Disconnected, context.epoch, -1,
                  reason, {}, {}});
    }

    void handleSubscribe(mosquitto* handle,
                         const EpochContext& context,
                         const int messageId,
                         const int qosCount,
                         const int* grantedQos) noexcept {
        MqttTransportFact fact{
            MqttTransportFactKind::SubscriptionAcknowledged,
            context.epoch, messageId, 0, {}, {}};
        bool ready{};
        bool failed{};
        std::deque<BufferedMessage> buffered;
        {
            std::lock_guard lock(mutex_);
            if (!isActiveEpochUnlocked(handle, context) || !connected_ ||
                stopping_) {
                return;
            }
            const auto found = pendingSubscriptions_.find(messageId);
            if (found == pendingSubscriptions_.end()) return;
            fact.subscriptionTopic = found->second.topic;
            if (grantedQos && qosCount > 0) {
                fact.grantedQos.assign(grantedQos, grantedQos + qosCount);
            }
            failed = fact.grantedQos.empty() ||
                     std::any_of(fact.grantedQos.begin(),
                                 fact.grantedQos.end(),
                                 [](const int qos) {
                                     return qos == kSubscriptionRejected;
                                 });
            fact.reason = failed ? kSubscriptionRejected : 0;
            pendingSubscriptions_.erase(found);
            if (failed) {
                connected_ = false;
                epochAbort_ = true;
            } else if (pendingSubscriptions_.empty()) {
                subscriptionsReady_ = true;
                ready = true;
                buffered.swap(preReadyMessages_);
                preReadyMessageBytes_ = 0;
            }
        }

        emitFact(fact);
        if (failed) {
            emitFact({MqttTransportFactKind::EpochFaulted, context.epoch,
                      messageId, kSubscriptionRejected,
                      fact.subscriptionTopic, fact.grantedQos});
        } else if (ready) {
            emitFact({MqttTransportFactKind::SubscriptionsReady,
                      context.epoch, -1, 0, {}, {}});
            for (auto& message : buffered) {
                MessageCallback callback;
                {
                    std::lock_guard lock(mutex_);
                    if (!isActiveEpochUnlocked(handle, context) ||
                        !connected_ || !subscriptionsReady_ || stopping_) {
                        return;
                    }
                    callback = messageCallback_;
                }
                try {
                    if (callback) callback(message.topic, message.payload);
                } catch (...) {
                    // Application exceptions stay outside the C callback.
                }
            }
        }
    }

    void handlePublish(mosquitto* handle,
                       const EpochContext& context,
                       const int messageId) noexcept {
        bool tracked{};
        {
            std::lock_guard lock(mutex_);
            if (!isActiveEpochUnlocked(handle, context) || !connected_ ||
                stopping_ || messageId <= 0 ||
                static_cast<std::size_t>(messageId) >=
                    pendingPublishKinds_.size()) {
                return;
            }
            auto& kind = pendingPublishKinds_[
                static_cast<std::size_t>(messageId)];
            if (kind == kPublishNone) return;
            tracked = kind == kPublishTracked;
            if (kind == kPublishRegular && regularPublishInFlight_ > 0)
                --regularPublishInFlight_;
            kind = kPublishNone;
        }
        if (!tracked) return;
        emitFact({MqttTransportFactKind::PublishAcknowledged,
                  context.epoch, messageId, 0, {}, {}});
    }

    void emitFact(const MqttTransportFact& fact) noexcept {
        ObserverCallbacks observers;
        {
            std::lock_guard lock(mutex_);
            observers = observerCallbacks_;
        }
        try {
            if (observers.onFact) observers.onFact(fact);
            switch (fact.kind) {
                case MqttTransportFactKind::Connected:
                    if (observers.onConnected) observers.onConnected();
                    break;
                case MqttTransportFactKind::Disconnected:
                    if (observers.onDisconnected)
                        observers.onDisconnected(fact.reason);
                    break;
                case MqttTransportFactKind::PublishAcknowledged:
                    if (observers.onPublished)
                        observers.onPublished(fact.messageId);
                    break;
                default:
                    break;
            }
        } catch (...) {
            // Observer failures are isolated from the MQTT network loop.
        }
    }

    [[nodiscard]] bool canPublishUnlocked() const noexcept {
        return started_ && !stopping_ && connected_ && mosq_ &&
               activeEpoch_ != 0;
    }

    [[nodiscard]] bool isActiveEpochUnlocked(
        mosquitto* handle,
        const EpochContext& context) const noexcept {
        return mosq_ == handle && epochContext_.get() == &context &&
               activeEpoch_ == context.epoch;
    }

    void resetStoppedStateUnlocked() noexcept {
        started_ = false;
        stopping_ = false;
        connected_ = false;
        subscriptionsSubmitted_ = false;
        subscriptionsReady_ = false;
        epochAbort_ = false;
        disconnectFactEmitted_ = false;
        activeEpoch_ = 0;
        mosq_ = nullptr;
        epochContext_.reset();
        pendingSubscriptions_.clear();
        pendingPublishKinds_.fill(kPublishNone);
        regularPublishInFlight_ = 0;
        preReadyMessages_.clear();
        preReadyMessageBytes_ = 0;
        subscriptions_.clear();
        options_ = {};
        messageCallback_ = {};
        observerCallbacks_ = {};
    }

    std::mutex mutex_;
    std::mutex stopMutex_;
    std::condition_variable wakeCondition_;
    std::thread supervisor_;
    mosquitto* mosq_{};
    std::unique_ptr<EpochContext> epochContext_;
    MqttConnectionOptions options_;
    std::vector<MqttSubscription> subscriptions_;
    std::unordered_map<int, MqttSubscription> pendingSubscriptions_;
    std::array<std::uint8_t, kMqttMidSlots> pendingPublishKinds_{};
    std::size_t regularPublishInFlight_{};
    std::deque<BufferedMessage> preReadyMessages_;
    std::size_t preReadyMessageBytes_{};
    MessageCallback messageCallback_;
    ObserverCallbacks observerCallbacks_;
    MqttConnectionEpoch epochCounter_{};
    MqttConnectionEpoch activeEpoch_{};
    bool libraryAcquired_{false};
    bool started_{false};
    bool stopping_{false};
    bool connected_{false};
    bool subscriptionsSubmitted_{false};
    bool subscriptionsReady_{false};
    bool epochAbort_{false};
    bool disconnectFactEmitted_{false};
};

}  // namespace

std::unique_ptr<IMqttTransport> makeMosquittoTransport() {
    return std::make_unique<MosquittoTransport>();
}

}  // namespace mqtt
