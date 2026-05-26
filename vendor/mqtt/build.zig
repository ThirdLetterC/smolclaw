// Used the following as cross-compilation build.zig example:
// https://git.sr.ht/~jamii/focus/tree/master/build.zig

const std = @import("std");

const common_cflags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-D_POSIX_C_SOURCE=200809L",
};

const debug_cflags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-D_POSIX_C_SOURCE=200809L",
    "-fsanitize=address",
    "-fsanitize=undefined",
    "-fsanitize=leak",
};

fn cflagsFor(
    optimize: std.builtin.OptimizeMode,
    target: std.Build.ResolvedTarget,
    sanitize: bool,
) []const []const u8 {
    const use_debug = optimize == .Debug and target.result.os.tag != .windows;
    if (use_debug and sanitize) return &debug_cflags;
    return &common_cflags;
}

pub fn build(b: *std.Build) !void {
    const optimize = b.standardOptimizeOption(.{});
    const target = b.standardTargetOptions(.{});
    const sanitize = b.option(
        bool,
        "sanitize",
        "Enable ASan/UBSan/leak sanitizers for Debug builds (default: off)",
    ) orelse false;

    const tests = try addExecutable(b, target, optimize, sanitize, .{
        .name = "tests",
        .source = "testing/tests.c",
        .extra_cflags = &.{},
        .link_pthread = false,
        .link_openssl = false,
    });
    b.installArtifact(tests);

    const example_specs = [_]ExecutableSpec{
        .{
            .name = "simple_publisher",
            .source = "examples/simple_publisher.c",
            .extra_cflags = &.{},
            .link_pthread = true,
            .link_openssl = false,
        },
        .{
            .name = "simple_subscriber",
            .source = "examples/simple_subscriber.c",
            .extra_cflags = &.{},
            .link_pthread = true,
            .link_openssl = false,
        },
        .{
            .name = "reconnect_subscriber",
            .source = "examples/reconnect_subscriber.c",
            .extra_cflags = &.{},
            .link_pthread = true,
            .link_openssl = false,
        },
        .{
            .name = "openssl_publisher",
            .source = "examples/openssl_publisher.c",
            .extra_cflags = &.{"-DMQTT_USE_BIO"},
            .link_pthread = true,
            .link_openssl = true,
        },
    };

    const examples_step = b.step("examples", "Build example executables");
    for (example_specs) |spec| {
        const exe = try addExecutable(b, target, optimize, sanitize, spec);
        b.installArtifact(exe);
    examples_step.dependOn(&exe.step);
    }

    const tests_step = b.step("tests", "Build tests executable");
    tests_step.dependOn(&tests.step);

    const all_step = b.step("all", "Build tests and examples");
    all_step.dependOn(&tests.step);
    all_step.dependOn(examples_step);
    b.default_step.dependOn(all_step);

    const test_step = b.step("test", "Build and run tests");
    const run_tests = b.addRunArtifact(tests);
    test_step.dependOn(&run_tests.step);
}

const ExecutableSpec = struct {
    name: []const u8,
    source: []const u8,
    extra_cflags: []const []const u8,
    link_pthread: bool,
    link_openssl: bool,
};

fn addExecutable(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    sanitize: bool,
    spec: ExecutableSpec,
) !*std.Build.Step.Compile {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    module.addIncludePath(b.path("include"));

    const resolved_target = module.resolved_target orelse @panic("target missing");
    const base_cflags = cflagsFor(optimize, resolved_target, sanitize);
    const cflags = joinFlags(b, base_cflags, spec.extra_cflags);

    module.addCSourceFiles(.{
        .files = &.{ "src/mqtt.c", "src/mqtt_pal.c", spec.source },
        .flags = cflags,
    });

    const exe = b.addExecutable(.{
        .name = spec.name,
        .root_module = module,
    });

    if (shouldUseSanitizers(optimize, resolved_target, sanitize)) {
        if (resolved_target.result.os.tag == .linux) {
            const linked = linkLinuxSanitizerLibraries(b, exe);
            if (!linked) {
                exe.bundle_compiler_rt = true;
                exe.bundle_ubsan_rt = true;
            }
        } else {
            exe.root_module.linkSystemLibrary("asan", .{ .use_pkg_config = .no });
            exe.root_module.linkSystemLibrary("ubsan", .{ .use_pkg_config = .no });
        }
    }

    if (resolved_target.result.os.tag == .windows) {
        exe.linkSystemLibrary("ws2_32");
    } else if (spec.link_pthread) {
        exe.linkSystemLibrary("pthread");
    }

    if (spec.link_openssl) {
        exe.linkSystemLibrary("ssl");
        exe.linkSystemLibrary("crypto");
        if (resolved_target.result.os.tag == .linux) {
            exe.linkSystemLibrary("dl");
        }
    }

    return exe;
}

