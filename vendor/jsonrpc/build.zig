const std = @import("std");

const common_flags = &[_][]const u8{
    "-march=x86-64",
    "-mtune=generic",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-std=c23",
    "-D_GNU_SOURCE",
    "-DARENA_DEFAULT_ALIGNMENT=alignof(max_align_t)",
    "-fstack-protector-strong",
};

const debug_flags = &[_][]const u8{
    "-march=x86-64",
    "-mtune=generic",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-std=c23",
    "-D_GNU_SOURCE",
    "-DARENA_DEFAULT_ALIGNMENT=alignof(max_align_t)",
    "-fstack-protector-strong",
    "-fsanitize=address",
    "-fsanitize=undefined",
    "-fsanitize=leak",
};

const hardened_release_flags = &[_][]const u8{
    "-march=x86-64",
    "-mtune=generic",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-std=c23",
    "-D_GNU_SOURCE",
    "-DARENA_DEFAULT_ALIGNMENT=alignof(max_align_t)",
    "-fstack-protector-strong",
    "-D_FORTIFY_SOURCE=3",
    "-fPIE",
};

fn selectedCFlags(use_sanitizers: bool, optimize: std.builtin.OptimizeMode) []const []const u8 {
    if (use_sanitizers) {
        return debug_flags;
    }
    if (optimize == .Debug) {
        return common_flags;
    }
    return hardened_release_flags;
}

fn applyReleaseHardening(exe: *std.Build.Step.Compile, optimize: std.builtin.OptimizeMode) void {
    if (optimize == .Debug) {
        return;
    }
    exe.pie = true;
    exe.link_z_relro = true;
    exe.link_z_lazy = false;
}

fn findSanitizerLib(b: *std.Build, name: []const u8) ?[]const u8 {
    const lib_dirs = &[_][]const u8{
        "/lib/x86_64-linux-gnu",
        "/usr/lib/x86_64-linux-gnu",
        "/lib64",
        "/usr/lib64",
        "/lib",
        "/usr/lib",
    };

    for (lib_dirs) |dir_path| {
        const unversioned = b.fmt("{s}/lib{s}.so", .{ dir_path, name });
        if (std.fs.accessAbsolute(unversioned, .{})) |_| {
            return unversioned;
        } else |_| {}

        var dir = std.fs.openDirAbsolute(dir_path, .{ .iterate = true }) catch continue;
        defer dir.close();

        const prefix = b.fmt("lib{s}.so.", .{name});
        var it = dir.iterate();
        while (it.next() catch break) |entry| {
            if (entry.kind != .file and entry.kind != .sym_link) {
                continue;
            }
            if (std.mem.startsWith(u8, entry.name, prefix)) {
                return b.fmt("{s}/{s}", .{ dir_path, entry.name });
            }
        }
    }

    return null;
}

fn linkSanitizerLib(exe: *std.Build.Step.Compile, b: *std.Build, name: []const u8) void {
    if (findSanitizerLib(b, name)) |path| {
        exe.addObjectFile(.{ .cwd_relative = path });
    } else {
        exe.linkSystemLibrary(name);
    }
}

fn addServerExecutable(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    enable_sanitizers: bool,
) *std.Build.Step.Compile {
    const use_sanitizers = enable_sanitizers and optimize == .Debug;
    const c_flags = selectedCFlags(use_sanitizers, optimize);

    const exe = b.addExecutable(.{
        .name = "jsonrpc_server",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    // Add C source files
    exe.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{
            "main.c",
            "server.c",
            "jsonrpc.c",
            "arena.c",
            "parson.c",
        },
        .flags = c_flags,
    });

    // Include the public headers
    exe.addIncludePath(b.path("include"));

    // Link against system libuv
    // This requires libuv headers and libraries to be in standard system paths.
    exe.linkSystemLibrary("uv");

    // Link against the C standard library
    exe.linkLibC();
    applyReleaseHardening(exe, optimize);

    if (optimize != .Debug) {
        exe.root_module.strip = true;
    }

    if (use_sanitizers) {
        exe.bundle_compiler_rt = true;
        exe.bundle_ubsan_rt = true;
        // The C sources are compiled with -fsanitize=..., so ensure the
        // corresponding runtime libraries are linked.
        linkSanitizerLib(exe, b, "asan");
        linkSanitizerLib(exe, b, "ubsan");
        linkSanitizerLib(exe, b, "lsan");
    }

    return exe;
}

