const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sanitize = b.option(bool, "sanitize", "Enable ASAN/UBSAN/LSAN in debug builds") orelse true;
    const sanitize_c = if (sanitize and optimize == .Debug) std.zig.SanitizeC.full else std.zig.SanitizeC.off;
    const coverage_cc = b.option([]const u8, "coverage-cc", "C compiler used by coverage step") orelse "clang";
    const coverage_profdata =
        b.option([]const u8, "coverage-profdata", "Path to llvm-profdata executable used by coverage step") orelse
        "llvm-profdata-21";
    const coverage_cov =
        b.option([]const u8, "coverage-cov", "Path to llvm-cov executable used by coverage step") orelse
        "llvm-cov-21";

    const c_flags = &[_][]const u8{
        "-std=c23",
        "-D_POSIX_C_SOURCE=200809L",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-fstack-protector-strong",
        "-U_FORTIFY_SOURCE",
        "-D_FORTIFY_SOURCE=3",
        "-fPIE",
    };

    const lib_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .sanitize_c = sanitize_c,
    });
    lib_module.addIncludePath(b.path("include"));
    lib_module.addCSourceFile(.{ .file = b.path("src/websocket_client.c"), .flags = c_flags });

    const lib = b.addLibrary(.{
        .name = "websocket_client",
        .linkage = .static,
        .root_module = lib_module,
    });
    b.installArtifact(lib);

    const example_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .sanitize_c = sanitize_c,
    });
    example_module.addIncludePath(b.path("include"));
    example_module.addCSourceFile(.{ .file = b.path("examples/simple.c"), .flags = c_flags });

    const example = b.addExecutable(.{
        .name = "ws_simple",
        .root_module = example_module,
    });
    example.linkLibrary(lib);
    example.linkSystemLibrary("wolfssl");
    if (target.result.os.tag == .linux) {
        example.pie = true;
        example.link_z_relro = true;
        example.link_z_lazy = false;
    }

    b.installArtifact(example);

    const run_example = b.addRunArtifact(example);
    if (b.args) |args| {
        run_example.addArgs(args);
    }

    const run_step = b.step("run-example", "Run websocket example client");
    run_step.dependOn(&run_example.step);

    const tests_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .sanitize_c = sanitize_c,
    });
    tests_module.addIncludePath(b.path("include"));
    tests_module.addCSourceFile(.{ .file = b.path("testing/tests.c"), .flags = c_flags });

    const tests = b.addExecutable(.{
        .name = "ws_tests",
        .root_module = tests_module,
    });
    tests.linkLibrary(lib);
    tests.linkSystemLibrary("wolfssl");
    if (target.result.os.tag == .linux) {
        tests.pie = true;
        tests.link_z_relro = true;
        tests.link_z_lazy = false;
    }

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_tests.step);

    const coverage_prepare = b.addSystemCommand(&.{ "mkdir", "-p", "coverage", "coverage/html" });
    const coverage_clean = b.addSystemCommand(&.{
        "rm",
        "-f",
        "coverage/ws_tests_cov",
        "coverage/ws_tests.profraw",
        "coverage/ws_tests.profdata",
    });
    coverage_clean.step.dependOn(&coverage_prepare.step);

    const coverage_compile = b.addSystemCommand(&.{
        coverage_cc,
        "-std=c23",
        "-D_POSIX_C_SOURCE=200809L",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-fstack-protector-strong",
        "-U_FORTIFY_SOURCE",
        "-D_FORTIFY_SOURCE=3",
        "-fPIE",
        "-fprofile-instr-generate",
        "-fcoverage-mapping",
        "-Iinclude",
        "src/websocket_client.c",
        "testing/tests.c",
        "-lwolfssl",
        "-o",
        "coverage/ws_tests_cov",
    });
    coverage_compile.step.dependOn(&coverage_clean.step);

    const coverage_run = b.addSystemCommand(&.{ "./coverage/ws_tests_cov" });
    coverage_run.setEnvironmentVariable("LLVM_PROFILE_FILE", "coverage/ws_tests.profraw");
    coverage_run.step.dependOn(&coverage_compile.step);

    const coverage_merge = b.addSystemCommand(&.{
        coverage_profdata,
        "merge",
        "-sparse",
        "coverage/ws_tests.profraw",
        "-o",
        "coverage/ws_tests.profdata",
    });
    coverage_merge.step.dependOn(&coverage_run.step);

    const coverage_report = b.addSystemCommand(&.{
        coverage_cov,
        "report",
        "coverage/ws_tests_cov",
        "-instr-profile=coverage/ws_tests.profdata",
        "src/websocket_client.c",
        "testing/tests.c",
    });
    coverage_report.step.dependOn(&coverage_merge.step);

    const coverage_html = b.addSystemCommand(&.{
        coverage_cov,
        "show",
        "coverage/ws_tests_cov",
        "-instr-profile=coverage/ws_tests.profdata",
        "src/websocket_client.c",
        "-format=html",
        "-output-dir=coverage/html",
    });
    coverage_html.step.dependOn(&coverage_merge.step);

    const coverage_step = b.step("coverage", "Run tests with source coverage report");
    coverage_step.dependOn(&coverage_report.step);
    coverage_step.dependOn(&coverage_html.step);
}