const SanitizerLib = struct {
    dir: []const u8,
    file: []const u8,
};

fn linkLinuxSanitizerLibraries(
    b: *std.Build,
    exe: *std.Build.Step.Compile,
) bool {
    const paths = [_][]const u8{
        "/usr/local/lib",
        "/usr/lib/x86_64-linux-gnu",
        "/lib64",
        "/lib",
        "/usr/lib64",
        "/usr/lib",
        "/lib/x86_64-linux-gnu",
    };

    const asan = findSanitizerLib(b, &paths, "asan") orelse return false;
    const ubsan = findSanitizerLib(b, &paths, "ubsan") orelse return false;
    const lsan = findSanitizerLib(b, &paths, "lsan") orelse return false;

    for (paths) |path| {
        exe.root_module.addLibraryPath(.{ .cwd_relative = path });
    }

    linkSanitizerLibrary(b, exe, "asan", asan);
    linkSanitizerLibrary(b, exe, "ubsan", ubsan);
    linkSanitizerLibrary(b, exe, "lsan", lsan);
    return true;
}

fn findSanitizerLib(
    b: *std.Build,
    paths: []const []const u8,
    base: []const u8,
) ?SanitizerLib {
    const so_name = b.fmt("lib{s}.so", .{base});
    const a_name = b.fmt("lib{s}.a", .{base});

    for (paths) |dir_path| {
        if (fileExists(b, dir_path, so_name)) {
            return .{ .dir = dir_path, .file = b.dupe(so_name) };
        }
        if (fileExists(b, dir_path, a_name)) {
            return .{ .dir = dir_path, .file = b.dupe(a_name) };
        }
        if (findVersionedSo(b, dir_path, so_name)) |versioned| {
            return .{ .dir = dir_path, .file = versioned };
        }
    }
    return null;
}

fn findVersionedSo(
    b: *std.Build,
    dir_path: []const u8,
    prefix: []const u8,
) ?[]const u8 {
    var dir = std.fs.openDirAbsolute(dir_path, .{ .iterate = true }) catch return null;
    defer dir.close();

    var it = dir.iterate();
    while (it.next() catch null) |entry| {
        if (entry.kind != .file and entry.kind != .sym_link) continue;
        if (std.mem.startsWith(u8, entry.name, prefix) and entry.name.len > prefix.len) {
            return b.dupe(entry.name);
        }
    }
    return null;
}

fn fileExists(
    b: *std.Build,
    dir_path: []const u8,
    file_name: []const u8,
) bool {
    const full_path = b.fmt("{s}/{s}", .{ dir_path, file_name });
    std.fs.accessAbsolute(full_path, .{}) catch return false;
    return true;
}

fn linkSanitizerLibrary(
    b: *std.Build,
    exe: *std.Build.Step.Compile,
    base: []const u8,
    lib: SanitizerLib,
) void {
    const so_name = b.fmt("lib{s}.so", .{base});
    const a_name = b.fmt("lib{s}.a", .{base});
    if (std.mem.eql(u8, lib.file, so_name) or std.mem.eql(u8, lib.file, a_name)) {
        exe.root_module.linkSystemLibrary(base, .{ .use_pkg_config = .no });
        return;
    }

    const full_path = b.fmt("{s}/{s}", .{ lib.dir, lib.file });
    exe.addObjectFile(.{ .cwd_relative = full_path });
}

fn shouldUseSanitizers(
    optimize: std.builtin.OptimizeMode,
    target: std.Build.ResolvedTarget,
    sanitize: bool,
) bool {
    return sanitize and optimize == .Debug and target.result.os.tag != .windows;
}

fn joinFlags(
    b: *std.Build,
    base: []const []const u8,
    extra: []const []const u8,
) []const []const u8 {
    if (extra.len == 0) return base;
    const joined = b.allocator.alloc([]const u8, base.len + extra.len) catch @panic("oom");
    std.mem.copyForwards([]const u8, joined[0..base.len], base);
    std.mem.copyForwards([]const u8, joined[base.len..], extra);
    return joined;
}
