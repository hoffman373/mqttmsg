/**
 * @file mqtt_message.hpp
 * @brief C++ interop for parsed messages: the `mqttParse*()` accessors
 *        wrapped in std::optional, and range-for over a SUBSCRIBE.
 *
 * Optional — the C accessors in mqtt.h are complete on their own. Those
 * take an out-parameter and return a bool, which makes the type check
 * impossible to skip but leaves the value and its validity as two separate
 * things. These bind them together:
 *
 * @code
 * if (auto connect = mqttmsg::connectPayload(m)) {
 *   use(connect->clientId);
 * }
 * @endcode
 *
 * That costs nothing: the optional is elided entirely at `-O2` on both
 * x86-64 and Cortex-M33, giving the same instructions as calling the C
 * function and testing its return.
 *
 * @warning Include this @b outside any `extern "C"` block. It pulls in
 *          `<optional>`, and a standard library header dragged into a C
 *          linkage block will not end well.
 * @note Requires C++17.
 */

#ifndef MQTTMSG_MQTT_MESSAGE_HPP
#define MQTTMSG_MQTT_MESSAGE_HPP

#include <mqttmsg/mqtt.h>
#include <mqttmsg/mqtt_string.hpp>
#include <mqttmsg/mqtt_types.h>

#if !defined(__cplusplus) || __cplusplus < 201703L
#error "mqtt_message.hpp requires C++17 or later; use mqtt.h from C."
#endif

#include <optional>
#include <type_traits>

/* Nothing here owns anything, so none of it needs destroying. If these stop
   holding, the optionals below stop being free. */
static_assert(std::is_trivially_copyable_v<ConnectPayload>);
static_assert(std::is_trivially_destructible_v<std::optional<ConnectPayload>>);

namespace mqttmsg {

/**
 * @brief The CONNECT strings, if @p m is a CONNECT.
 * @param m Parsed message.
 * @return The payload, or `std::nullopt` unless @p m parsed cleanly and
 *         really is a CONNECT.
 * @warning The result borrows the same receive buffer the Message does and
 *          dies with it. Copy anything you need to outlive the callback.
 */
inline std::optional<ConnectPayload> connectPayload(const Message& m) noexcept {
    ConnectPayload out;
    if (!mqttParseConnectPayload(&m, &out)) {
        return std::nullopt;
    }
    return out;
}

/**
 * @brief The subscription list, if @p m is a SUBSCRIBE.
 * @param m Parsed message.
 * @return The payload, or `std::nullopt` unless @p m parsed cleanly and
 *         really is a SUBSCRIBE.
 * @warning Borrows the receive buffer; see connectPayload().
 * @see each()
 */
inline std::optional<SubscriptionPayload> subscriptions(const Message& m) noexcept {
    SubscriptionPayload out;
    if (!mqttParseSubscriptions(&m, &out)) {
        return std::nullopt;
    }
    return out;
}

/**
 * @brief The topic list, if @p m is an UNSUBSCRIBE.
 * @param m Parsed message.
 * @return The payload, or `std::nullopt` unless @p m parsed cleanly and
 *         really is an UNSUBSCRIBE.
 * @warning Borrows the receive buffer; see connectPayload().
 */
inline std::optional<UnsubscribePayload> unsubTopics(const Message& m) noexcept {
    UnsubscribePayload out;
    if (!mqttParseUnsubTopics(&m, &out)) {
        return std::nullopt;
    }
    return out;
}

/**
 * @brief The granted QoS bytes, if @p m is a SUBACK.
 * @param m Parsed message.
 * @return The payload, or `std::nullopt` unless @p m parsed cleanly and
 *         really is a SUBACK.
 * @warning Borrows the receive buffer; see connectPayload().
 * @see mqttGrantedQosAt()
 */
inline std::optional<SubscriptionResponsePayload> grantedQos(const Message& m) noexcept {
    SubscriptionResponsePayload out;
    if (!mqttParseGrantedQos(&m, &out)) {
        return std::nullopt;
    }
    return out;
}

/** @brief End marker for SubscriptionIterator. */
class SubscriptionSentinel {};

/**
 * @brief Input iterator over the entries of a parsed SUBSCRIBE.
 *
 * A thin wrapper over MqttSubscriptionCursor, the same shape as
 * mqttmsg::SegmentIterator in mqtt_string.hpp.
 *
 * @see each()
 */
class SubscriptionIterator {
   public:
    using value_type = MqttSubscription;               /**< What dereferencing yields. */
    using difference_type = std::ptrdiff_t;            /**< Required by the concept. */
    using reference = MqttSubscription;                /**< By value; the pair is small. */
    using pointer = void;                              /**< No `operator->`. */
    using iterator_category = std::input_iterator_tag; /**< Single-pass. */

    /** @brief Constructs an end iterator. */
    SubscriptionIterator() noexcept : cursor_{nullptr, nullptr}, atEnd_(true) {}

    /**
     * @brief Constructs an iterator over a subscription cursor.
     * @param cursor Cursor to walk. Positioned on the first entry.
     */
    explicit SubscriptionIterator(MqttSubscriptionCursor cursor) noexcept
        : cursor_(cursor), atEnd_(false) {
        advance();
    }

    /** @brief The current entry. @return The topic/QoS pair, by value. */
    reference operator*() const noexcept { return current_; }

    /** @brief Advances to the next entry. @return `*this`. */
    SubscriptionIterator& operator++() noexcept {
        advance();
        return *this;
    }

    /** @brief Advances to the next entry, discarding the old value. */
    void operator++(int) noexcept { advance(); }

    /**
     * @brief Whether the iterator has run out of entries.
     * @param it Iterator to test.
     * @return true once the entries are exhausted.
     */
    friend bool operator==(const SubscriptionIterator& it, SubscriptionSentinel) noexcept {
        return it.atEnd_;
    }

#if __cplusplus < 202002L
    /**
     * @brief Whether entries remain.
     * @param it Iterator to test.
     * @return true while entries remain.
     * @note Pre-C++20 only.
     */
    friend bool operator!=(const SubscriptionIterator& it, SubscriptionSentinel) noexcept {
        return !it.atEnd_;
    }
#endif

   private:
    void advance() noexcept {
        if (!mqttNextSubscription(&cursor_, &current_)) {
            atEnd_ = true;
        }
    }

    MqttSubscriptionCursor cursor_;
    MqttSubscription current_{};
    bool atEnd_;
};

/** @brief The range each() returns. */
class SubscriptionRange {
   public:
    /**
     * @brief Constructs a range over a SUBSCRIBE's entries.
     * @param payload Subscription list to walk.
     */
    explicit SubscriptionRange(SubscriptionPayload payload) noexcept : cursor_(payload.cursor) {}

    /** @brief @return An iterator on the first entry. */
    SubscriptionIterator begin() const noexcept { return SubscriptionIterator(cursor_); }
    /** @brief @return The end sentinel. */
    SubscriptionSentinel end() const noexcept { return SubscriptionSentinel{}; }

   private:
    MqttSubscriptionCursor cursor_;
};

/**
 * @brief Range-for over the entries of a parsed SUBSCRIBE.
 *
 * @code
 * for (MqttSubscription s : mqttmsg::each(subs)) { ... }
 * @endcode
 *
 * @param payload Subscription list from subscriptions().
 * @return A range over the entries. Nothing is allocated or copied.
 */
inline SubscriptionRange each(SubscriptionPayload payload) noexcept {
    return SubscriptionRange(payload);
}

}  // namespace mqttmsg

#endif
