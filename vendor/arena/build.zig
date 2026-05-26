const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const use_mimalloc = b.option(bool, "mimalloc", "Use mimalloc for arena allocations") orelse false;

    const common_debug_flags: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE", "-Og", "-g" };
    const common_release_flags: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE" };
    const common_debug_flags_mimalloc: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE", "-Og", "-g", "-DARENA_USE_MIMALLOC" };
    const common_release_flags_mimalloc: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE", "-DARENA_USE_MIMALLOC" };
    const common_cflags: []const []const u8 =
        if (optimize == .Debug)
            if (use_mimalloc) common_debug_flags_mimalloc else common_debug_flags
        else if (use_mimalloc)
            common_release_flags_mimalloc
        else
            common_release_flags;

    const test_debug_flags: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wno-newline-eof", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE", "-Og", "-g" };
    const test_release_flags: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wno-newline-eof", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE" };
    const test_debug_flags_mimalloc: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wno-newline-eof", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE", "-Og", "-g", "-DARENA_USE_MIMALLOC" };
    const test_release_flags_mimalloc: []const []const u8 =
        &[_][]const u8{ "-std=c23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wno-newline-eof", "-fstack-protector-strong", "-D_FORTIFY_SOURCE=3", "-fPIE", "-DARENA_USE_MIMALLOC" };
    const test_cflags: []const []const u8 =
        if (optimize == .Debug)
            if (use_mimalloc) test_debug_flags_mimalloc else test_debug_flags
        else if (use_mimalloc)
            test_release_flags_mimalloc
        else
            test_release_flags;

    const tests_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .stack_protector = true,
        .pic = true,
    });
    if (optimize == .Debug) {
        tests_module.sanitize_c = .full;
    }
    const tests_exe = b.addExecutable(.{
        .name = "test",
        .root_module = tests_module,
    });
    tests_exe.pie = true;
    tests_exe.link_z_relro = true;
    tests_exe.link_z_lazy = false;
    tests_exe.addCSourceFile(.{ .file = b.path("testing/tests.c"), .flags = test_cflags });
    tests_exe.addCSourceFile(.{ .file = b.path("src/rktest.c"), .flags = test_cflags });
    tests_exe.addCSourceFile(.{ .file = b.path("src/arena.c"), .flags = test_cflags });
    tests_exe.addIncludePath(b.path("include"));
    tests_exe.linkLibC();
    tests_exe.linkSystemLibrary("m");
    if (use_mimalloc) {
        tests_exe.linkSystemLibrary("mimalloc");
    }
    b.installArtifact(tests_exe);

    const tests_step = b.step("tests", "Build the test executable");
    tests_step.dependOn(&tests_exe.step);

    const run_tests = b.addRunArtifact(tests_exe);
    if (b.args) |args| {
        run_tests.addArgs(args);
    }
    const test_step = b.step("test", "Run the test executable");
    test_step.dependOn(&run_tests.step);

    const examples_step = b.step("examples", "Build all examples");
    var examples_dir = b.build_root.handle.openDir("examples", .{ .iterate = true }) catch |err| {
        std.debug.panic("failed to open examples: {s}", .{@errorName(err)});
    };
    defer examples_dir.close();

    var iter = examples_dir.iterate();
    while (true) {
        const entry = iter.next() catch |err| {
            std.debug.panic("failed to iterate examples: {s}", .{@errorName(err)});
        } orelse break;

        if (entry.kind != .file) {
            continue;
        }
        if (!std.mem.endsWith(u8, entry.name, ".c")) {
            continue;
        }

        const stem = entry.name[0 .. entry.name.len - 2];
        const example_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .stack_protector = true,
            .pic = true,
        });
        if (optimize == .Debug) {
            example_module.sanitize_c = .full;
        }
        const exe = b.addExecutable(.{
            .name = stem,
            .root_module = example_module,
        });
        exe.pie = true;
        exe.link_z_relro = true;
        exe.link_z_lazy = false;
        const source_path = b.fmt("examples/{s}", .{entry.name});
        exe.addCSourceFile(.{ .file = b.path(source_path), .flags = common_cflags });
        exe.addIncludePath(b.path("include"));
        exe.linkLibC();
        if (use_mimalloc) {
            exe.linkSystemLibrary("mimalloc");
        }
        const install_example = b.addInstallArtifact(exe, .{});
        examples_step.dependOn(&install_example.step);
    }
}
