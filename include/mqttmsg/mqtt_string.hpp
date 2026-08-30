/**
 * @file mqtt_string.hpp
 * @brief C++ interop for MqttString: std::string_view conversion,
 *        comparison operators and range-for over topic segments.
 *
 * Optional — the C API in mqtt_string.h is complete on its own. Everything
 * here is a free function, so MqttString stays a plain aggregate that can
 * cross the language boundary by value.
 *
 * @warning Include this @b outside any `extern "C"` block. It pulls in
 *          `<string_view>`, and a standard library header dragged into a C
 *          linkage block will not end well. mqtt_string.h itself is safe
 *          anywhere.
 * @code
 * #include <mqttmsg/mqttmsg.h>        // inside extern "C" is fine
 * #include <mqttmsg/mqtt_string.hpp>  // must be outside
 * @endcode
 *
 * @note Requires C++17. `bytes()` additionally requires C++20 `<span>`.
 */

#ifndef MQTTMSG_MQTT_STRING_HPP
#define MQTTMSG_MQTT_STRING_HPP

#include <mqttmsg/mqtt_string.h>
#include <mqttmsg/mqtt_types.h>

#if !defined(__cplusplus) || __cplusplus < 201703L
#error "mqtt_string.hpp requires C++17 or later; use mqtt_string.h from C."
#endif

#include <cstddef>
#include <iterator>
#include <optional>
#include <string_view>
#include <type_traits>

#if __cplusplus >= 202002L && __has_include(<span>)
#include <span>
/** @brief Defined when `<span>` is available, which is what gates bytes(). */
#define MQTTMSG_HAS_SPAN 1
#endif

/* The ABI guarantees the C side relies on. Deliberately not a sizeof check:
   the struct is 8 bytes on a 32-bit target and 16 on a 64-bit host, and both
   are fine — what matters is that it stays a POD that can cross the
   language boundary by value. */
static_assert(std::is_trivially_copyable_v<MqttString>, "MqttString must stay trivially copyable");
static_assert(std::is_standard_layout_v<MqttString>, "MqttString must stay standard layout");

