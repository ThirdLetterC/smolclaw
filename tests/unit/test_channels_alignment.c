#include "sc/acp.h"
#include "sc/channel.h"
#include "sc/gateway.h"
#include "sc/provider.h"
#include "sc/runtime.h"
#include "sc/tool.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define INBOUND(id_value, channel_value, conversation_value, sender_value, text_value) \
    (&(sc_channel_inbound){                                                           \
        .struct_size = sizeof(sc_channel_inbound),                                    \
        .message_id = sc_str_from_cstr(id_value),                                     \
        .channel_name = sc_str_from_cstr(channel_value),                              \
        .conversation_id = sc_str_from_cstr(conversation_value),                      \
        .sender_id = sc_str_from_cstr(sender_value),                                  \
        .text = sc_str_from_cstr(text_value),                                         \
    })

typedef struct model_echo_provider {
    int calls;
    sc_string last_prompt;
    sc_string last_tool_specs_json;
} model_echo_provider;

typedef struct counting_tool {
    int calls;
} counting_tool;

typedef struct mail_stub {
    sc_string operation;
    sc_string url;
    sc_string username;
    sc_string password;
    sc_string payload;
    const char *fetch_response;
} mail_stub;

static int expect_contains(const char *label, sc_str text, const char *needle);
static sc_status make_mock_agent(sc_provider_mock_mode mode,
                                 sc_str text,
                                 sc_tool **tools,
                                 size_t tool_count,
                                 sc_provider **provider,
                                 sc_agent **agent);
static sc_status model_echo_generate(void *impl,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_response *out);
static void model_echo_destroy(void *impl);
static sc_status counting_tool_spec(void *impl, sc_tool_spec *out);
static sc_status counting_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void counting_tool_destroy(void *impl);
static sc_status mail_stub_request(void *user,
                                   sc_str operation,
                                   sc_str url,
                                   sc_str username,
                                   sc_str password,
                                   sc_str payload,
                                   sc_allocator *alloc,
                                   sc_string *out);
static void mail_stub_clear(mail_stub *stub);
static int test_common_channel_policy(void);
static int test_tts_audio_reply(void);
static int test_asr_audio_input(void);
static int test_acp_stdio_and_gateway(void);
static int test_named_webhook_router(void);
static int test_mail_alignment(void);
static int test_platform_reference_adapters(void);

static const sc_provider_vtab model_echo_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "model-echo",
    .display_name = "Model Echo",
    .feature_flag = "SC_TEST_MODEL_ECHO",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = model_echo_generate,
    .destroy = model_echo_destroy,
};

static const sc_tool_vtab counting_tool_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock_tool",
    .display_name = "Mock Tool",
    .feature_flag = "SC_TEST_COUNTING_TOOL",
    .capabilities = SC_CONTRACT_CAP_TOOLS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = counting_tool_spec,
    .invoke = counting_tool_invoke,
    .destroy = counting_tool_destroy,
};

int main()
{
    int failures = 0;
    failures += test_common_channel_policy();
    failures += test_tts_audio_reply();
    failures += test_asr_audio_input();
    failures += test_acp_stdio_and_gateway();
    failures += test_named_webhook_router();
    failures += test_mail_alignment();
    failures += test_platform_reference_adapters();
    return failures == 0 ? 0 : 1;
}

