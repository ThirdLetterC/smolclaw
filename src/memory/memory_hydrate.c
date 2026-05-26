#include "memory/memory_internal.h"

sc_status sc_memory_hydrate_entry(sc_memory *memory, const sc_json_value *object, bool allow_secrets)
{
    sc_str id = {0};
    sc_str namespace_name = {0};
    sc_str session_id = {0};
    sc_str category = {0};
    sc_str key = {0};
    sc_str content = {0};
    sc_str metadata = {0};
    sc_str source = {0};
    sc_str content_ref = {0};
    sc_str superseded_by = {0};
    double redaction_state = 0.0;
    sc_json_value *value = nullptr;

    if (sc_json_type_of(object) != SC_JSON_OBJECT) {
        return sc_status_parse("sc.memory.hydrate_expected_object");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("id"));
    (void)sc_json_as_str(value, &id);
    value = sc_json_object_get(object, sc_str_from_cstr("namespace"));
    if (!sc_json_as_str(value, &namespace_name)) {
        return sc_status_parse("sc.memory.hydrate_missing_namespace");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("session_id"));
    (void)sc_json_as_str(value, &session_id);
    value = sc_json_object_get(object, sc_str_from_cstr("category"));
    (void)sc_json_as_str(value, &category);
    value = sc_json_object_get(object, sc_str_from_cstr("key"));
    if (!sc_json_as_str(value, &key)) {
        return sc_status_parse("sc.memory.hydrate_missing_key");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("content"));
    if (!sc_json_as_str(value, &content)) {
        return sc_status_parse("sc.memory.hydrate_missing_content");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("metadata"));
    (void)sc_json_as_str(value, &metadata);
    value = sc_json_object_get(object, sc_str_from_cstr("source"));
    (void)sc_json_as_str(value, &source);
    value = sc_json_object_get(object, sc_str_from_cstr("content_ref"));
    (void)sc_json_as_str(value, &content_ref);
    value = sc_json_object_get(object, sc_str_from_cstr("superseded_by"));
    (void)sc_json_as_str(value, &superseded_by);
    value = sc_json_object_get(object, sc_str_from_cstr("redaction_state"));
    (void)sc_json_as_number(value, &redaction_state);
    return sc_memory_put(memory,
                         &(sc_memory_record){
                             .struct_size = sizeof(sc_memory_record),
                             .id = id,
                             .namespace_name = namespace_name,
                             .session_id = session_id,
                             .category = category,
                             .key = key,
                             .value = content,
                             .superseded_by_id = superseded_by,
                             .source = source,
                             .metadata_json = metadata,
                             .redaction_state = (sc_memory_redaction_state)(int)redaction_state,
                             .content_ref = content_ref,
                             .allow_sensitive_content = allow_secrets,
                         });
}

sc_status sc_memory_hydrate_store_entry(sc_memory_store *store, const sc_json_value *object, bool allow_secrets)
{
    sc_str id = {0};
    sc_str namespace_name = {0};
    sc_str session_id = {0};
    sc_str category = {0};
    sc_str key = {0};
    sc_str content = {0};
    sc_str metadata = {0};
    sc_str source = {0};
    sc_str content_ref = {0};
    sc_str superseded_by = {0};
    double redaction_state = 0.0;
    sc_json_value *value = nullptr;

    if (sc_json_type_of(object) != SC_JSON_OBJECT) {
        return sc_status_parse("sc.memory.hydrate_expected_object");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("id"));
    (void)sc_json_as_str(value, &id);
    value = sc_json_object_get(object, sc_str_from_cstr("namespace"));
    if (!sc_json_as_str(value, &namespace_name)) {
        return sc_status_parse("sc.memory.hydrate_missing_namespace");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("session_id"));
    (void)sc_json_as_str(value, &session_id);
    value = sc_json_object_get(object, sc_str_from_cstr("category"));
    (void)sc_json_as_str(value, &category);
    value = sc_json_object_get(object, sc_str_from_cstr("key"));
    if (!sc_json_as_str(value, &key)) {
        return sc_status_parse("sc.memory.hydrate_missing_key");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("content"));
    if (!sc_json_as_str(value, &content)) {
        return sc_status_parse("sc.memory.hydrate_missing_content");
    }
    value = sc_json_object_get(object, sc_str_from_cstr("metadata"));
    (void)sc_json_as_str(value, &metadata);
    value = sc_json_object_get(object, sc_str_from_cstr("source"));
    (void)sc_json_as_str(value, &source);
    value = sc_json_object_get(object, sc_str_from_cstr("content_ref"));
    (void)sc_json_as_str(value, &content_ref);
    value = sc_json_object_get(object, sc_str_from_cstr("superseded_by"));
    (void)sc_json_as_str(value, &superseded_by);
    value = sc_json_object_get(object, sc_str_from_cstr("redaction_state"));
    (void)sc_json_as_number(value, &redaction_state);
    return sc_memory_store_put(store,
                               &(sc_memory_record){
                                   .struct_size = sizeof(sc_memory_record),
                                   .id = id,
                                   .namespace_name = namespace_name,
                                   .session_id = session_id,
                                   .category = category,
                                   .key = key,
                                   .value = content,
                                   .superseded_by_id = superseded_by,
                                   .source = source,
                                   .metadata_json = metadata,
                                   .redaction_state = (sc_memory_redaction_state)(int)redaction_state,
                                   .content_ref = content_ref,
                                   .allow_sensitive_content = allow_secrets,
                               });
}
