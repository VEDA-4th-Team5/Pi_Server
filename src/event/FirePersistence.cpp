#include "event/FirePersistence.hpp"

namespace event {

const char* toString(const FireAlarmLifecycle lifecycle) noexcept {
    switch (lifecycle) {
    case FireAlarmLifecycle::Open:
        return "OPEN";
    case FireAlarmLifecycle::Acknowledged:
        return "ACKNOWLEDGED";
    case FireAlarmLifecycle::Resolved:
        return "RESOLVED";
    }
    return "RESOLVED";
}

const char* toString(const FireProtocolMode mode) noexcept {
    switch (mode) {
    case FireProtocolMode::Unseen:
        return "UNSEEN";
    case FireProtocolMode::Legacy:
        return "LEGACY";
    case FireProtocolMode::Versioned:
        return "VERSIONED";
    }
    return "UNSEEN";
}

const char* toString(const FireDeliverySinkKind kind) noexcept {
    switch (kind) {
    case FireDeliverySinkKind::RetainedState:
        return "RETAINED_STATE";
    case FireDeliverySinkKind::LifecycleEvent:
        return "LIFECYCLE_EVENT";
    }
    return "LIFECYCLE_EVENT";
}

const char* toString(const FireDeliveryState state) noexcept {
    switch (state) {
    case FireDeliveryState::Pending:
        return "PENDING";
    case FireDeliveryState::InFlight:
        return "IN_FLIGHT";
    case FireDeliveryState::Acknowledged:
        return "ACKED";
    }
    return "PENDING";
}

std::optional<FireAlarmLifecycle> fireAlarmLifecycleFromString(
    const std::string& value) noexcept {
    if (value == "OPEN") return FireAlarmLifecycle::Open;
    if (value == "ACKNOWLEDGED") return FireAlarmLifecycle::Acknowledged;
    if (value == "RESOLVED") return FireAlarmLifecycle::Resolved;
    return std::nullopt;
}

std::optional<FireProtocolMode> fireProtocolModeFromString(
    const std::string& value) noexcept {
    if (value == "UNSEEN") return FireProtocolMode::Unseen;
    if (value == "LEGACY") return FireProtocolMode::Legacy;
    if (value == "VERSIONED") return FireProtocolMode::Versioned;
    return std::nullopt;
}

std::optional<FireDeliverySinkKind> fireDeliverySinkFromString(
    const std::string& value) noexcept {
    if (value == "RETAINED_STATE") return FireDeliverySinkKind::RetainedState;
    if (value == "LIFECYCLE_EVENT") return FireDeliverySinkKind::LifecycleEvent;
    return std::nullopt;
}

std::optional<FireDeliveryState> fireDeliveryStateFromString(
    const std::string& value) noexcept {
    if (value == "PENDING") return FireDeliveryState::Pending;
    if (value == "IN_FLIGHT") return FireDeliveryState::InFlight;
    if (value == "ACKED") return FireDeliveryState::Acknowledged;
    return std::nullopt;
}

}  // namespace event
