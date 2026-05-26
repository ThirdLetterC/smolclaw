const std = @import("std");

const base_sources = [_][]const u8{
    "src/alloc.c",
    "src/async.c",
    "src/dict.c",
    "src/read.c",
    "src/sds.c",
};

const fmacros_sources = [_][]const u8{
    "src/hiredis.c",
    "src/net.c",
};

const common_cflags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wstrict-prototypes",
    "-Wwrite-strings",
    "-Wno-missing-field-initializers",
};

const common_cflags_pic = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wstrict-prototypes",
    "-Wwrite-strings",
    "-Wno-missing-field-initializers",
    "-fPIC",
};

const posix_feature_cflags = [_][]const u8{
    "-D_XOPEN_SOURCE=600",
    "-D_POSIX_C_SOURCE=200112L",
};

const darwin_feature_cflags = [_][]const u8{
    "-D_DARWIN_C_SOURCE",
};

const sanitizer_cflags = [_][]const u8{
    "-fsanitize=address,undefined,leak",
    "-fno-omit-frame-pointer",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const enable_ssl = b.option(bool, "ssl", "Enable wolfSSL support") orelse false;
    const enable_examples = b.option(bool, "examples", "Build example programs") orelse false;
    const enable_libuv = b.option(bool, "libuv", "Build the libuv example (requires libuv)") orelse false;
    const build_shared = b.option(bool, "shared", "Build shared library") orelse true;
    const build_static = b.option(bool, "static", "Build static library") orelse true;

    const os_tag = target.result.os.tag;
    const enable_sanitizers =
        b.option(bool, "sanitize", "Enable ASan/UBSan/LSan hardening") orelse (optimize == .Debug);
    const use_sanitizers = enable_sanitizers and os_tag != .windows;

    var base_cflags_list = std.ArrayList([]const u8).empty;
    base_cflags_list.appendSlice(b.allocator, if (os_tag == .windows) &common_cflags else &common_cflags_pic) catch @panic("OOM");
    if (use_sanitizers) {
        base_cflags_list.appendSlice(b.allocator, &sanitizer_cflags) catch @panic("OOM");
    }
    const base_cflags = base_cflags_list.items;

    var fmacros_cflags_list = std.ArrayList([]const u8).empty;
    fmacros_cflags_list.appendSlice(b.allocator, base_cflags) catch @panic("OOM");
    if (os_tag != .aix) {
        fmacros_cflags_list.appendSlice(b.allocator, &posix_feature_cflags) catch @panic("OOM");
    }
    if (os_tag.isDarwin()) {
        fmacros_cflags_list.appendSlice(b.allocator, &darwin_feature_cflags) catch @panic("OOM");
    }
    const fmacros_cflags = fmacros_cflags_list.items;

    var test_base_cflags_list = std.ArrayList([]const u8).empty;
    test_base_cflags_list.appendSlice(b.allocator, if (os_tag == .windows) &common_cflags else &common_cflags_pic) catch @panic("OOM");
    const test_base_cflags = test_base_cflags_list.items;

    var test_fmacros_cflags_list = std.ArrayList([]const u8).empty;
    test_fmacros_cflags_list.appendSlice(b.allocator, test_base_cflags) catch @panic("OOM");
    if (os_tag != .aix) {
        test_fmacros_cflags_list.appendSlice(b.allocator, &posix_feature_cflags) catch @panic("OOM");
    }
    if (os_tag.isDarwin()) {
        test_fmacros_cflags_list.appendSlice(b.allocator, &darwin_feature_cflags) catch @panic("OOM");
    }
    const test_fmacros_cflags = test_fmacros_cflags_list.items;

    var static_lib: ?*std.Build.Step.Compile = null;
    var shared_lib: ?*std.Build.Step.Compile = null;

    if (!build_static and !build_shared) {
        std.debug.panic("At least one of -Dstatic or -Dshared must be enabled.", .{});
    }

    if (build_static) {
        const lib = addHiredisLibrary(b, "hiredis", target, optimize, false, enable_ssl, base_cflags, fmacros_cflags);
        b.installArtifact(lib);
        static_lib = lib;
    }

    if (build_shared) {
        const lib = addHiredisLibrary(b, "hiredis", target, optimize, true, enable_ssl, base_cflags, fmacros_cflags);
        b.installArtifact(lib);
        shared_lib = lib;
    }

    b.installDirectory(.{
        .source_dir = b.path("include"),
        .install_dir = .header,
        .install_subdir = "",
    });
    b.installDirectory(.{
        .source_dir = b.path("adapters"),
        .install_dir = .header,
        .install_subdir = "adapters",
    });

    const clean_step = b.step("clean", "Remove build artifacts");
    const remove_cache = b.addRemoveDirTree(b.path(".zig-cache"));
    const remove_out = b.addRemoveDirTree(b.path("zig-out"));
    clean_step.dependOn(&remove_cache.step);
    clean_step.dependOn(&remove_out.step);

    const link_lib = static_lib orelse shared_lib.?;
    const tests_step = b.step("test", "Build and run unit tests");
    {
        const test_lib = addHiredisLibrary(
            b,
            "hiredis-test",
            target,
            optimize,
            false,
            false,
            test_base_cflags,
            test_fmacros_cflags,
        );
        const exe = addExample(
            b,
            "hiredis-unit-tests",
            "testing/test.c",
            target,
            optimize,
            test_lib,
            test_base_cflags,
            false,
            false,
            false,
        );
        const run_tests = b.addRunArtifact(exe);
        tests_step.dependOn(&run_tests.step);
    }

    const examples_step = b.step("examples", "Build example programs");

    {
        const exe = addExample(b, "example", "examples/example.c", target, optimize, link_lib, base_cflags, false, false, false);
        const install_exe = b.addInstallArtifact(exe, .{});
        examples_step.dependOn(&install_exe.step);
        if (enable_examples) {
            b.getInstallStep().dependOn(&install_exe.step);
        }
    }
    {
        const exe = addExample(b, "example-push", "examples/example-push.c", target, optimize, link_lib, base_cflags, false, false, false);
        const install_exe = b.addInstallArtifact(exe, .{});
        examples_step.dependOn(&install_exe.step);
        if (enable_examples) {
            b.getInstallStep().dependOn(&install_exe.step);
        }
    }
    {
        const exe = addExample(b, "example-poll", "examples/example-poll.c", target, optimize, link_lib, base_cflags, false, false, false);
        const install_exe = b.addInstallArtifact(exe, .{});
        examples_step.dependOn(&install_exe.step);
        if (enable_examples) {
            b.getInstallStep().dependOn(&install_exe.step);
        }
    }
    {
        const exe = addExample(
            b,
            "example-streams-threads",
            "examples/example-streams-threads.c",
            target,
            optimize,
            link_lib,
            base_cflags,
            false,
            false,
            true,
        );
        const install_exe = b.addInstallArtifact(exe, .{});
        examples_step.dependOn(&install_exe.step);
        if (enable_examples) {
            b.getInstallStep().dependOn(&install_exe.step);
        }
    }

    if (enable_ssl) {
        const exe = addExample(b, "example-ssl", "examples/example-ssl.c", target, optimize, link_lib, base_cflags, true, false, false);
        const install_exe = b.addInstallArtifact(exe, .{});
        examples_step.dependOn(&install_exe.step);
        if (enable_examples) {
            b.getInstallStep().dependOn(&install_exe.step);
        }
    }

    if (enable_libuv) {
        const exe = addExample(b, "example-libuv", "examples/example-libuv.c", target, optimize, link_lib, base_cflags, false, true, false);
        const install_exe = b.addInstallArtifact(exe, .{});
        examples_step.dependOn(&install_exe.step);
        if (enable_examples) {
            b.getInstallStep().dependOn(&install_exe.step);
        }
    }
}

