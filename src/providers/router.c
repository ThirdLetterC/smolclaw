#include "sc/provider.h"

#include <string.h>

typedef struct router_provider {
    sc_allocator *alloc;
    sc_provider *default_provider;
    sc_provider *tool_provider;
    sc_vec routes;
    sc_observer *observer;
} router_provider;

typedef struct router_route {
    sc_string hint;
    sc_provider *provider;
} router_route;

static sc_status router_generate(void *impl,
                                 const sc_provider_request *request,
                                 sc_allocator *alloc,
                                 sc_provider_response *out);
static sc_status router_stream(void *impl,
                               const sc_provider_request *request,
                               sc_allocator *alloc,
                               sc_provider_stream_callback callback,
                               void *callback_user_data);
static void router_destroy(void *impl);
static sc_provider *select_provider(router_provider *provider, const sc_provider_request *request, const char **route_name);
static sc_provider *select_hint_route(router_provider *provider, sc_str route_hint, const char **route_name);
static bool route_hint_matches(sc_str configured, sc_str requested);
static bool prompt_mentions_tools(sc_str prompt);
static void emit_route(router_provider *provider, const char *route_name);

static const sc_provider_vtab router_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "router",
    .display_name = "Router provider",
    .feature_flag = "SC_PROVIDER_ROUTER",
    .capabilities = SC_CONTRACT_CAP_STREAMING,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = router_generate,
    .stream = router_stream,
    .destroy = router_destroy,
    .description_key = "sc.provider.router.description",
    .config_schema_ref = "sc.schema.provider.router.v1",
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy)},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM,
};

sc_status sc_provider_router_new(sc_allocator *alloc,
                                 sc_provider *default_provider,
                                 sc_provider *tool_provider,
                                 sc_observer *observer,
                                 sc_provider **out)
{
    sc_provider_route route = {
        .struct_size = sizeof(route),
        .hint = sc_str_from_cstr("tools"),
        .provider = tool_provider,
    };
    return sc_provider_router_routes_new(alloc,
                                         default_provider,
                                         tool_provider == nullptr ? nullptr : &route,
                                         tool_provider == nullptr ? 0 : 1,
                                         observer,
                                         out);
}

sc_status sc_provider_router_routes_new(sc_allocator *alloc,
                                        sc_provider *default_provider,
                                        const sc_provider_route *routes,
                                        size_t route_count,
                                        sc_observer *observer,
                                        sc_provider **out)
{
    router_provider *impl = nullptr;
    sc_status status;

    if (out == nullptr || default_provider == nullptr) {
        return sc_status_invalid_argument("sc.provider_router.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(router_provider));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (router_provider){
        .alloc = alloc,
        .default_provider = default_provider,
        .tool_provider = default_provider,
        .observer = observer,
    };
    sc_vec_init(&impl->routes, alloc, sizeof(router_route));
    status = sc_status_ok();
    for (size_t i = 0; sc_status_is_ok(status) && i < route_count; i += 1) {
        router_route route = {0};
        if (routes[i].provider == nullptr || routes[i].hint.len == 0) {
            status = sc_status_invalid_argument("sc.provider_router.invalid_route");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc, routes[i].hint, &route.hint);
        }
        if (sc_status_is_ok(status)) {
            route.provider = routes[i].provider;
            status = sc_vec_push(&impl->routes, &route);
        }
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&route.hint);
        }
    }
    if (sc_status_is_ok(status)) {
        for (size_t i = 0; i < impl->routes.len; i += 1) {
            router_route *route = sc_vec_at(&impl->routes, i);
            if (route != nullptr && sc_str_equal(sc_string_as_str(&route->hint), sc_str_from_cstr("tools"))) {
                impl->tool_provider = route->provider;
                break;
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &router_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        router_destroy(impl);
    }
    return status;
}

static sc_status router_generate(void *impl,
                                 const sc_provider_request *request,
                                 sc_allocator *alloc,
                                 sc_provider_response *out)
{
    router_provider *provider = impl;
    const char *route_name = "default";
    sc_provider *selected = nullptr;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_router.invalid_argument");
    }
    selected = select_provider(provider, request, &route_name);
    emit_route(provider, route_name);
    return sc_provider_generate(selected, request, alloc, out);
}

