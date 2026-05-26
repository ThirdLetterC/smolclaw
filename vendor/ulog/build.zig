const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const enable_sanitizers = b.option(
        bool,
        "sanitizers",
        "Enable AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer",
    ) orelse false;

    const c_flags = createCFlags(b, false, enable_sanitizers);
    const c_flags_dynamic = createCFlags(b, true, enable_sanitizers);

    const exe = b.addExecutable(.{
        .name = "ulog_example",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    exe.root_module.addIncludePath(b.path("include"));
    exe.root_module.addCSourceFile(.{ .file = b.path("src/ulog.c"), .flags = c_flags });
    exe.root_module.addCSourceFile(.{ .file = b.path("examples/ulog_example.c"), .flags = c_flags });
    exe.linkLibC();
    if (enable_sanitizers) {
        linkSanitizers(b, exe);
    }

    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the ulog example");
    run_step.dependOn(&run_cmd.step);

    const all_features = b.addExecutable(.{
        .name = "ulog_all_features",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    all_features.root_module.addIncludePath(b.path("include"));
    all_features.root_module.addIncludePath(b.path("extensions"));
    all_features.root_module.addCSourceFile(.{ .file = b.path("src/ulog.c"), .flags = c_flags_dynamic });
    all_features.root_module.addCSourceFile(.{ .file = b.path("extensions/ulog_syslog.c"), .flags = c_flags_dynamic });
    all_features.root_module.addCSourceFile(.{ .file = b.path("examples/ulog_all_features.c"), .flags = c_flags_dynamic });
    all_features.linkLibC();
    if (enable_sanitizers) {
        linkSanitizers(b, all_features);
    }

    b.installArtifact(all_features);

    const run_all_cmd = b.addRunArtifact(all_features);
    run_all_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_all_cmd.addArgs(args);
    }

    const run_all_step = b.step("run-all-features", "Run the all-features example");
    run_all_step.dependOn(&run_all_cmd.step);

    const tests = b.addExecutable(.{
        .name = "ulog_tests",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    tests.root_module.addIncludePath(b.path("include"));
    tests.root_module.addCSourceFile(.{ .file = b.path("src/ulog.c"), .flags = c_flags_dynamic });
    tests.root_module.addCSourceFile(.{ .file = b.path("testing/tests.c"), .flags = c_flags_dynamic });
    tests.linkLibC();
    if (enable_sanitizers) {
        linkSanitizers(b, tests);
    }

    const run_tests_cmd = b.addRunArtifact(tests);
    if (b.args) |args| {
        run_tests_cmd.addArgs(args);
    }

    const test_step = b.step("test", "Run the unit tests");
    test_step.dependOn(&run_tests_cmd.step);
}

fn createCFlags(
    b: *std.Build,
    dynamic_config: bool,
    enable_sanitizers: bool,
) []const []const u8 {
    var flags = std.array_list.Managed([]const u8).init(b.allocator);
    flags.appendSlice(&.{
        "-std=c23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    }) catch @panic("OOM");

    if (dynamic_config) {
        flags.append("-DULOG_BUILD_DYNAMIC_CONFIG=1") catch @panic("OOM");
    }

    if (enable_sanitizers) {
        flags.appendSlice(&.{
            "-O0",
            "-g",
            "-fno-omit-frame-pointer",
            "-fsanitize=address,undefined,leak",
        }) catch @panic("OOM");
    }

    return flags.toOwnedSlice() catch @panic("OOM");
}

fn linkSanitizers(b: *std.Build, artifact: *std.Build.Step.Compile) void {
    addSanitizerLibrary(b, artifact, "asan");
    addSanitizerLibrary(b, artifact, "ubsan");
    addSanitizerLibrary(b, artifact, "lsan");
}

fn addSanitizerLibrary(
    b: *std.Build,
    artifact: *std.Build.Step.Compile,
    lib_name: []const u8,
) void {
    const runtime_dir = findCompilerRuntimeDir(b, lib_name) orelse
        std.debug.panic("unable to locate sanitizer runtime for {s}", .{lib_name});

    artifact.root_module.addLibraryPath(.{ .cwd_relative = runtime_dir });
    artifact.root_module.addRPath(.{ .cwd_relative = runtime_dir });
    artifact.root_module.linkSystemLibrary(lib_name, .{ .use_pkg_config = .no });
}

fn findCompilerRuntimeDir(b: *std.Build, lib_name: []const u8) ?[]const u8 {
    const cc = b.graph.env_map.get("CC") orelse "cc";
    const candidates = [_][]const u8{
        b.fmt("lib{s}.so", .{lib_name}),
        b.fmt("lib{s}.a", .{lib_name}),
    };

    var code: u8 = 0;
    for (candidates) |file_name| {
        const output = b.runAllowFail(
            &.{ cc, b.fmt("-print-file-name={s}", .{file_name}) },
            &code,
            .Ignore,
        ) catch continue;
        if (code != 0) {
            continue;
        }

        const resolved = std.mem.trim(u8, output, " \r\n\t");
        if (resolved.len == 0 or std.mem.eql(u8, resolved, file_name)) {
            continue;
        }

        if (std.fs.path.dirname(resolved)) |dir_name| {
            return dir_name;
        }
    }

    return null;
}