static int test_asr_audio_input(void)
{
    int failures = 0;
    char path[128] = {0};
    FILE *file = nullptr;
    const unsigned char audio[] = "not-a-real-ogg-but-path-backed";
    model_echo_provider echo_impl = {0};
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *fake = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_transcriber *transcriber = nullptr;
    sc_channel_process_result process = {0};
    int written = snprintf(path, sizeof(path), "/tmp/smolclaw-asr-%ld.ogg", (long)getpid());

    failures += sc_test_expect_true("asr path format", written > 0 && (size_t)written < sizeof(path));
    file = fopen(path, "wb");
    failures += sc_test_expect_true("asr file open", file != nullptr);
    if (file != nullptr) {
        failures += sc_test_expect_true("asr file write", fwrite(audio, 1, sizeof(audio) - 1, file) == sizeof(audio) - 1);
        failures += sc_test_expect_true("asr file close", fclose(file) == 0);
    }
    failures += sc_test_expect_status("asr provider", sc_provider_new(sc_allocator_heap(), &model_echo_vtab, &echo_impl, &provider), SC_OK);
    failures += sc_test_expect_status("asr agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .model = sc_str_from_cstr("asr-model"),
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("asr fake", sc_channel_fake_new(sc_allocator_heap(), 0, &fake), SC_OK);
    failures += sc_test_expect_status("asr transcriber",
                              sc_transcriber_mock_new(sc_allocator_heap(), sc_str_from_cstr("hello from voice"), false, &transcriber),
                              SC_OK);
    failures += sc_test_expect_status("asr orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = &fake,
                                                              .channel_count = 1,
                                                              .transcriber = transcriber,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("asr process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              fake,
                                                              &(sc_channel_inbound){
                                                                  .struct_size = sizeof(sc_channel_inbound),
                                                                  .message_id = sc_str_from_cstr("asr1"),
                                                                  .channel_name = sc_str_from_cstr("fake"),
                                                                  .conversation_id = sc_str_from_cstr("room"),
                                                                  .sender_id = sc_str_from_cstr("alice"),
                                                                  .text = sc_str_from_cstr("typed context"),
                                                                  .attachment_media_type = sc_str_from_cstr("audio/ogg"),
                                                                  .attachment_name = sc_str_from_cstr("voice.ogg"),
                                                                  .attachment_storage_path = sc_str_from_cstr(path),
                                                                  .attachment_size_bytes = sizeof(audio) - 1,
                                                              },
                                                              sc_allocator_heap(),
                                                              &process),
                              SC_OK);
    failures += sc_test_expect_true("asr processed", process.processed && echo_impl.calls == 1);
    failures += expect_contains("asr prompt text", sc_string_as_str(&echo_impl.last_prompt), "User text: typed context");
    failures += expect_contains("asr prompt transcript", sc_string_as_str(&echo_impl.last_prompt), "Media transcript: hello from voice");

    sc_channel_process_result_clear(&process);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(fake);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_transcriber_destroy(transcriber);
    sc_string_clear(&echo_impl.last_prompt);
    sc_string_clear(&echo_impl.last_tool_specs_json);
    (void)remove(path);
    return failures;
}

static int test_tts_audio_reply(void)
{
    int failures = 0;
    const unsigned char wav[] = {'R', 'I', 'F', 'F', 16, 0, 0, 0, 'W', 'A', 'V', 'E', 4, 0, 0, 0};
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *fake = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_tts *tts = nullptr;
    sc_channel_process_result process = {0};
    sc_buf audio = {0};

    failures += sc_test_expect_status("tts mock", sc_tts_mock_new(sc_allocator_heap(), sc_buf_from_parts(wav, sizeof(wav)), sc_str_from_cstr("audio/wav"), false, &tts), SC_OK);
    failures += sc_test_expect_status("tts agent", make_mock_agent(SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("spoken response"), nullptr, 0, &provider, &agent), SC_OK);
    failures += sc_test_expect_status("tts fake",
                              sc_channel_fake_new(sc_allocator_heap(),
                                                  SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM | SC_CHANNEL_CAP_APPROVAL_PROMPTS |
                                                      SC_CHANNEL_CAP_ATTACHMENTS,
                                                  &fake),
                              SC_OK);
    failures += sc_test_expect_status("tts orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = &fake,
                                                              .channel_count = 1,
                                                              .tts = tts,
                                                              .tts_reply_mode = SC_CHANNEL_TTS_REPLY_TEXT_AND_AUDIO,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("tts process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              fake,
                                                              INBOUND("tts1", "fake", "room", "alice", "say it"),
                                                              sc_allocator_heap(),
                                                              &process),
                              SC_OK);
    audio = sc_channel_fake_last_sent_attachment_bytes(fake);
    failures += sc_test_expect_true("tts processed", process.processed && sc_channel_fake_sent_count(fake) == 2);
    failures += sc_test_expect_true("tts attachment type", sc_str_equal(sc_channel_fake_last_sent_attachment_content_type(fake), sc_str_from_cstr("audio/wav")));
    failures += sc_test_expect_true("tts attachment name", sc_str_equal(sc_channel_fake_last_sent_attachment_filename(fake), sc_str_from_cstr("smolclaw-reply.wav")));
    failures += sc_test_expect_true("tts attachment bytes", audio.len == sizeof(wav) && memcmp(audio.ptr, wav, sizeof(wav)) == 0);

    sc_channel_process_result_clear(&process);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(fake);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tts_destroy(tts);
    return failures;
}

static int expect_contains(const char *label, sc_str text, const char *needle)
{
    if (text.ptr == nullptr || needle == nullptr || strstr(text.ptr, needle) == nullptr) {
        (void)fprintf(stderr, "%s: expected '%s' to contain '%s'\n", label, text.ptr == nullptr ? "" : text.ptr, needle == nullptr ? "" : needle);
        return 1;
    }
    return 0;
}

static sc_status make_mock_agent(sc_provider_mock_mode mode,
                                 sc_str text,
                                 sc_tool **tools,
                                 size_t tool_count,
                                 sc_provider **provider,
                                 sc_agent **agent)
{
    sc_status status = sc_provider_mock_new(sc_allocator_heap(), mode, text, provider);
    if (sc_status_is_ok(status)) {
        status = sc_agent_new(sc_allocator_heap(),
                              &(sc_agent_options){
                                  .struct_size = sizeof(sc_agent_options),
                                  .provider = *provider,
                                  .tools = tools,
                                  .tool_count = tool_count,
                                  .model = sc_str_from_cstr("default-model"),
                                  .use_streaming = true,
                                  .emit_stream_deltas = true,
                              },
                              agent);
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_destroy(*provider);
        *provider = nullptr;
    }
    return status;
}

static int test_common_channel_policy(void)
{
    int failures = 0;
    sc_channel *fake = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result process = {0};
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    model_echo_provider echo_impl = {0};
    sc_str allowed_user[] = {sc_str_from_cstr("alice")};
    sc_str allowed_chat[] = {sc_str_from_cstr("room")};
    sc_channel_common_config mention_config = {
        .struct_size = sizeof(sc_channel_common_config),
        .channel_name = sc_str_from_cstr("fake"),
        .enabled = true,
        .enabled_set = true,
        .allowed_users = allowed_user,
        .allowed_user_count = 1,
        .allowed_chats = allowed_chat,
        .allowed_chat_count = 1,
        .reply_to_mentions_only = true,
        .mention_token = sc_str_from_cstr("@sc"),
        .model = sc_str_from_cstr("channel-model"),
        .pairing_required = true,
        .paired = true,
    };

    failures += sc_test_expect_status("model provider", sc_provider_new(sc_allocator_heap(), &model_echo_vtab, &echo_impl, &provider), SC_OK);
    failures += sc_test_expect_status("model agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .model = sc_str_from_cstr("default-model"),
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("fake channel", sc_channel_fake_new(sc_allocator_heap(), 0, &fake), SC_OK);
    failures += sc_test_expect_status("orchestrator disabled",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .common_configs = &(sc_channel_common_config){
                                                                  .struct_size = sizeof(sc_channel_common_config),
                                                                  .channel_name = sc_str_from_cstr("fake"),
                                                                  .enabled = false,
                                                                  .enabled_set = true,
                                                              },
                                                              .common_config_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("disabled process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              fake,
                                                              INBOUND("m1", "fake", "room", "alice", "@sc hello"),
                                                              sc_allocator_heap(),
                                                              &process),
                              SC_OK);
    failures += sc_test_expect_true("disabled denied", process.denied && !process.processed && echo_impl.calls == 0);
    failures += sc_test_expect_true("disabled no reply", sc_channel_fake_sent_count(fake) == 0);
    sc_channel_process_result_clear(&process);
    sc_channel_orchestrator_destroy(orchestrator);
    orchestrator = nullptr;

    failures += sc_test_expect_status("orchestrator unpaired",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .common_configs = &(sc_channel_common_config){
                                                                  .struct_size = sizeof(sc_channel_common_config),
                                                                  .channel_name = sc_str_from_cstr("fake"),
                                                                  .enabled = true,
                                                                  .enabled_set = true,
                                                                  .pairing_required = true,
                                                                  .paired = false,
                                                              },
                                                              .common_config_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("unpaired process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              fake,
                                                              INBOUND("m2", "fake", "room", "alice", "@sc hello"),
                                                              sc_allocator_heap(),
                                                              &process),
                              SC_OK);
    failures += sc_test_expect_true("unpaired denied", process.denied && echo_impl.calls == 0);
    sc_channel_process_result_clear(&process);
    sc_channel_orchestrator_destroy(orchestrator);
    orchestrator = nullptr;

    failures += sc_test_expect_status("orchestrator mention",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .common_configs = &mention_config,
                                                              .common_config_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("mention denied",
                              sc_channel_orchestrator_process(orchestrator,
                                                              fake,
                                                              INBOUND("m3", "fake", "room", "alice", "hello"),
                                                              sc_allocator_heap(),
                                                              &process),
                              SC_OK);
    failures += sc_test_expect_true("mention missing denied", process.denied);
    sc_channel_process_result_clear(&process);
    failures += sc_test_expect_status("mention allowed",
                              sc_channel_orchestrator_process(orchestrator,
                                                              fake,
                                                              INBOUND("m4", "fake", "room", "alice", "@sc hello"),
                                                              sc_allocator_heap(),
                                                              &process),
                              SC_OK);
    failures += sc_test_expect_true("mention processed", process.processed && !process.denied);
    failures += expect_contains("model override reply", sc_channel_fake_last_sent_text(fake), "channel-model");
    sc_channel_process_result_clear(&process);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(fake);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);

    sc_string_clear(&echo_impl.last_tool_specs_json);
    provider = nullptr;
    agent = nullptr;
    fake = nullptr;
    failures += sc_test_expect_status("fake for tools", sc_channel_fake_new(sc_allocator_heap(), 0, &fake), SC_OK);
    counting_tool tool_impl = {0};
    model_echo_provider tool_echo = {0};
    sc_tool *tool = nullptr;
    sc_tool *tools[] = {tool};
    sc_str allowed_tools[] = {sc_str_from_cstr("mock_tool")};
    sc_str denied_tools[] = {sc_str_from_cstr("mock_tool")};
    failures += sc_test_expect_status("counting tool", sc_tool_new(sc_allocator_heap(), &counting_tool_vtab, &tool_impl, &tool), SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("tool provider", sc_provider_new(sc_allocator_heap(), &model_echo_vtab, &tool_echo, &provider), SC_OK);
    failures += sc_test_expect_status("tool agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                              .model = sc_str_from_cstr("default-model"),
                                           },
                                           &agent),
                              SC_OK);
    orchestrator = nullptr;
    failures += sc_test_expect_status("orchestrator allow deny conflict",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .common_configs = &(sc_channel_common_config){
                                                                  .struct_size = sizeof(sc_channel_common_config),
                                                                  .channel_name = sc_str_from_cstr("fake"),
                                                                  .tools_allow = allowed_tools,
                                                                  .tools_allow_count = 1,
                                                                  .tools_deny = denied_tools,
                                                                  .tools_deny_count = 1,
                                                              },
                                                              .common_config_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_ERR_INVALID_ARGUMENT);
    orchestrator = nullptr;
    failures += sc_test_expect_status("orchestrator tools",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .common_configs = &(sc_channel_common_config){
                                                                  .struct_size = sizeof(sc_channel_common_config),
                                                                  .channel_name = sc_str_from_cstr("fake"),
                                                                  .enabled = true,
                                                                  .enabled_set = true,
                                                                  .tools_deny = denied_tools,
                                                                  .tools_deny_count = 1,
                                                              },
                                                              .common_config_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("tool denied process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              fake,
                                                              INBOUND("m5", "fake", "room", "alice", "use tool"),
                                                              sc_allocator_heap(),
                                                              &process),
                              SC_OK);
    failures += sc_test_expect_true("tool not invoked", tool_impl.calls == 0);
    failures += sc_test_expect_true("tool not advertised", tool_echo.last_tool_specs_json.ptr != nullptr && strcmp(tool_echo.last_tool_specs_json.ptr, "[]") == 0);
    sc_channel_process_result_clear(&process);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_tool_destroy(tool);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_channel_destroy(fake);
    sc_string_clear(&tool_echo.last_tool_specs_json);
    return failures;
}

static int test_acp_stdio_and_gateway(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_acp_server *acp = nullptr;
    sc_gateway_server *gateway = nullptr;
    sc_gateway_response response = {0};
    sc_string out = {0};

    failures += sc_test_expect_status("acp agent", make_mock_agent(SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("acp reply"), nullptr, 0, &provider, &agent), SC_OK);
    failures += sc_test_expect_status("acp new",
                              sc_acp_server_new(sc_allocator_heap(),
                                                &(sc_acp_options){
                                                    .struct_size = sizeof(sc_acp_options),
                                                    .agent = agent,
                                                    .default_model = sc_str_from_cstr("acp-model"),
                                                    .max_sessions = 1,
                                                    .idle_timeout_secs = 60,
                                                    .approval_requests_enabled = true,
                                                },
                                                &acp),
                              SC_OK);
    failures += sc_test_expect_status("acp initialize",
                              sc_acp_handle_line(acp,
                                                 sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}"),
                                                 sc_allocator_heap(),
                                                 &out),
                              SC_OK);
    failures += expect_contains("acp initialize response", sc_string_as_str(&out), "\"defaultModel\":\"acp-model\"");
    sc_string_clear(&out);
    failures += sc_test_expect_status("acp session new",
                              sc_acp_handle_line(acp,
                                                 sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session/new\",\"params\":{\"cwd\":\".\",\"systemPrompt\":\"sys\"}}"),
                                                 sc_allocator_heap(),
                                                 &out),
                              SC_OK);
    failures += expect_contains("acp session id", sc_string_as_str(&out), "\"sessionId\":\"acp-1\"");
    sc_string_clear(&out);
    failures += sc_test_expect_status("acp session limit",
                              sc_acp_handle_line(acp,
                                                 sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"session/new\",\"params\":{}}"),
                                                 sc_allocator_heap(),
                                                 &out),
                              SC_OK);
    failures += expect_contains("acp limit error", sc_string_as_str(&out), "ACP session limit reached");
    sc_string_clear(&out);
    failures += sc_test_expect_status("acp prompt",
                              sc_acp_handle_line(acp,
                                                 sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"session/prompt\",\"params\":{\"sessionId\":\"acp-1\",\"prompt\":\"hello\"}}"),
                                                 sc_allocator_heap(),
                                                 &out),
                              SC_OK);
    failures += expect_contains("acp text notification", sc_string_as_str(&out), "\"method\":\"session/update\"");
    failures += expect_contains("acp final notification", sc_string_as_str(&out), "\"kind\":\"final\"");
    failures += expect_contains("acp prompt result", sc_string_as_str(&out), "\"finished\":true");
    sc_string_clear(&out);
    failures += sc_test_expect_status("acp stop",
                              sc_acp_handle_line(acp,
                                                 sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"session/stop\",\"params\":{\"sessionId\":\"acp-1\"}}"),
                                                 sc_allocator_heap(),
                                                 &out),
                              SC_OK);
    failures += sc_test_expect_true("acp sessions removed", sc_acp_session_count(acp) == 0);
    sc_string_clear(&out);

    failures += sc_test_expect_status("gateway new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .agent = agent,
                                                        .auth_token = sc_str_from_cstr("token"),
                                                        .max_body_bytes = 4096,
                                                    },
                                                    &gateway),
                              SC_OK);
    failures += sc_test_expect_status("gateway start", sc_gateway_server_start(gateway), SC_OK);
    failures += sc_test_expect_status("gateway acp route",
                              sc_gateway_handle_request(gateway,
                                                        &(sc_gateway_request){
                                                            .struct_size = sizeof(sc_gateway_request),
                                                            .method = SC_GATEWAY_POST,
                                                            .path = sc_str_from_cstr("/acp"),
                                                            .auth_token = sc_str_from_cstr("token"),
                                                            .body = sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}"),
                                                        },
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("gateway acp status", response.status == 200);
    failures += expect_contains("gateway acp body", sc_string_as_str(&response.body), "\"protocolVersion\":\"0.1\"");
    sc_gateway_response_clear(&response);
    failures += sc_test_expect_status("gateway acp websocket",
                              sc_gateway_websocket_receive_text(gateway,
                                                                sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{}}"),
                                                                sc_str_from_cstr("token"),
                                                                sc_str_from_cstr("acp"),
                                                                sc_allocator_heap(),
                                                                &out),
                              SC_OK);
    failures += expect_contains("gateway acp websocket body", sc_string_as_str(&out), "\"protocolVersion\":\"0.1\"");
    sc_string_clear(&out);

    sc_gateway_server_destroy(gateway);
    sc_acp_server_destroy(acp);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_named_webhook_router(void)
{
    int failures = 0;
    sc_webhook_router *router = nullptr;
    sc_webhook_named_result result = {0};
    sc_str allow_one[] = {sc_str_from_cstr("203.0.113.7")};
    const sc_str body = sc_str_from_cstr("{\"message\":\"hello\",\"sender\":\"alice\"}");

    failures += sc_test_expect_status("webhook router", sc_webhook_router_new(sc_allocator_heap(), &router), SC_OK);
    failures += sc_test_expect_status("webhook signed add",
                              sc_webhook_router_add(router,
                                                    &(sc_webhook_named_options){
                                                        .struct_size = sizeof(sc_webhook_named_options),
                                                        .name = sc_str_from_cstr("signed"),
                                                        .enabled = true,
                                                        .path = sc_str_from_cstr("/hooks/signed"),
                                                        .secret = sc_str_from_cstr("secret"),
                                                        .allow_list = allow_one,
                                                        .allow_list_count = 1,
                                                        .dispatch_to = sc_str_from_cstr("conversation:signed-room"),
                                                        .template_text = sc_str_from_cstr("msg={{ payload.message }}"),
                                                        .reply_mode = SC_WEBHOOK_REPLY_INLINE_TEXT,
                                                        .rate_limit_per_sec = 10,
                                                        .max_body_bytes = 512,
                                                    }),
                              SC_OK);
    failures += sc_test_expect_status("webhook bad signature",
                              sc_webhook_router_ingest(router,
                                                       &(sc_webhook_named_request){
                                                           .struct_size = sizeof(sc_webhook_named_request),
                                                           .method = sc_str_from_cstr("POST"),
                                                           .path = sc_str_from_cstr("/hooks/signed"),
                                                           .remote_addr = sc_str_from_cstr("203.0.113.7"),
                                                           .body = body,
                                                           .x_hub_signature_256 = sc_str_from_cstr("sha256=bad"),
                                                       },
                                                       sc_allocator_heap(),
                                                       &result),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("webhook allow list",
                              sc_webhook_router_ingest(router,
                                                       &(sc_webhook_named_request){
                                                           .struct_size = sizeof(sc_webhook_named_request),
                                                           .method = sc_str_from_cstr("POST"),
                                                           .path = sc_str_from_cstr("/hooks/signed"),
                                                           .remote_addr = sc_str_from_cstr("198.51.100.9"),
                                                           .body = body,
                                                           .x_hub_signature_256 = sc_str_from_cstr("sha256=c31f657fcb575b1eb9857e81daed4548b476a1b04b950afee8e741060a65f03a"),
                                                       },
                                                       sc_allocator_heap(),
                                                       &result),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("webhook signed success",
                              sc_webhook_router_ingest(router,
                                                       &(sc_webhook_named_request){
                                                           .struct_size = sizeof(sc_webhook_named_request),
                                                           .method = sc_str_from_cstr("POST"),
                                                           .path = sc_str_from_cstr("/hooks/signed"),
                                                           .remote_addr = sc_str_from_cstr("203.0.113.7"),
                                                           .body = body,
                                                           .x_hub_signature_256 = sc_str_from_cstr("sha256=c31f657fcb575b1eb9857e81daed4548b476a1b04b950afee8e741060a65f03a"),
                                                       },
                                                       sc_allocator_heap(),
                                                       &result),
                              SC_OK);
    failures += sc_test_expect_true("webhook dispatch conversation", result.dispatch_kind == SC_WEBHOOK_DISPATCH_CONVERSATION);
    failures += expect_contains("webhook dispatch target", sc_string_as_str(&result.dispatch_target), "signed-room");
    failures += expect_contains("webhook template", sc_string_as_str(&result.rendered_text), "msg=hello");
    failures += expect_contains("webhook inline text", sc_string_as_str(&result.response_body), "msg=hello");
    sc_webhook_named_result_clear(&result);

    failures += sc_test_expect_status("webhook cron add",
                              sc_webhook_router_add(router,
                                                    &(sc_webhook_named_options){
                                                        .struct_size = sizeof(sc_webhook_named_options),
                                                        .name = sc_str_from_cstr("cron"),
                                                        .enabled = true,
                                                        .path = sc_str_from_cstr("/hooks/cron"),
                                                        .dispatch_to = sc_str_from_cstr("cron:nightly"),
                                                        .template_text = sc_str_from_cstr("cron {{ payload.sender }}"),
                                                        .reply_mode = SC_WEBHOOK_REPLY_STATUS_ONLY,
                                                        .rate_limit_per_sec = 1,
                                                        .max_body_bytes = 512,
                                                    }),
                              SC_OK);
    failures += sc_test_expect_status("webhook cron success",
                              sc_webhook_router_ingest(router,
                                                       &(sc_webhook_named_request){
                                                           .struct_size = sizeof(sc_webhook_named_request),
                                                           .method = sc_str_from_cstr("POST"),
                                                           .path = sc_str_from_cstr("/hooks/cron"),
                                                           .remote_addr = sc_str_from_cstr("10.0.0.1"),
                                                           .body = body,
                                                       },
                                                       sc_allocator_heap(),
                                                       &result),
                              SC_OK);
    failures += sc_test_expect_true("webhook cron kind", result.dispatch_kind == SC_WEBHOOK_DISPATCH_CRON);
    failures += expect_contains("webhook status reply", sc_string_as_str(&result.response_body), "OK");
    sc_webhook_named_result_clear(&result);
    failures += sc_test_expect_status("webhook rate limit",
                              sc_webhook_router_ingest(router,
                                                       &(sc_webhook_named_request){
                                                           .struct_size = sizeof(sc_webhook_named_request),
                                                           .method = sc_str_from_cstr("POST"),
                                                           .path = sc_str_from_cstr("/hooks/cron"),
                                                           .remote_addr = sc_str_from_cstr("10.0.0.1"),
                                                           .body = body,
                                                       },
                                                       sc_allocator_heap(),
                                                       &result),
                              SC_ERR_SECURITY_DENIED);

    failures += sc_test_expect_status("webhook system add",
                              sc_webhook_router_add(router,
                                                    &(sc_webhook_named_options){
                                                        .struct_size = sizeof(sc_webhook_named_options),
                                                        .name = sc_str_from_cstr("system"),
                                                        .enabled = true,
                                                        .path = sc_str_from_cstr("/hooks/system"),
                                                        .dispatch_to = sc_str_from_cstr("system:run-once"),
                                                        .template_text = sc_str_from_cstr("payload={{ payload }}"),
                                                        .reply_mode = SC_WEBHOOK_REPLY_INLINE_JSON,
                                                        .max_body_bytes = 512,
                                                    }),
                              SC_OK);
    failures += sc_test_expect_status("webhook system success",
                              sc_webhook_router_ingest(router,
                                                       &(sc_webhook_named_request){
                                                           .struct_size = sizeof(sc_webhook_named_request),
                                                           .method = sc_str_from_cstr("POST"),
                                                           .path = sc_str_from_cstr("/hooks/system"),
                                                           .remote_addr = sc_str_from_cstr("10.0.0.1"),
                                                           .body = body,
                                                       },
                                                       sc_allocator_heap(),
                                                       &result),
                              SC_OK);
    failures += sc_test_expect_true("webhook system kind", result.dispatch_kind == SC_WEBHOOK_DISPATCH_SYSTEM_PROMPT);
    failures += expect_contains("webhook json reply", sc_string_as_str(&result.response_body), "{\"text\":\"payload=");
    sc_webhook_named_result_clear(&result);
    sc_webhook_router_destroy(router);
    return failures;
}

static int test_mail_alignment(void)
{
    int failures = 0;
    mail_stub stub = {
        .fetch_response = "Message-ID: <mail-2>\r\n"
                          "From: sender@example.test\r\n"
                          "Subject: [SC] Inbox prompt\r\n"
                          "Attachment-Name: report.pdf\r\n"
                          "Attachment-Type: application/pdf\r\n"
                          "Attachment-Size: 42\r\n"
                          "\r\n"
                          "Body text\r\n",
    };
    sc_channel *mail = nullptr;
    sc_channel *denied_mail = nullptr;
    sc_channel_inbound inbound = {0};
    sc_str allowed[] = {sc_str_from_cstr("sender@example.test")};
    sc_str denied[] = {sc_str_from_cstr("other@example.test")};

    failures += sc_test_expect_status("mail aligned new",
                              sc_channel_mail_new(sc_allocator_heap(),
                                                  &(sc_mail_channel_options){
                                                      .struct_size = sizeof(sc_mail_channel_options),
                                                      .inbox_url = sc_str_from_cstr("imap://mail.example.test/INBOX;UID=2"),
                                                      .smtp_url = sc_str_from_cstr("smtp://mail.example.test"),
                                                      .username = sc_str_from_cstr("agent"),
                                                      .password = sc_str_from_cstr("password"),
                                                      .oauth_token = sc_str_from_cstr("oauth-token"),
                                                      .from = sc_str_from_cstr("agent@example.test"),
                                                      .to = sc_str_from_cstr("sender@example.test"),
                                                      .mailbox = sc_str_from_cstr("INBOX"),
                                                      .poll_interval_seconds = 15,
                                                      .allowed_senders = allowed,
                                                      .allowed_sender_count = 1,
                                                      .subject_prefix = sc_str_from_cstr("[SC]"),
                                                      .attachment_dir = sc_str_from_cstr("/tmp/sc-attachments"),
                                                      .max_message_bytes = 4096,
                                                      .request = mail_stub_request,
                                                      .request_user = &stub,
                                                  },
                                                  &mail),
                              SC_OK);
    failures += sc_test_expect_status("mail aligned listen", sc_channel_listen(mail, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("mail oauth fetch", strcmp(stub.password.ptr, "oauth-token") == 0);
    failures += expect_contains("mail subject prefix inbound", inbound.text, "[SC] Inbox prompt");
    failures += sc_test_expect_true("mail attachment size", inbound.attachment_size_bytes == 42);
    failures += expect_contains("mail attachment path", inbound.attachment_storage_path, "/tmp/sc-attachments/");
    failures += expect_contains("mail attachment name", inbound.attachment_name, "report.pdf");
    sc_channel_inbound_clear(&inbound);

    failures += sc_test_expect_status("mail reply",
                              sc_channel_send(mail,
                                              &(sc_channel_message){
                                                  .struct_size = sizeof(sc_channel_message),
                                                  .conversation_id = sc_str_from_cstr("imap://mail.example.test/INBOX;UID=2"),
                                                  .sender_id = sc_str_from_cstr("sender@example.test"),
                                                  .text = sc_str_from_cstr("Reply body"),
                                                  .reply_to_message_id = sc_str_from_cstr("<mail-2>"),
                                              }),
                              SC_OK);
    failures += sc_test_expect_true("mail oauth send", strcmp(stub.password.ptr, "oauth-token") == 0);
    failures += expect_contains("mail subject prefix send", sc_string_as_str(&stub.payload), "Subject: [SC] SmolClaw reply");
    failures += expect_contains("mail in reply to", sc_string_as_str(&stub.payload), "In-Reply-To: <mail-2>");
    failures += expect_contains("mail references", sc_string_as_str(&stub.payload), "References: <mail-2>");

    failures += sc_test_expect_status("mail denied new",
                              sc_channel_mail_new(sc_allocator_heap(),
                                                  &(sc_mail_channel_options){
                                                      .struct_size = sizeof(sc_mail_channel_options),
                                                      .inbox_url = sc_str_from_cstr("imap://mail.example.test/INBOX;UID=3"),
                                                      .smtp_url = sc_str_from_cstr("smtp://mail.example.test"),
                                                      .username = sc_str_from_cstr("agent"),
                                                      .password = sc_str_from_cstr("password"),
                                                      .from = sc_str_from_cstr("agent@example.test"),
                                                      .to = sc_str_from_cstr("sender@example.test"),
                                                      .allowed_senders = denied,
                                                      .allowed_sender_count = 1,
                                                      .max_message_bytes = 4096,
                                                      .request = mail_stub_request,
                                                      .request_user = &stub,
                                                  },
                                                  &denied_mail),
                              SC_OK);
    failures += sc_test_expect_status("mail sender denied", sc_channel_listen(denied_mail, sc_allocator_heap(), &inbound), SC_ERR_SECURITY_DENIED);
    sc_channel_inbound_clear(&inbound);
    sc_channel_destroy(denied_mail);
    sc_channel_destroy(mail);
    mail_stub_clear(&stub);
    return failures;
}

static int test_platform_reference_adapters(void)
{
    int failures = 0;
    sc_channel *channel = nullptr;
    const sc_channel_vtab *vtab = nullptr;
    const sc_platform_channel_options options = {
        .struct_size = sizeof(sc_platform_channel_options),
        .endpoint = sc_str_from_cstr("https://chat.example.test"),
        .bot_token = sc_str_from_cstr("token"),
        .bot_user_id = sc_str_from_cstr("bot"),
        .bot_display_name = sc_str_from_cstr("SmolClaw"),
        .thread_replies = true,
        .ack_reactions = true,
        .draft_updates = true,
        .multi_message_streaming = true,
        .device_id = sc_str_from_cstr("DEVICE"),
        .channel_secret = sc_str_from_cstr("secret"),
        .access_token = sc_str_from_cstr("access-token"),
    };

    failures += sc_test_expect_status("matrix e2ee device required",
                              sc_channel_matrix_new(sc_allocator_heap(),
                                                    &(sc_platform_channel_options){
                                                        .struct_size = sizeof(sc_platform_channel_options),
                                                        .endpoint = sc_str_from_cstr("https://matrix.example.test"),
                                                        .e2ee_enabled = true,
                                                    },
                                                    &channel),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("matrix new", sc_channel_matrix_new(sc_allocator_heap(), &options, &channel), SC_OK);
    vtab = sc_channel_vtab_of(channel);
    failures += sc_test_expect_true("matrix caps", vtab != nullptr && strcmp(vtab->name, "matrix") == 0 && (vtab->capabilities & SC_CHANNEL_CAP_THREAD_REPLIES) != 0);
    sc_channel_destroy(channel);
    channel = nullptr;
    failures += sc_test_expect_status("mattermost new", sc_channel_mattermost_new(sc_allocator_heap(), &options, &channel), SC_OK);
    vtab = sc_channel_vtab_of(channel);
    failures += sc_test_expect_true("mattermost caps", vtab != nullptr && strcmp(vtab->name, "mattermost") == 0 && (vtab->capabilities & SC_CHANNEL_CAP_DRAFT_UPDATES) != 0);
    sc_channel_destroy(channel);
    channel = nullptr;
    failures += sc_test_expect_status("line new", sc_channel_line_new(sc_allocator_heap(), &options, &channel), SC_OK);
    vtab = sc_channel_vtab_of(channel);
    failures += sc_test_expect_true("line caps", vtab != nullptr && strcmp(vtab->name, "line") == 0 && (vtab->capabilities & SC_CHANNEL_CAP_INLINE_APPROVAL_BUTTONS) != 0);
    sc_channel_destroy(channel);
    channel = nullptr;
    failures += sc_test_expect_status("nextcloud new", sc_channel_nextcloud_talk_new(sc_allocator_heap(), &options, &channel), SC_OK);
    vtab = sc_channel_vtab_of(channel);
    failures += sc_test_expect_true("nextcloud caps", vtab != nullptr && strcmp(vtab->name, "nextcloud-talk") == 0);
    sc_channel_destroy(channel);
    channel = nullptr;
    failures += sc_test_expect_status("discord new", sc_channel_discord_new(sc_allocator_heap(), &options, &channel), SC_OK);
    vtab = sc_channel_vtab_of(channel);
    failures += sc_test_expect_true("discord caps", vtab != nullptr && strcmp(vtab->name, "discord") == 0 && (vtab->capabilities & SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM) != 0);
    sc_channel_destroy(channel);
    channel = nullptr;
    failures += sc_test_expect_status("slack new", sc_channel_slack_new(sc_allocator_heap(), &options, &channel), SC_OK);
    vtab = sc_channel_vtab_of(channel);
    failures += sc_test_expect_true("slack caps", vtab != nullptr && strcmp(vtab->name, "slack") == 0 && (vtab->capabilities & SC_CHANNEL_CAP_THREAD_REPLIES) != 0);
    sc_channel_destroy(channel);
    channel = nullptr;
    failures += sc_test_expect_status("signal new", sc_channel_signal_new(sc_allocator_heap(), &options, &channel), SC_OK);
    vtab = sc_channel_vtab_of(channel);
    failures += sc_test_expect_true("signal caps", vtab != nullptr && strcmp(vtab->name, "signal") == 0 && (vtab->capabilities & SC_CHANNEL_CAP_ATTACHMENTS) != 0);
    sc_channel_destroy(channel);
    return failures;
}

static sc_status model_echo_generate(void *impl,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_response *out)
{
    model_echo_provider *provider = impl;
    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("test.model_echo.invalid_argument");
    }
    provider->calls += 1;
    sc_string_clear(&provider->last_prompt);
    sc_string_clear(&provider->last_tool_specs_json);
    sc_status status = sc_string_from_str(sc_allocator_heap(), request->prompt, &provider->last_prompt);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), request->tool_specs_json, &provider->last_tool_specs_json);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    sc_provider_response_init(out, alloc);
    return sc_string_from_str(alloc, request->model.len == 0 ? sc_str_from_cstr("empty-model") : request->model, &out->text);
}

static void model_echo_destroy(void *impl)
{
    model_echo_provider *provider = impl;

    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->last_prompt);
    sc_string_clear(&provider->last_tool_specs_json);
}

static sc_status counting_tool_spec(void *impl, sc_tool_spec *out)
{
    (void)impl;
    if (out == nullptr) {
        return sc_status_invalid_argument("test.tool.spec_invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(sc_tool_spec),
        .name = sc_str_from_cstr("mock_tool"),
        .description = sc_str_from_cstr("count calls"),
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
    };
    return sc_status_ok();
}

static sc_status counting_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    counting_tool *tool = impl;
    (void)call;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("test.tool.invoke_invalid_argument");
    }
    tool->calls += 1;
    *out = (sc_tool_result){.struct_size = sizeof(sc_tool_result), .success = true};
    return sc_string_from_cstr(alloc, "called", &out->output);
}

static void counting_tool_destroy(void *impl)
{
    (void)impl;
}

static sc_status mail_stub_request(void *user,
                                   sc_str operation,
                                   sc_str url,
                                   sc_str username,
                                   sc_str password,
                                   sc_str payload,
                                   sc_allocator *alloc,
                                   sc_string *out)
{
    mail_stub *stub = user;
    sc_status status;
    if (stub == nullptr || out == nullptr) {
        return sc_status_invalid_argument("test.mail.stub_invalid_argument");
    }
    mail_stub_clear(stub);
    status = sc_string_from_str(sc_allocator_heap(), operation, &stub->operation);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), url, &stub->url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), username, &stub->username);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), password, &stub->password);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), payload, &stub->payload);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, sc_str_equal(operation, sc_str_from_cstr("fetch")) ? stub->fetch_response : "OK", out);
    }
    return status;
}

static void mail_stub_clear(mail_stub *stub)
{
    if (stub == nullptr) {
        return;
    }
    sc_string_clear(&stub->operation);
    sc_string_clear(&stub->url);
    sc_string_clear(&stub->username);
    sc_string_secure_clear(&stub->password);
    sc_string_clear(&stub->payload);
}