static sc_status router_stream(void *impl,
                               const sc_provider_request *request,
                               sc_allocator *alloc,
                               sc_provider_stream_callback callback,
                               void *callback_user_data)
{
    router_provider *provider = impl;
    const char *route_name = "default";
    sc_provider *selected = nullptr;

    if (provider == nullptr || request == nullptr || callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_router.stream_invalid_argument");
    }
    selected = select_provider(provider, request, &route_name);
    emit_route(provider, route_name);
    return sc_provider_stream(selected, request, alloc, callback, callback_user_data);
}

static void router_destroy(void *impl)
{
    router_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    for (size_t i = 0; i < provider->routes.len; i += 1) {
        router_route *route = sc_vec_at(&provider->routes, i);
        if (route != nullptr) {
            sc_string_clear(&route->hint);
        }
    }
    sc_vec_clear(&provider->routes);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(router_provider));
}

static sc_provider *select_provider(router_provider *provider, const sc_provider_request *request, const char **route_name)
{
    sc_provider *hint_provider = select_hint_route(provider, request->route_hint, route_name);
    if (hint_provider != nullptr) {
        return hint_provider;
    }
    if (provider->tool_provider != nullptr && prompt_mentions_tools(request->prompt)) {
        *route_name = "tools";
        return provider->tool_provider;
    }
    *route_name = "default";
    return provider->default_provider;
}

static sc_provider *select_hint_route(router_provider *provider, sc_str route_hint, const char **route_name)
{
    if (route_hint.len == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < provider->routes.len; i += 1) {
        router_route *route = sc_vec_at(&provider->routes, i);
        if (route != nullptr && route_hint_matches(sc_string_as_str(&route->hint), route_hint)) {
            *route_name = route->hint.ptr;
            return route->provider;
        }
    }
    return nullptr;
}

static bool route_hint_matches(sc_str configured, sc_str requested)
{
    if (configured.len == 0 || requested.len == 0) {
        return false;
    }
    if (sc_str_equal(configured, requested)) {
        return true;
    }
    for (size_t i = 0; i + configured.len <= requested.len; i += 1) {
        bool left_boundary = i == 0 || requested.ptr[i - 1] == ',' || requested.ptr[i - 1] == ' ' ||
                             requested.ptr[i - 1] == ':' || requested.ptr[i - 1] == ';';
        bool right_boundary = i + configured.len == requested.len ||
                              requested.ptr[i + configured.len] == ',' ||
                              requested.ptr[i + configured.len] == ' ' ||
                              requested.ptr[i + configured.len] == ';';
        if (left_boundary && right_boundary && memcmp(requested.ptr + i, configured.ptr, configured.len) == 0) {
            return true;
        }
    }
    return false;
}

static bool prompt_mentions_tools(sc_str prompt)
{
    static const char needle[] = "tool";

    if (prompt.ptr == nullptr || prompt.len < sizeof(needle) - 1) {
        return false;
    }
    for (size_t i = 0; i <= prompt.len - (sizeof(needle) - 1); i += 1) {
        if (memcmp(prompt.ptr + i, needle, sizeof(needle) - 1) == 0) {
            return true;
        }
    }
    return false;
}

static void emit_route(router_provider *provider, const char *route_name)
{
    sc_log_field fields[] = {
        {.key = "route", .value = sc_str_from_cstr(route_name), .secret = false},
    };
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("provider.router"),
        .name = sc_str_from_cstr("provider.route_selected"),
        .fields = fields,
        .field_count = SC_ARRAY_LEN(fields),
    };
    if (provider->observer != nullptr) {
        (void)sc_observer_emit_safe(provider->observer, &event, nullptr);
    }
}
