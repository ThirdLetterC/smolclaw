const std = @import("std");

fn addCommonSettings(
    step: *std.Build.Step.Compile,
    b: *std.Build,
    enable_ssl: bool,
    amq_platform_define: []const u8,
) void {
    step.addIncludePath(b.path("include"));
    step.addIncludePath(b.path("src"));
    if (enable_ssl) {
        step.addIncludePath(b.path("src/unix"));
    }

    step.root_module.addCMacro("HAVE_POLL", "1");
    step.root_module.addCMacro("_POSIX_C_SOURCE", "200809L");

    step.root_module.addCMacro("AMQ_PLATFORM", amq_platform_define);
    step.linkLibC();
}

fn linkPlatformLibs(step: *std.Build.Step.Compile, target: std.Build.ResolvedTarget) void {
    switch (target.result.os.tag) {
        .linux => {
            step.linkSystemLibrary("pthread");
            step.linkSystemLibrary("rt");
        },
        else => {
            step.linkSystemLibrary("pthread");
        },
    }
}

fn addHardeningSettings(
    step: *std.Build.Step.Compile,
    target: std.Build.ResolvedTarget,
    enable_hardening: bool,
    enable_sanitizers: bool,
) void {
    if (enable_sanitizers) {
        step.root_module.sanitize_c = .full;
    }

    if (!enable_hardening) {
        return;
    }

    // Keep relocations read-only and resolved at startup.
    step.link_z_relro = true;
    step.link_z_lazy = false;

    if (target.result.os.tag == .linux and (step.kind == .exe or step.kind == .@"test")) {
        step.pie = true;
    }
}

