const std = @import("std");

const warning_flags: []const []const u8 = &.{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-fstack-protector-strong",
    "-D_FORTIFY_SOURCE=3",
    "-D_POSIX_C_SOURCE=200809L",
    "-fPIE",
};

const debug_warning_flags: []const []const u8 = &.{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-fstack-protector-strong",
    "-D_FORTIFY_SOURCE=3",
    "-D_POSIX_C_SOURCE=200809L",
    "-fPIE",
    "-fsanitize=address,undefined,leak",
    "-fno-omit-frame-pointer",
};

const SanitizerRuntime = struct {
    soname: []const u8,
    fallback_name: []const u8,
};

const sanitizer_runtimes = [_]SanitizerRuntime{
    .{ .soname = "libasan.so", .fallback_name = "asan" },
    .{ .soname = "libubsan.so", .fallback_name = "ubsan" },
    .{ .soname = "liblsan.so", .fallback_name = "lsan" },
};

const Example = struct {
    name: []const u8,
    source: []const u8,
};

const examples = [_]Example{
    .{ .name = "simple", .source = "examples/simple.c" },
};

const CExecutableOptions = struct {
    name: []const u8,
    source: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    link_math: bool = false,
};

fn gnuMultiarchSubdir(target: std.Target) ?[]const u8 {
    if (target.os.tag != .linux) {
        return null;
    }

    return switch (target.cpu.arch) {
        .x86_64 => "x86_64-linux-gnu",
        .aarch64 => "aarch64-linux-gnu",
        .arm => "arm-linux-gnueabihf",
        .riscv64 => "riscv64-linux-gnu",
        else => null,
    };
}

fn parseSharedObjectMajor(name: []const u8, versioned_prefix: []const u8) ?u32 {
    if (!std.mem.startsWith(u8, name, versioned_prefix)) {
        return null;
    }

    const suffix = name[versioned_prefix.len..];
    if (suffix.len == 0) {
        return null;
    }

    const major_end = std.mem.indexOfScalar(u8, suffix, '.') orelse suffix.len;
    return std.fmt.parseUnsigned(u32, suffix[0..major_end], 10) catch null;
}

fn findSanitizerRuntimeInDir(
    b: *std.Build,
    dir_path: []const u8,
    runtime_name: []const u8,
) ?[]const u8 {
    var dir = std.fs.openDirAbsolute(dir_path, .{ .iterate = true }) catch return null;
    defer dir.close();

    const exact_runtime_path = std.fs.path.join(b.allocator, &.{ dir_path, runtime_name }) catch @panic("OOM");
    if (std.fs.openFileAbsolute(exact_runtime_path, .{})) |file| {
        file.close();
        return exact_runtime_path;
    } else |_| {}

    const versioned_prefix = b.fmt("{s}.", .{runtime_name});
    var chosen_runtime_name: ?[]const u8 = null;
    var highest_major: ?u32 = null;

    var it = dir.iterate();
    while (it.next() catch null) |entry| {
        if (entry.kind != .file and entry.kind != .sym_link) {
            continue;
        }

        const major = parseSharedObjectMajor(entry.name, versioned_prefix) orelse continue;
        if (highest_major) |current_major| {
            if (major < current_major) {
                continue;
            }
            if (major == current_major) {
                if (chosen_runtime_name) |current_name| {
                    if (std.mem.order(u8, entry.name, current_name) != .gt) {
                        continue;
                    }
                }
            }
        }

        highest_major = major;
        chosen_runtime_name = b.dupe(entry.name);
    }

    if (chosen_runtime_name) |name| {
        return std.fs.path.join(b.allocator, &.{ dir_path, name }) catch @panic("OOM");
    }

    return null;
}

fn findSanitizerRuntimePath(
    b: *std.Build,
    target: std.Target,
    runtime_name: []const u8,
) ?[]const u8 {
    if (gnuMultiarchSubdir(target)) |subdir| {
        const multiarch_bases = [_][]const u8{ "/usr/lib", "/lib" };
        for (multiarch_bases) |base| {
            const dir_path = std.fs.path.join(b.allocator, &.{ base, subdir }) catch @panic("OOM");
            if (findSanitizerRuntimeInDir(b, dir_path, runtime_name)) |runtime_path| {
                return runtime_path;
            }
        }
    }

    const generic_dirs = [_][]const u8{
        "/usr/lib",
        "/lib",
        "/usr/local/lib",
        "/usr/lib64",
        "/lib64",
    };
    for (generic_dirs) |dir_path| {
        if (findSanitizerRuntimeInDir(b, dir_path, runtime_name)) |runtime_path| {
            return runtime_path;
        }
    }

    return null;
}

fn addDebugSanitizers(module: *std.Build.Module, target: std.Target) void {
    const b = module.owner;

    for (sanitizer_runtimes) |runtime| {
        if (findSanitizerRuntimePath(b, target, runtime.soname)) |runtime_path| {
            module.addObjectFile(.{ .cwd_relative = runtime_path });
        } else {
            module.linkSystemLibrary(runtime.fallback_name, .{});
        }
    }
}

fn addCExecutable(b: *std.Build, options: CExecutableOptions) *std.Build.Step.Compile {
    const c_flags = if (options.optimize == .Debug) debug_warning_flags else warning_flags;

    const module = b.createModule(.{
        .target = options.target,
        .optimize = options.optimize,
        .link_libc = true,
    });
    module.addIncludePath(b.path("include"));
    module.addCSourceFile(.{
        .file = b.path(options.source),
        .flags = c_flags,
    });
    module.addCSourceFile(.{
        .file = b.path("src/nanocron.c"),
        .flags = c_flags,
    });

    const exe = b.addExecutable(.{
        .name = options.name,
        .root_module = module,
    });

    if (options.optimize == .Debug) {
        addDebugSanitizers(exe.root_module, options.target.result);
    }
    if (options.target.result.os.tag == .linux) {
        exe.pie = true;
        exe.link_z_relro = true;
        exe.link_z_lazy = false;
    }

    if (options.link_math) {
        exe.root_module.linkSystemLibrary("m", .{});
    }

    return exe;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const install_step = b.getInstallStep();

    const all = b.step("all", "Build all examples and the test binary");
    const examples_step = b.step("examples", "Build all examples");
    const tests_step = b.step("tests", "Build the test binary");
    const test_run_step = b.step("test", "Build and run the tests");

    b.default_step = all;
    all.dependOn(examples_step);
    all.dependOn(tests_step);
    all.dependOn(install_step);

    for (examples) |example| {
        const exe = addCExecutable(b, .{
            .name = example.name,
            .source = example.source,
            .target = target,
            .optimize = optimize,
        });

        b.installArtifact(exe);
        examples_step.dependOn(&exe.step);
    }

    const tests_exe = addCExecutable(b, .{
        .name = "tests",
        .source = "testing/tests.c",
        .target = target,
        .optimize = optimize,
        .link_math = true,
    });

    b.installArtifact(tests_exe);
    tests_step.dependOn(&tests_exe.step);

    const run_tests = b.addRunArtifact(tests_exe);
    test_run_step.dependOn(&run_tests.step);
}
