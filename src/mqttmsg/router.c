/**
 * @file router.c
 * @brief Topic-to-subscription matching.
 *
 * @see router.h
 */

#include "router.h"

bool mqttRouteMatches(MqttString topic, const char* filter) {
    /* No captures wanted here: the question is only whether the filter
       covers the topic. Callers that want the wildcards' contents call
       mqttTopicMatch() themselves. */
    return mqttTopicMatch(topic, filter, NULL, 0, NULL);
}

void mqttRouteDispatch(MqttString topic, MqttFilterAt filterAt, MqttRouteVisitor visit, void* ctx,
                       int count) {
    for (int i = 0; i < count; i++) {
        if (mqttRouteMatches(topic, filterAt(ctx, i))) {
            visit(ctx, i, topic);
        }
    }
}