fn addHiredisLibrary(
    b: *std.Build,
    name: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    shared: bool,
    enable_ssl: bool,
    base_cflags: []const []const u8,
    fmacros_cflags: []const []const u8,
) *std.Build.Step.Compile {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    module.addIncludePath(b.path("."));
    module.addIncludePath(b.path("include"));
    module.addCSourceFiles(.{ .files = &base_sources, .flags = base_cflags });
    module.addCSourceFiles(.{ .files = &fmacros_sources, .flags = fmacros_cflags });

    if (enable_ssl) {
        module.addCSourceFiles(.{ .files = &[_][]const u8{"src/ssl.c"}, .flags = base_cflags });
    }

    const lib = b.addLibrary(.{
        .name = name,
        .root_module = module,
        .linkage = if (shared) .dynamic else .static,
    });

    if (enable_ssl and shared) {
        lib.linkSystemLibrary("wolfssl");
        lib.linkSystemLibrary("pthread");
    }

    return lib;
}

fn addExample(
    b: *std.Build,
    name: []const u8,
    source: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    lib: *std.Build.Step.Compile,
    cflags: []const []const u8,
    needs_ssl: bool,
    needs_libuv: bool,
    needs_pthread: bool,
) *std.Build.Step.Compile {
    const module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    module.addIncludePath(b.path("."));
    module.addIncludePath(b.path("include"));
    module.addCSourceFiles(.{ .files = &[_][]const u8{source}, .flags = cflags });

    const exe = b.addExecutable(.{ .name = name, .root_module = module });
    exe.linkLibrary(lib);

    if (needs_ssl) {
        exe.linkSystemLibrary("wolfssl");
    }

    if (needs_ssl or needs_pthread) {
        exe.linkSystemLibrary("pthread");
    }

    if (needs_libuv) {
        exe.linkSystemLibrary("uv");
    }

    return exe;
}
