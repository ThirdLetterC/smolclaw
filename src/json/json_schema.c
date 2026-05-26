#include "json/json_internal.h"

sc_status sc_json_schema_object(sc_allocator *alloc, sc_json_value **out)
{
    sc_json_value *schema = nullptr;
    sc_json_value *type = nullptr;
    sc_json_value *properties = nullptr;
    sc_json_value *required = nullptr;
    sc_status status = sc_json_object_new(alloc, &schema);

    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(schema->alloc, sc_str_from_cstr("object"), &type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(schema, sc_str_from_cstr("type"), type);
        type = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(schema->alloc, &properties);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(schema, sc_str_from_cstr("properties"), properties);
        properties = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(schema->alloc, &required);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(schema, sc_str_from_cstr("required"), required);
        required = nullptr;
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(schema);
        sc_json_destroy(type);
        sc_json_destroy(properties);
        sc_json_destroy(required);
        return status;
    }

    *out = schema;
    return sc_status_ok();
}

sc_status sc_json_schema_add_string_property(sc_json_value *schema, sc_str name, bool required)
{
    sc_json_value *properties = sc_json_object_get(schema, sc_str_from_cstr("properties"));
    sc_json_value *required_array = sc_json_object_get(schema, sc_str_from_cstr("required"));
    sc_json_value *property = nullptr;
    sc_json_value *type = nullptr;
    sc_status status = sc_json_object_new(schema->alloc, &property);

    if (properties == nullptr || required_array == nullptr) {
        sc_json_destroy(property);
        return sc_status_invalid_argument("sc.json.schema.invalid_argument");
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(schema->alloc, sc_str_from_cstr("string"), &type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(property, sc_str_from_cstr("type"), type);
        type = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(properties, name, property);
        property = nullptr;
    }
    if (sc_status_is_ok(status) && required) {
        sc_json_value *name_value = nullptr;
        status = sc_json_string_new(schema->alloc, name, &name_value);
        if (sc_status_is_ok(status)) {
            status = sc_json_array_append(required_array, name_value);
            if (!sc_status_is_ok(status)) {
                sc_json_destroy(name_value);
            }
        }
    }
    sc_json_destroy(property);
    sc_json_destroy(type);
    return status;
}

sc_status sc_json_provider_payload(sc_allocator *alloc,
                                   sc_str model,
                                   sc_str prompt,
                                   sc_json_value **out)
{
    sc_json_value *payload = nullptr;
    sc_json_value *model_value = nullptr;
    sc_json_value *prompt_value = nullptr;
    sc_status status = sc_json_object_new(alloc, &payload);

    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(payload->alloc, model, &model_value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(payload, sc_str_from_cstr("model"), model_value);
        model_value = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(payload->alloc, prompt, &prompt_value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(payload, sc_str_from_cstr("prompt"), prompt_value);
        prompt_value = nullptr;
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(payload);
        sc_json_destroy(model_value);
        sc_json_destroy(prompt_value);
        return status;
    }

    *out = payload;
    return sc_status_ok();
}