fn addBenchExecutable(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    enable_sanitizers: bool,
) *std.Build.Step.Compile {
    const use_sanitizers = enable_sanitizers and optimize == .Debug;
    const c_flags = selectedCFlags(use_sanitizers, optimize);

    const exe = b.addExecutable(.{
        .name = "bench_rps",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    exe.addCSourceFile(.{
        .file = b.path("tools/bench_rps.c"),
        .flags = c_flags,
    });
    exe.addCSourceFile(.{
        .file = b.path("src/parson.c"),
        .flags = c_flags,
    });

    exe.addIncludePath(b.path("include"));
    exe.linkSystemLibrary("uv");
    exe.linkLibC();
    applyReleaseHardening(exe, optimize);

    if (optimize != .Debug) {
        exe.root_module.strip = true;
    }

    if (use_sanitizers) {
        exe.bundle_compiler_rt = true;
        exe.bundle_ubsan_rt = true;
        linkSanitizerLib(exe, b, "asan");
        linkSanitizerLib(exe, b, "ubsan");
        linkSanitizerLib(exe, b, "lsan");
    }

    return exe;
}

fn addTestsExecutable(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    enable_sanitizers: bool,
) *std.Build.Step.Compile {
    const use_sanitizers = enable_sanitizers and optimize == .Debug;
    const c_flags = selectedCFlags(use_sanitizers, optimize);

    const exe = b.addExecutable(.{
        .name = "jsonrpc_tests",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    exe.addCSourceFiles(.{
        .root = b.path("."),
        .files = &.{
            "testing/tests.c",
            "src/jsonrpc.c",
            "src/arena.c",
            "src/parson.c",
        },
        .flags = c_flags,
    });

    exe.addIncludePath(b.path("include"));
    exe.linkLibC();

    if (use_sanitizers) {
        exe.bundle_compiler_rt = true;
        exe.bundle_ubsan_rt = true;
        linkSanitizerLib(exe, b, "asan");
        linkSanitizerLib(exe, b, "ubsan");
        linkSanitizerLib(exe, b, "lsan");
    }

    return exe;
}

pub fn build(b: *std.Build) void {
    const force_valgrind = b.option(
        bool,
        "valgrind",
        "Force baseline CPU features for Valgrind compatibility.",
    ) orelse false;

    const enable_sanitizers = b.option(bool, "sanitizers", "Enable ASan/UBSan/LSan in Debug builds.") orelse false;
    const release_mode = b.option(
        std.builtin.OptimizeMode,
        "release_mode",
        "Optimize mode for the 'release' and 'run-release' steps (ReleaseSafe/ReleaseFast/ReleaseSmall).",
    ) orelse .ReleaseFast;

    // Standard target options (arch, os, abi). We rely on C flags below to pin
    // the ISA to a Valgrind-friendly baseline without forcing a cross target.
    const base_target_query = b.standardTargetOptionsQueryOnly(.{});
    const base_target = b.resolveTargetQuery(base_target_query);

    var valgrind_target_query = base_target_query;
    valgrind_target_query.cpu_model = .baseline;
    valgrind_target_query.cpu_features_add = .empty;
    valgrind_target_query.cpu_features_sub = .empty;
    const valgrind_target = b.resolveTargetQuery(valgrind_target_query);

    const target = if (force_valgrind) valgrind_target else base_target;

    // Standard optimization options (Debug, ReleaseSafe, ReleaseFast, ReleaseSmall)
    const optimize = b.standardOptimizeOption(.{});

    const exe = addServerExecutable(b, target, optimize, enable_sanitizers);
    const bench_exe = addBenchExecutable(b, target, optimize, enable_sanitizers);

    // If you add crypto for handshakes (e.g., OpenSSL), link it here:
    // exe.linkSystemLibrary("ssl");
    // exe.linkSystemLibrary("crypto");

    // Install the artifact (moves it to zig-out/bin)
    b.installArtifact(exe);
    b.installArtifact(bench_exe);

    const debug_exe = addServerExecutable(b, target, .Debug, enable_sanitizers);
    const debug_bench_exe = addBenchExecutable(b, target, .Debug, enable_sanitizers);
    const debug_install = b.addInstallArtifact(debug_exe, .{});
    const debug_bench_install = b.addInstallArtifact(debug_bench_exe, .{});
    const debug_step = b.step("debug", "Build Debug");
    debug_step.dependOn(&debug_install.step);
    debug_step.dependOn(&debug_bench_install.step);

    const release_exe = addServerExecutable(b, target, release_mode, enable_sanitizers);
    const release_bench_exe = addBenchExecutable(b, target, release_mode, enable_sanitizers);
    const release_install = b.addInstallArtifact(release_exe, .{});
    const release_bench_install = b.addInstallArtifact(release_bench_exe, .{});
    const release_step = b.step("release", "Build Release");
    release_step.dependOn(&release_install.step);
    release_step.dependOn(&release_bench_install.step);

    const run_release_cmd = b.addRunArtifact(release_exe);
    run_release_cmd.step.dependOn(&release_install.step);
    if (b.args) |args| {
        run_release_cmd.addArgs(args);
    }
    const run_release_step = b.step("run-release", "Run the JSON-RPC server (release)");
    run_release_step.dependOn(&run_release_cmd.step);

    // Create a 'run' step to execute the server directly via 'zig build run'
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the JSON-RPC server");
    run_step.dependOn(&run_cmd.step);

    const tests_exe = addTestsExecutable(b, target, optimize, enable_sanitizers);
    const test_cmd = b.addRunArtifact(tests_exe);
    const test_step = b.step("test", "Build and run unit tests");
    test_step.dependOn(&test_cmd.step);

    const valgrind_exe = addServerExecutable(b, valgrind_target, .Debug, false);
    const valgrind_cmd = b.addSystemCommand(&.{
        "valgrind",
        "--leak-check=full",
        "--show-leak-kinds=all",
    });
    valgrind_cmd.addArtifactArg(valgrind_exe);
    if (b.args) |args| {
        valgrind_cmd.addArgs(args);
    }
    const valgrind_step = b.step("valgrind", "Run the JSON-RPC server under Valgrind");
    valgrind_step.dependOn(&valgrind_cmd.step);
}