namespace mqttmsg {

/**
 * @brief Converts an MqttString to a std::string_view.
 * @param s View to convert.
 * @return A string_view over the same bytes, empty if `s.data` is null.
 * @warning Zero-copy: the result borrows the same bytes, with the same
 *          lifetime caveat as the MqttString it came from.
 */
constexpr std::string_view view(MqttString s) noexcept {
    return s.data != nullptr ? std::string_view(s.data, s.length) : std::string_view();
}

/**
 * @brief Converts a std::string_view to an MqttString.
 * @param v Source view.
 * @return An MqttString over the same bytes.
 * @warning Does not copy, and does not check the length against
 *          `UINT16_MAX` — a view longer than that truncates on the cast.
 */
constexpr MqttString fromView(std::string_view v) noexcept {
    return MqttString{v.data(), static_cast<uint16_t>(v.size())};
}

/**
 * @brief Prefix match against a std::string_view.
 * @param s      View to test.
 * @param prefix Prefix to look for.
 * @return true if @p s starts with @p prefix.
 */
constexpr bool startsWith(MqttString s, std::string_view prefix) noexcept {
    return view(s).substr(0, prefix.size()) == prefix;
}

/**
 * @brief Strips a topic prefix, separator included.
 *
 * The C++ spelling of mqttStrStripTopicPrefix(): what is left comes back in
 * the optional rather than through an out parameter, so the test and the
 * result are one expression.
 *
 * @code
 * if (auto rest = mqttmsg::stripTopicPrefix(topic, config.topicPrefix)) {
 *     for (MqttString segment : mqttmsg::segments(*rest)) { ... }
 * }
 * @endcode
 *
 * @param s      Topic to strip.
 * @param prefix Prefix to remove, without a trailing `/`. An empty prefix
 *               strips nothing and always matches.
 * @return What follows the prefix, empty if @p s is exactly the prefix, or
 *         `std::nullopt` if @p s is not under it. Matches whole segments,
 *         so `"atx"` does not strip `"atxfoo/1"`.
 * @warning Borrows, as every MqttString does: the result points into @p s.
 */
constexpr std::optional<MqttString> stripTopicPrefix(MqttString s,
                                                     std::string_view prefix) noexcept {
    if (prefix.empty()) {
        return s;
    }

    std::string_view topic = view(s);
    if (topic.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }

    /* The separator is consumed along with the prefix, except where the
       topic ended at the prefix and there is none. */
    if (topic.size() == prefix.size()) {
        return fromView(topic.substr(prefix.size()));
    }

    if (topic[prefix.size()] != '/') {
        return std::nullopt;
    }

    return fromView(topic.substr(prefix.size() + 1));
}

#ifdef MQTTMSG_HAS_SPAN
/**
 * @brief Converts a MqttPayload to a span of bytes.
 * @param p MqttPayload to convert.
 * @return A span over the same bytes; empty unless @p p carries any. A
 *         MqttPayload that has no body, or that failed to build, yields an
 *         empty span rather than the failure tag read as a size.
 * @note Requires C++20; gated on #MQTTMSG_HAS_SPAN.
 */
constexpr std::span<const uint8_t> bytes(MqttPayload p) noexcept {
    return mqttPayloadIsOk(p) ? std::span<const uint8_t>(mqttPayloadBytes(p), mqttPayloadLength(p))
                              : std::span<const uint8_t>();
}
#endif

/** @brief End marker for SegmentIterator. */
class SegmentSentinel {};

/**
 * @brief Input iterator over the `/`-separated segments of a topic.
 *
 * A thin wrapper over MqttStrCursor; nothing is allocated or copied.
 *
 * @see segments()
 */
class SegmentIterator {
   public:
    using value_type = MqttString;                     /**< What dereferencing yields. */
    using difference_type = std::ptrdiff_t;            /**< Required by the concept. */
    using reference = MqttString;                      /**< By value; a view is small. */
    using pointer = void;                              /**< No `operator->`. */
    using iterator_category = std::input_iterator_tag; /**< Single-pass. */

    /** @brief Constructs an end iterator. */
    SegmentIterator() noexcept
        : cursor_{nullptr, nullptr, true}, current_(mqttStrEmpty()), atEnd_(true) {}

    /**
     * @brief Constructs an iterator over the segments of @p s.
     * @param s Topic to walk. Positioned on the first segment.
     */
    explicit SegmentIterator(MqttString s) noexcept
        : cursor_(mqttStrSegments(s)), current_(mqttStrEmpty()), atEnd_(false) {
        advance();
    }

    /** @brief The current segment. @return The segment, by value. */
    reference operator*() const noexcept { return current_; }

    /** @brief Advances to the next segment. @return `*this`. */
    SegmentIterator& operator++() noexcept {
        advance();
        return *this;
    }

    /** @brief Advances to the next segment, discarding the old value. */
    void operator++(int) noexcept { advance(); }

    /**
     * @brief Whether the iterator has run out of segments.
     * @param it Iterator to test.
     * @return true once the segments are exhausted.
     */
    friend bool operator==(const SegmentIterator& it, SegmentSentinel) noexcept {
        return it.atEnd_;
    }

#if __cplusplus < 202002L
    /**
     * @brief Whether segments remain.
     * @param it Iterator to test.
     * @return true while segments remain.
     * @note Pre-C++20 only.
     */
    friend bool operator!=(const SegmentIterator& it, SegmentSentinel) noexcept {
        return !it.atEnd_;
    }
    /**
     * @brief Exhaustion test with the sentinel on the left.
     * @param s  The sentinel.
     * @param it Iterator to test.
     * @return true once the segments are exhausted.
     * @note Pre-C++20 only.
     */
    friend bool operator==(SegmentSentinel s, const SegmentIterator& it) noexcept {
        return it == s;
    }
    /**
     * @brief Remaining test with the sentinel on the left.
     * @param s  The sentinel.
     * @param it Iterator to test.
     * @return true while segments remain.
     * @note Pre-C++20 only.
     */
    friend bool operator!=(SegmentSentinel s, const SegmentIterator& it) noexcept {
        return !(it == s);
    }
#endif

