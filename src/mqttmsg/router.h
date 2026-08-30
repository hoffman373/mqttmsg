/**
 * @file router.h
 * @internal
 * @brief Deciding which registered subscriptions a received topic belongs
 *        to.
 *
 * A broker sends a PUBLISH once per matching subscription it holds, but it
 * does not say which one matched, and a client that registered several
 * filters has to work that out for itself. That is a question about MQTT's
 * filter vocabulary — `+` for one level, `#` for the rest — rather than
 * about the platform underneath, so it is answered here rather than in
 * either client.
 *
 * The matching itself is mqttTopicMatch() in mqtt_string.h. What this adds
 * is the walk: one topic against a whole table of filters, visiting every
 * match rather than stopping at the first, because a topic may legitimately
 * belong to more than one subscription.
 *
 * Internal to the library; not shipped in `include/`.
 */

#ifndef MQTTMSG_ROUTER_H
#define MQTTMSG_ROUTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include <mqttmsg/mqtt_string.h>

/**
 * @internal
 * @brief Whether a subscription filter covers a topic.
 * @param topic  Topic the message arrived on.
 * @param filter Filter as registered, which may contain `+` or `#`.
 * @return true if the broker would have sent this topic for this filter.
 */
bool mqttRouteMatches(MqttString topic, const char* filter);

/**
 * @internal
 * @brief Supplies a registered filter.
 * @param ctx   The context pointer mqttRouteDispatch() was called with.
 * @param index Which filter is wanted, counting from zero.
 * @return The filter, NUL-terminated.
 */
typedef const char* (*MqttFilterAt)(void* ctx, int index);

/**
 * @internal
 * @brief Called once for each filter that matched.
 * @param ctx   The context pointer mqttRouteDispatch() was called with.
 * @param index Which filter matched.
 * @param topic The topic that matched it.
 */
typedef void (*MqttRouteVisitor)(void* ctx, int index, MqttString topic);

/**
 * @internal
 * @brief Visits every registered filter that covers @p topic.
 *
 * Every match, not the first: subscribing to both `atx/1/command` and
 * `atx/#` is a legitimate thing to do, and a message under the first is
 * genuinely covered by both.
 *
 * @param topic    Topic the message arrived on.
 * @param filterAt Supplies the filters, called once per index.
 * @param visit    Called once per match, in registration order.
 * @param ctx      Passed back to both @p filterAt and @p visit.
 * @param count    How many filters there are. Zero or fewer visits nothing.
 */
void mqttRouteDispatch(MqttString topic, MqttFilterAt filterAt, MqttRouteVisitor visit, void* ctx,
                       int count);

#ifdef __cplusplus
}
#endif

#endif
