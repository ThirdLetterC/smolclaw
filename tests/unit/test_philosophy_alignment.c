#include "sc/security.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static int expect_prop(const sc_config *config, const char *path, const char *expected)
{
    sc_string value = {0};
    sc_status status = sc_config_get_prop(config, sc_str_from_cstr(path), sc_allocator_heap(), &value);
    int failures = sc_test_expect_status(path, status, SC_OK);

    if (failures == 0 && strcmp(value.ptr == nullptr ? "" : value.ptr, expected) != 0) {
        (void)fprintf(stderr,
                      "%s: expected '%s', got '%s'\n",
                      path,
                      expected,
                      value.ptr == nullptr ? "" : value.ptr);
        failures += 1;
    }
    sc_string_clear(&value);
    return failures;
}

static int test_default_posture(void)
{
    int failures = 0;
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_security_policy policy = {0};
    sc_estop_state estop = {0};
    bool approval_required = false;
    sc_security_tool_request shell_request = {
        .struct_size = sizeof(shell_request),
        .tool_name = sc_str_from_cstr("shell"),
        .risk = SC_TOOL_RISK_SHELL,
        .shell_arg = sc_str_from_cstr("echo default"),
    };

    failures += sc_test_expect_status("default config", sc_config_load(sc_allocator_heap(), nullptr, &config, &diag), SC_OK);
    failures += sc_test_expect_status("default policy", sc_security_policy_from_config(&policy, &config), SC_OK);

    failures += sc_test_expect_true("supervised autonomy", policy.autonomy == SC_AUTONOMY_SUPERVISED);
    failures += sc_test_expect_true("workspace only", policy.workspace_only);
    failures += sc_test_expect_true("shell disabled", !policy.shell_enabled);
    failures += sc_test_expect_true("receipts enabled", policy.receipts_enabled);
    failures += sc_test_expect_true("public bind disabled",
                            !sc_config_get_bool(&config, sc_str_from_cstr("gateway.public_bind_enabled"), true));
    failures += sc_test_expect_true("gateway disabled",
                            !sc_config_get_bool(&config, sc_str_from_cstr("gateway.enabled"), true));
    failures += sc_test_expect_true("webhook disabled",
                            !sc_config_get_bool(&config, sc_str_from_cstr("channels.webhook.enabled"), true));
    failures += sc_test_expect_true("rabbitmq disabled",
                            !sc_config_get_bool(&config, sc_str_from_cstr("channels.rabbitmq.enabled"), true));
    failures += sc_test_expect_true("mail disabled",
                            !sc_config_get_bool(&config, sc_str_from_cstr("channels.mail.enabled"), true));
    failures += expect_prop(&config, "tools.web_search.provider", "none");
    failures += expect_prop(&config, "memory.backend", "none");
    failures += sc_test_expect_status("default shell denied",
                              sc_security_validate_request(&policy, &estop, &shell_request, &approval_required),
                              SC_ERR_SECURITY_DENIED);

    sc_security_policy_clear(&policy);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    return failures;
}

static int test_yolo_preset_policy(void)
{
    static const char body[] =
        "[runtime]\n"
        "autonomy_level = \"full\"\n"
        "\n"
        "[autonomy]\n"
        "level = \"full\"\n"
        "shell_enabled = true\n"
        "workspace_only = false\n"
        "auto_approve = [\"shell\"]\n"
        "forbidden_commands = []\n"
        "\n"
        "[security.otp]\n"
        "enabled = false\n"
        "\n"
        "[security.sandbox]\n"
        "backend = \"noop\"\n"
        "network = \"full\"\n"
        "\n"
        "[agent.tool_receipts]\n"
        "enabled = true\n"
        "show_in_response = true\n"
        "inject_system_prompt = true\n";
    int failures = 0;
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options options = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("yolo.toml"),
            .body = sc_str_from_cstr(body),
            .present = true,
        },
    };
    sc_security_policy policy = {0};
    sc_estop_state estop = {0};
    bool approval_required = true;
    sc_security_tool_request shell_request = {
        .struct_size = sizeof(shell_request),
        .tool_name = sc_str_from_cstr("shell"),
        .risk = SC_TOOL_RISK_SHELL,
        .shell_arg = sc_str_from_cstr("echo yolo"),
    };

    failures += sc_test_expect_status("yolo config", sc_config_load(sc_allocator_heap(), &options, &config, &diag), SC_OK);
    failures += sc_test_expect_status("yolo policy", sc_security_policy_from_config(&policy, &config), SC_OK);
    failures += sc_test_expect_true("full autonomy", policy.autonomy == SC_AUTONOMY_FULL);
    failures += sc_test_expect_true("yolo shell enabled", policy.shell_enabled);
    failures += sc_test_expect_true("yolo workspace unrestricted", !policy.workspace_only);
    failures += sc_test_expect_true("noop sandbox", policy.sandbox_backend == SC_SANDBOX_BACKEND_NOOP);
    failures += sc_test_expect_true("visible receipts", policy.receipts_show_in_response);
    failures += sc_test_expect_status("yolo shell allowed",
                              sc_security_validate_request(&policy, &estop, &shell_request, &approval_required),
                              SC_OK);
    failures += sc_test_expect_true("yolo shell auto approved", !approval_required);

    sc_security_policy_clear(&policy);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    return failures;
}

static int test_provider_override_is_neutral(void)
{
    static const char body[] =
        "[providers]\n"
        "fallback = \"mock\"\n"
        "\n"
        "[providers.models.mock]\n"
        "kind = \"mock\"\n"
        "model = \"neutral-model\"\n"
        "\n"
        "[channels.cli]\n"
        "provider = \"mock\"\n"
        "tools_allow = [\"time\"]\n";
    int failures = 0;
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options options = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("neutral-provider.toml"),
            .body = sc_str_from_cstr(body),
            .present = true,
        },
    };

    failures += sc_test_expect_status("neutral provider config", sc_config_load(sc_allocator_heap(), &options, &config, &diag), SC_OK);
    failures += expect_prop(&config, "providers.fallback", "mock");
    failures += expect_prop(&config, "channels.cli.provider", "mock");
    failures += expect_prop(&config, "channels.cli.tools_allow", "[\"time\"]");

    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    return failures;
}

int main(void)
{
    int failures = 0;

    failures += test_default_posture();
    failures += test_yolo_preset_policy();
    failures += test_provider_override_is_neutral();

    return failures == 0 ? 0 : 1;
}
