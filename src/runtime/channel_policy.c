#include "runtime/channel_policy.h"

const sc_runtime_channel_provider_route_entry *sc_runtime_channel_provider_route_for(const sc_vec *routes,
                                                                                    sc_str channel_name)
{
    if (routes == nullptr || channel_name.len == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < routes->len; i += 1) {
        const sc_runtime_channel_provider_route_entry *route = sc_vec_at_const(routes, i);
        if (route != nullptr && sc_str_equal(sc_string_as_str(&route->channel_name), channel_name)) {
            return route;
        }
    }
    return nullptr;
}

sc_status sc_runtime_channel_model_switch_for(const sc_vec *routes,
                                              sc_str channel_name,
                                              sc_model_switch_request *out)
{
    const sc_runtime_channel_provider_route_entry *route = nullptr;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.runtime.channel_policy.invalid_argument");
    }
    *out = (sc_model_switch_request){0};
    route = sc_runtime_channel_provider_route_for(routes, channel_name);
    if (route == nullptr || route->model.len == 0) {
        return sc_status_ok();
    }
    *out = (sc_model_switch_request){
        .struct_size = sizeof(*out),
        .provider_name = sc_string_as_str(&route->provider_name),
        .model = sc_string_as_str(&route->model),
        .reason = channel_name,
    };
    return sc_status_ok();
}