fn addRabbitmqSources(
    step: *std.Build.Step.Compile,
    enable_ssl: bool,
    cflags: []const []const u8,
) void {
    const base_sources = [_][]const u8{
        "src/amqp_api.c",
        "src/amqp_connection.c",
        "src/amqp_consumer.c",
        "src/amqp_framing.c",
        "src/amqp_mem.c",
        "src/amqp_socket.c",
        "src/amqp_table.c",
        "src/amqp_tcp_socket.c",
        "src/amqp_time.c",
        "src/amqp_url.c",
    };

    step.addCSourceFiles(.{
        .files = base_sources[0..],
        .flags = cflags,
    });

    if (enable_ssl) {
        const ssl_sources = [_][]const u8{
            "src/amqp_openssl.c",
            "src/amqp_openssl_bio.c",
        };
        step.addCSourceFiles(.{
            .files = ssl_sources[0..],
            .flags = cflags,
        });
    }
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const enable_ssl = b.option(bool, "ssl", "Enable OpenSSL support") orelse false;
    const build_shared = b.option(bool, "shared", "Build shared library") orelse true;
    const build_static = b.option(bool, "static", "Build static library") orelse true;
    const build_examples = b.option(bool, "examples", "Build examples") orelse true;
    const build_tools = b.option(bool, "tools", "Build CLI tools (requires popt)") orelse true;
    const build_tests = b.option(bool, "tests", "Build tests") orelse true;
    const enable_hardening = b.option(bool, "hardening", "Enable hardening linker/compiler flags") orelse true;
    const enable_sanitizers = b.option(
        bool,
        "sanitizers",
        "Enable Zig C sanitizer mode (-fsanitize-c=full)",
    ) orelse false;

    if (target.result.os.tag == .windows) {
        @panic("Windows targets are not supported by this build configuration");
    }

    if (!build_shared and !build_static) {
        @panic("At least one of -Dshared or -Dstatic must be true");
    }

    const amq_platform_define = b.fmt("\"{s}-{s}\"", .{
        @tagName(target.result.cpu.arch),
        @tagName(target.result.os.tag),
    });

    var cflags = std.array_list.Managed([]const u8).init(b.allocator);
    cflags.appendSlice(&.{
        "-std=c23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    }) catch @panic("failed to append base C flags");

    if (enable_hardening) {
        cflags.append("-fstack-protector-strong") catch @panic("failed to append hardening C flags");
        if (optimize != .Debug) {
            cflags.append("-D_FORTIFY_SOURCE=3") catch @panic("failed to append fortify C flag");
        }
    }

    const mk_module = struct {
        fn create(bld: *std.Build, tgt: std.Build.ResolvedTarget, opt: std.builtin.OptimizeMode) *std.Build.Module {
            return bld.createModule(.{
                .target = tgt,
                .optimize = opt,
                .link_libc = true,
            });
        }
    }.create;

    var shared_lib: ?*std.Build.Step.Compile = null;
    var static_lib: ?*std.Build.Step.Compile = null;

    if (build_shared) {
        const lib = b.addLibrary(.{
            .name = "rabbitmq",
            .root_module = mk_module(b, target, optimize),
            .linkage = .dynamic,
        });
        addCommonSettings(lib, b, enable_ssl, amq_platform_define);
        addHardeningSettings(lib, target, enable_hardening, enable_sanitizers);
        addRabbitmqSources(lib, enable_ssl, cflags.items);
        lib.root_module.addCMacro("AMQP_EXPORTS", "1");
        linkPlatformLibs(lib, target);
        if (enable_ssl) {
            lib.linkSystemLibrary("ssl");
            lib.linkSystemLibrary("crypto");
        }
        b.installArtifact(lib);
        shared_lib = lib;
    }

    if (build_static) {
        const lib = b.addLibrary(.{
            .name = "rabbitmq",
            .root_module = mk_module(b, target, optimize),
            .linkage = .static,
        });
        addCommonSettings(lib, b, enable_ssl, amq_platform_define);
        addHardeningSettings(lib, target, enable_hardening, enable_sanitizers);
        addRabbitmqSources(lib, enable_ssl, cflags.items);
        lib.root_module.addCMacro("AMQP_STATIC", "1");
        linkPlatformLibs(lib, target);
        if (enable_ssl) {
            lib.linkSystemLibrary("ssl");
            lib.linkSystemLibrary("crypto");
        }
        b.installArtifact(lib);
        static_lib = lib;
    }

    const link_lib = if (shared_lib) |lib| lib else static_lib.?;
    const use_static = shared_lib == null;

    if (build_examples) {
        const examples_step = b.step("examples", "Build example binaries");
        const examples_common = b.addLibrary(.{
            .name = "examples-common",
            .root_module = mk_module(b, target, optimize),
            .linkage = .static,
        });
        examples_common.addIncludePath(b.path("examples"));
        addCommonSettings(examples_common, b, enable_ssl, amq_platform_define);
        addHardeningSettings(examples_common, target, enable_hardening, enable_sanitizers);
        examples_common.addCSourceFiles(.{
            .files = &.{
                "examples/utils.c",
                "examples/unix/platform_utils.c",
            },
            .flags = cflags.items,
        });
        if (use_static) {
            examples_common.root_module.addCMacro("AMQP_STATIC", "1");
        }

        const examples = [_]struct {
            name: []const u8,
            file: []const u8,
            requires_ssl: bool,
        }{
            .{ .name = "amqp_sendstring", .file = "examples/amqp_sendstring.c", .requires_ssl = false },
            .{ .name = "amqp_rpc_sendstring_client", .file = "examples/amqp_rpc_sendstring_client.c", .requires_ssl = false },
            .{ .name = "amqp_exchange_declare", .file = "examples/amqp_exchange_declare.c", .requires_ssl = false },
            .{ .name = "amqp_listen", .file = "examples/amqp_listen.c", .requires_ssl = false },
            .{ .name = "amqp_producer", .file = "examples/amqp_producer.c", .requires_ssl = false },
            .{ .name = "amqp_confirm_select", .file = "examples/amqp_confirm_select.c", .requires_ssl = false },
            .{ .name = "amqp_connect_timeout", .file = "examples/amqp_connect_timeout.c", .requires_ssl = false },
            .{ .name = "amqp_consumer", .file = "examples/amqp_consumer.c", .requires_ssl = false },
            .{ .name = "amqp_unbind", .file = "examples/amqp_unbind.c", .requires_ssl = false },
            .{ .name = "amqp_bind", .file = "examples/amqp_bind.c", .requires_ssl = false },
            .{ .name = "amqp_listenq", .file = "examples/amqp_listenq.c", .requires_ssl = false },
            .{ .name = "amqp_ssl_connect", .file = "examples/amqp_ssl_connect.c", .requires_ssl = true },
        };

        for (examples) |ex| {
            if (ex.requires_ssl and !enable_ssl) continue;

            const exe = b.addExecutable(.{
                .name = ex.name,
                .root_module = mk_module(b, target, optimize),
            });
            addCommonSettings(exe, b, enable_ssl, amq_platform_define);
            addHardeningSettings(exe, target, enable_hardening, enable_sanitizers);
            exe.addIncludePath(b.path("examples"));
            exe.addCSourceFiles(.{
                .files = &.{ex.file},
                .flags = cflags.items,
            });
            if (use_static) {
                exe.root_module.addCMacro("AMQP_STATIC", "1");
            }
            exe.linkLibrary(examples_common);
            exe.linkLibrary(link_lib);
            linkPlatformLibs(exe, target);
            if (enable_ssl) {
                exe.linkSystemLibrary("ssl");
                exe.linkSystemLibrary("crypto");
            }
            const install_exe = b.addInstallArtifact(exe, .{});
            b.getInstallStep().dependOn(&install_exe.step);
            examples_step.dependOn(&install_exe.step);
        }
    }

    if (build_tools) {
        const tools_step = b.step("tools", "Build CLI tools");
        const tools_common = b.addLibrary(.{
            .name = "tools-common",
            .root_module = mk_module(b, target, optimize),
            .linkage = .static,
        });
        tools_common.addIncludePath(b.path("tools"));
        tools_common.addIncludePath(b.path("tools/unix"));
        addCommonSettings(tools_common, b, enable_ssl, amq_platform_define);
        addHardeningSettings(tools_common, target, enable_hardening, enable_sanitizers);
        tools_common.addCSourceFiles(.{
            .files = &.{
                "tools/common.c",
            },
            .flags = cflags.items,
        });
        if (use_static) {
            tools_common.root_module.addCMacro("AMQP_STATIC", "1");
        }
        if (enable_ssl) {
            tools_common.root_module.addCMacro("WITH_SSL", "1");
        }

        const tools = [_]struct {
            name: []const u8,
            file: []const u8,
        }{
            .{ .name = "amqp-publish", .file = "tools/publish.c" },
            .{ .name = "amqp-get", .file = "tools/get.c" },
            .{ .name = "amqp-consume", .file = "tools/consume.c" },
            .{ .name = "amqp-declare-queue", .file = "tools/declare_queue.c" },
            .{ .name = "amqp-delete-queue", .file = "tools/delete_queue.c" },
        };

        for (tools) |tool| {
            const exe = b.addExecutable(.{
                .name = tool.name,
                .root_module = mk_module(b, target, optimize),
            });
            addCommonSettings(exe, b, enable_ssl, amq_platform_define);
            addHardeningSettings(exe, target, enable_hardening, enable_sanitizers);
            exe.addIncludePath(b.path("tools"));
            exe.addIncludePath(b.path("tools/unix"));
            exe.addCSourceFiles(.{
                .files = if (std.mem.eql(u8, tool.name, "amqp-consume"))
                    &.{
                        "tools/consume.c",
                        "tools/unix/process.c",
                    }
                else
                    &.{tool.file},
                .flags = cflags.items,
            });
            if (use_static) {
                exe.root_module.addCMacro("AMQP_STATIC", "1");
            }
            if (enable_ssl) {
                exe.root_module.addCMacro("WITH_SSL", "1");
                exe.linkSystemLibrary("ssl");
                exe.linkSystemLibrary("crypto");
            }
            exe.linkSystemLibrary("popt");
            exe.linkLibrary(tools_common);
            exe.linkLibrary(link_lib);
            linkPlatformLibs(exe, target);
            const install_exe = b.addInstallArtifact(exe, .{});
            b.getInstallStep().dependOn(&install_exe.step);
            tools_step.dependOn(&install_exe.step);
        }
    }

    if (build_tests) {
        const tests_step = b.step("tests", "Build tests");
        const tests = [_]struct {
            name: []const u8,
            file: []const u8,
        }{
            .{ .name = "test_parse_url", .file = "tests/test_parse_url.c" },
            .{ .name = "test_tables", .file = "tests/test_tables.c" },
            .{ .name = "test_status_enum", .file = "tests/test_status_enum.c" },
            .{ .name = "test_basic", .file = "tests/test_basic.c" },
            .{ .name = "test_sasl_mechanism", .file = "tests/test_sasl_mechanism.c" },
            .{ .name = "test_merge_capabilities", .file = "tests/test_merge_capabilities.c" },
            .{ .name = "test_timeouts", .file = "tests/test_timeouts.c" },
        };

        for (tests) |test_bin| {
            const exe = b.addExecutable(.{
                .name = test_bin.name,
                .root_module = mk_module(b, target, optimize),
            });
            addCommonSettings(exe, b, enable_ssl, amq_platform_define);
            addHardeningSettings(exe, target, enable_hardening, enable_sanitizers);
            exe.addIncludePath(b.path("tests"));
            exe.addCSourceFiles(.{
                .files = &.{test_bin.file},
                .flags = cflags.items,
            });
            if (use_static) {
                exe.root_module.addCMacro("AMQP_STATIC", "1");
            }
            exe.linkLibrary(link_lib);
            linkPlatformLibs(exe, target);
            if (enable_ssl) {
                exe.linkSystemLibrary("ssl");
                exe.linkSystemLibrary("crypto");
            }
            const install_exe = b.addInstallArtifact(exe, .{});
            b.getInstallStep().dependOn(&install_exe.step);
            tests_step.dependOn(&install_exe.step);
        }
    }
}
