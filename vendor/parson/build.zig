const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sanitize: std.zig.SanitizeC = b.option(std.zig.SanitizeC, "sanitize", "Set C sanitizer mode: off, trap, or full") orelse if (optimize == .Debug)
        std.zig.SanitizeC.full
    else
        .off;

    const c_warnings = &.{
        "-std=c23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    };

    const parson_module = b.addModule("parson", .{
        .root_source_file = null,
        .target = target,
        .optimize = optimize,
        .sanitize_c = sanitize,
        .link_libc = true,
    });
    parson_module.addCSourceFiles(.{
        .files = &.{"src/parson.c"},
        .flags = c_warnings,
    });
    parson_module.addIncludePath(b.path("include"));
    parson_module.addIncludePath(b.path("include/parson"));

    const parson = b.addLibrary(.{
        .name = "parson",
        .root_module = parson_module,
    });
    parson.installHeader(b.path("include/parson/parson.h"), "parson/parson.h");
    b.installArtifact(parson);

    const test_flags = &.{
        "-std=c23",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-DTESTS_MAIN",
    };

    const tests_module = b.createModule(.{
        .root_source_file = null,
        .target = target,
        .optimize = optimize,
        .sanitize_c = sanitize,
        .link_libc = true,
    });
    tests_module.addCSourceFiles(.{
        .files = &.{"examples/tests.c", "src/parson.c"},
        .flags = test_flags,
    });
    tests_module.addIncludePath(b.path("include"));
    tests_module.addIncludePath(b.path("include/parson"));

    const tests = b.addExecutable(.{
        .name = "parson-tests",
        .root_module = tests_module,
    });

    const run_tests = b.addRunArtifact(tests);
    run_tests.setCwd(b.path("."));
    if (b.args) |args| {
        run_tests.addArgs(args);
    }

    const test_step = b.step("test", "Run parson tests");
    test_step.dependOn(&run_tests.step);

    const collision_module = b.createModule(.{
        .root_source_file = null,
        .target = target,
        .optimize = optimize,
        .sanitize_c = sanitize,
        .link_libc = true,
    });
    collision_module.addCSourceFiles(.{
        .files = &.{"examples/tests.c", "src/parson.c"},
        .flags = &.{
            "-std=c23",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-DTESTS_MAIN",
            "-DPARSON_FORCE_HASH_COLLISIONS",
        },
    });
    collision_module.addIncludePath(b.path("include"));
    collision_module.addIncludePath(b.path("include/parson"));

    const collision_tests = b.addExecutable(.{
        .name = "parson-tests-collisions",
        .root_module = collision_module,
    });

    const run_collision_tests = b.addRunArtifact(collision_tests);
    run_collision_tests.setCwd(b.path("."));
    if (b.args) |args| {
        run_collision_tests.addArgs(args);
    }

    const collision_step = b.step("test-collisions", "Run tests with forced hash collisions");
    collision_step.dependOn(&run_collision_tests.step);

    const security_module = b.createModule(.{
        .root_source_file = null,
        .target = target,
        .optimize = optimize,
        .sanitize_c = sanitize,
        .link_libc = true,
    });
    security_module.addCSourceFiles(.{
        .files = &.{"examples/security_tests.c", "src/parson.c"},
        .flags = &.{
            "-std=c23",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-DTESTS_MAIN",
        },
    });
    security_module.addIncludePath(b.path("include"));
    security_module.addIncludePath(b.path("include/parson"));

    const security_tests = b.addExecutable(.{
        .name = "parson-security-tests",
        .root_module = security_module,
    });

    const run_security_tests = b.addRunArtifact(security_tests);
    run_security_tests.setCwd(b.path("."));
    if (b.args) |args| {
        run_security_tests.addArgs(args);
    }

    const security_step = b.step("test-security", "Run security regression tests");
    security_step.dependOn(&run_security_tests.step);
}
