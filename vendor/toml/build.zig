const std = @import("std");

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

fn addTomlExecutable(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    c_flags: []const []const u8,
    name: []const u8,
    source: []const u8,
) *std.Build.Step.Compile {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    const exe = b.addExecutable(.{
        .name = name,
        .root_module = module,
    });

    module.addIncludePath(b.path("src"));
    module.addIncludePath(b.path("include"));
    module.addCSourceFiles(.{
        .files = &.{ source, "src/toml.c" },
        .flags = c_flags,
    });

    if (optimize == .Debug) {
        exe.bundle_compiler_rt = true;
        exe.bundle_ubsan_rt = true;
        linkSanitizerLib(exe, b, "asan");
        linkSanitizerLib(exe, b, "ubsan");
        linkSanitizerLib(exe, b, "lsan");
    }

    b.installArtifact(exe);
    return exe;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    var c_flags = std.ArrayList([]const u8).empty;
    c_flags.appendSlice(b.allocator, &.{
        "-std=c23",
        "-D_XOPEN_SOURCE=600",
        "-D_POSIX_C_SOURCE=200112L",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-Wstrict-prototypes",
        "-Wwrite-strings",
        "-Wno-missing-field-initializers",
        "-fPIC",
    }) catch @panic("Out of memory");
    if (optimize == .Debug) {
        c_flags.appendSlice(b.allocator, &.{
            "-fsanitize=address,undefined,leak",
            "-fno-omit-frame-pointer",
        }) catch @panic("Out of memory");
    }
    const simple = addTomlExecutable(
        b,
        target,
        optimize,
        c_flags.items,
        "simple",
        "examples/simple.c",
    );
    const security = addTomlExecutable(
        b,
        target,
        optimize,
        c_flags.items,
        "security_regression",
        "testing/security_regression.c",
    );
    const tests = addTomlExecutable(
        b,
        target,
        optimize,
        c_flags.items,
        "tests",
        "testing/tests.c",
    );

    const run_cmd = b.addRunArtifact(simple);
    run_cmd.setCwd(b.path("examples"));
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the example");
    run_step.dependOn(&run_cmd.step);

    const security_cmd = b.addRunArtifact(security);
    const security_step = b.step("security", "Run the security regression executable");
    security_step.dependOn(&security_cmd.step);

    const tests_cmd = b.addRunArtifact(tests);
    const tests_step = b.step("test", "Run the unit test executable");
    tests_step.dependOn(&tests_cmd.step);
}