   private:
    void advance() noexcept {
        if (!mqttStrNextSegment(&cursor_, &current_)) {
            current_ = mqttStrEmpty();
            atEnd_ = true;
        }
    }

    MqttStrCursor cursor_;
    MqttString current_;
    bool atEnd_;
};

/** @brief The range segments() returns. */
class SegmentRange {
   public:
    /**
     * @brief Constructs a range over the segments of @p s.
     * @param s Topic to walk.
     */
    constexpr explicit SegmentRange(MqttString s) noexcept : s_(s) {}

    /** @brief @return An iterator on the first segment. */
    SegmentIterator begin() const noexcept { return SegmentIterator(s_); }
    /** @brief @return The end sentinel. */
    SegmentSentinel end() const noexcept { return SegmentSentinel{}; }

   private:
    MqttString s_;
};

/**
 * @brief Range-for over the `/`-separated segments of a topic.
 *
 * @code
 * for (MqttString segment : mqttmsg::segments(topic)) { ... }
 * @endcode
 *
 * @param s Topic to walk.
 * @return A range over the segments. Nothing is allocated or copied.
 */
constexpr SegmentRange segments(MqttString s) noexcept { return SegmentRange(s); }

}  // namespace mqttmsg

/**
 * @brief Compares two MqttStrings for equality.
 *
 * At global scope rather than in `namespace mqttmsg`, because MqttString is
 * a C type in the global namespace and that is where argument-dependent
 * lookup goes looking. Under C++20 only the forward form is declared and
 * the reversed one is synthesised; declaring both would make every
 * comparison ambiguous.
 *
 * @param a First view.
 * @param b Second view.
 * @return true if they hold the same bytes.
 */
constexpr bool operator==(MqttString a, MqttString b) noexcept {
    return mqttmsg::view(a) == mqttmsg::view(b);
}

/**
 * @brief Compares an MqttString against a std::string_view.
 * @param a View to compare.
 * @param b string_view to compare against.
 * @return true if they hold the same bytes.
 */
constexpr bool operator==(MqttString a, std::string_view b) noexcept {
    return mqttmsg::view(a) == b;
}

#if __cplusplus < 202002L
/**
 * @brief Inequality of two MqttStrings.
 * @param a First view.
 * @param b Second view.
 * @return true if they differ.
 * @note Pre-C++20 only; from C++20 the compiler synthesises it.
 */
constexpr bool operator!=(MqttString a, MqttString b) noexcept { return !(a == b); }
/**
 * @brief Equality with the string_view on the left.
 * @param a string_view to compare.
 * @param b View to compare against.
 * @return true if they hold the same bytes.
 * @note Pre-C++20 only; from C++20 the compiler synthesises it.
 */
constexpr bool operator==(std::string_view a, MqttString b) noexcept {
    return a == mqttmsg::view(b);
}
/**
 * @brief Inequality against a std::string_view.
 * @param a View to compare.
 * @param b string_view to compare against.
 * @return true if they differ.
 * @note Pre-C++20 only; from C++20 the compiler synthesises it.
 */
constexpr bool operator!=(MqttString a, std::string_view b) noexcept { return !(a == b); }
/**
 * @brief Inequality with the string_view on the left.
 * @param a string_view to compare.
 * @param b View to compare against.
 * @return true if they differ.
 * @note Pre-C++20 only; from C++20 the compiler synthesises it.
 */
constexpr bool operator!=(std::string_view a, MqttString b) noexcept { return !(a == b); }
#endif

#endif
