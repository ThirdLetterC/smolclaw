const std = @import("std");

pub fn build(b: *std.Build) void {
    // Standard target options (arch, os, abi). We rely on C flags below to pin
    // the ISA to a Valgrind-friendly baseline without forcing a cross target.
    const target = b.standardTargetOptions(.{});

    // Standard optimization options (Debug, ReleaseSafe, ReleaseFast, ReleaseSmall)
    const optimize = b.standardOptimizeOption(.{});

    const exe = b.addExecutable(.{
        .name = "ws_server",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    const enable_sanitizers = b.option(bool, "sanitizers", "Enable ASan/UBSan/LSan in Debug builds.") orelse false;
    const use_sanitizers = enable_sanitizers and optimize == .Debug and target.result.os.tag != .windows;

    const common_flags = &[_][]const u8{
        "-march=x86-64",
        "-mtune=generic",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-std=c23",
        "-D_GNU_SOURCE",
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
        "-fsanitize=address",
        "-fsanitize=undefined",
        "-fsanitize=leak",
    };

    const c_flags = if (use_sanitizers) debug_flags else common_flags;

    const simple_exe = b.addExecutable(.{
        .name = "ws_simple",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });

    const unit_tests = b.addExecutable(.{
        .name = "ws_unit_tests",
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
            "websocket.c",
        },
        .flags = c_flags,
    });

    // Include the current directory for headers
    exe.addIncludePath(b.path("include"));

    simple_exe.addCSourceFiles(.{
        .files = &.{
            "examples/simple.c",
            "src/server.c",
            "src/websocket.c",
        },
        .flags = c_flags,
    });

    simple_exe.addIncludePath(b.path("include"));

    unit_tests.addCSourceFiles(.{
        .files = &.{
            "testing/tests.c",
            "src/websocket.c",
        },
        .flags = c_flags,
    });

    unit_tests.addIncludePath(b.path("include"));

    if (use_sanitizers) {
        exe.bundle_compiler_rt = true;
        exe.bundle_ubsan_rt = true;
        if (!linkSanitizers(b, exe, target)) {
            exe.root_module.linkSystemLibrary("asan", .{ .use_pkg_config = .no });
            exe.root_module.linkSystemLibrary("ubsan", .{ .use_pkg_config = .no });
            exe.root_module.linkSystemLibrary("lsan", .{ .use_pkg_config = .no });
        }

        simple_exe.bundle_compiler_rt = true;
        simple_exe.bundle_ubsan_rt = true;
        if (!linkSanitizers(b, simple_exe, target)) {
            simple_exe.root_module.linkSystemLibrary("asan", .{ .use_pkg_config = .no });
            simple_exe.root_module.linkSystemLibrary("ubsan", .{ .use_pkg_config = .no });
            simple_exe.root_module.linkSystemLibrary("lsan", .{ .use_pkg_config = .no });
        }

        unit_tests.bundle_compiler_rt = true;
        unit_tests.bundle_ubsan_rt = true;
        if (!linkSanitizers(b, unit_tests, target)) {
            unit_tests.root_module.linkSystemLibrary("asan", .{ .use_pkg_config = .no });
            unit_tests.root_module.linkSystemLibrary("ubsan", .{ .use_pkg_config = .no });
            unit_tests.root_module.linkSystemLibrary("lsan", .{ .use_pkg_config = .no });
        }
    }

    // Link against system libuv
    // This requires libuv headers and libraries to be in standard system paths.
    exe.linkSystemLibrary("uv");
    simple_exe.linkSystemLibrary("uv");

    // Link against the C standard library
    exe.linkLibC();
    simple_exe.linkLibC();
    unit_tests.linkLibC();

    // If you add crypto for handshakes (e.g., OpenSSL), link it here:
    // exe.linkSystemLibrary("ssl");
    // exe.linkSystemLibrary("crypto");

    // Install the artifact (moves it to zig-out/bin)
    b.installArtifact(exe);
    b.installArtifact(simple_exe);

    // Create a 'run' step to execute the server directly via 'zig build run'
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the WebSocket server");
    run_step.dependOn(&run_cmd.step);

    const run_simple_cmd = b.addRunArtifact(simple_exe);
    run_simple_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_simple_cmd.addArgs(args);
    }

    const run_simple_step = b.step("run-simple", "Run the simple demo server");
    run_simple_step.dependOn(&run_simple_cmd.step);

    const run_tests_cmd = b.addRunArtifact(unit_tests);
    const tests_step = b.step("test", "Run unit tests");
    tests_step.dependOn(&run_tests_cmd.step);
}

const SanitizerTriplet = struct {
    asan: []const u8,
    ubsan: []const u8,
    lsan: []const u8,
};

fn linkSanitizers(
    b: *std.Build,
    exe: *std.Build.Step.Compile,
    target: std.Build.ResolvedTarget,
) bool {
    if (target.result.os.tag != .linux) {
        return false;
    }

    const paths = buildSanitizerSearchPaths(b, target);
    if (paths.len == 0) {
        return false;
    }

    if (findGccSanitizers(b, paths)) |libs| {
        exe.addObjectFile(.{ .cwd_relative = libs.asan });
        exe.addObjectFile(.{ .cwd_relative = libs.ubsan });
        exe.addObjectFile(.{ .cwd_relative = libs.lsan });
        return true;
    }

    const clang_arch = clangRtArch(target) orelse return false;
    const asan = findClangRtLib(b, paths, "asan", clang_arch) orelse return false;
    const lsan = findClangRtLib(b, paths, "lsan", clang_arch) orelse return false;
    const ubsan = findClangRtLib(b, paths, "ubsan_standalone", clang_arch) orelse
        findClangRtLib(b, paths, "ubsan", clang_arch) orelse
        return false;

    exe.addObjectFile(.{ .cwd_relative = asan });
    exe.addObjectFile(.{ .cwd_relative = ubsan });
    exe.addObjectFile(.{ .cwd_relative = lsan });

    if (findClangRtLib(b, paths, "asan-preinit", clang_arch)) |preinit| {
        exe.addObjectFile(.{ .cwd_relative = preinit });
    }

    return true;
}

fn findGccSanitizers(
    b: *std.Build,
    paths: []const []const u8,
) ?SanitizerTriplet {
    const asan = findLibByBase(b, paths, "asan") orelse return null;
    const ubsan = findLibByBase(b, paths, "ubsan") orelse return null;
    const lsan = findLibByBase(b, paths, "lsan") orelse return null;
    return .{ .asan = asan, .ubsan = ubsan, .lsan = lsan };
}

fn findLibByBase(
    b: *std.Build,
    paths: []const []const u8,
    base: []const u8,
) ?[]const u8 {
    const so_name = b.fmt("lib{s}.so", .{base});
    const a_name = b.fmt("lib{s}.a", .{base});
    if (findExactInPaths(b, paths, so_name)) |path| return path;
    if (findExactInPaths(b, paths, a_name)) |path| return path;

    const so_prefix = b.fmt("lib{s}.so.", .{base});
    return findVersionedInPaths(b, paths, so_prefix);
}

fn findClangRtLib(
    b: *std.Build,
    paths: []const []const u8,
    base: []const u8,
    arch: []const u8,
) ?[]const u8 {
    const so_name = b.fmt("libclang_rt.{s}-{s}.so", .{ base, arch });
    const a_name = b.fmt("libclang_rt.{s}-{s}.a", .{ base, arch });
    if (findExactInPaths(b, paths, so_name)) |path| return path;
    if (findExactInPaths(b, paths, a_name)) |path| return path;

    const alt_so = b.fmt("libclang_rt.{s}.so", .{base});
    const alt_a = b.fmt("libclang_rt.{s}.a", .{base});
    if (findExactInPaths(b, paths, alt_so)) |path| return path;
    if (findExactInPaths(b, paths, alt_a)) |path| return path;
    return null;
}

fn buildSanitizerSearchPaths(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
) []const []const u8 {
    var list = std.ArrayList([]const u8).empty;

    const base_paths = [_][]const u8{
        "/usr/local/lib",
        "/usr/local/lib64",
        "/usr/lib",
        "/usr/lib64",
        "/lib",
        "/lib64",
    };
    for (base_paths) |path| {
        appendDirIfExists(b.allocator, &list, path);
    }

    if (gnuMultiArch(target)) |multi| {
        appendDirIfExists(b.allocator, &list, b.fmt("/usr/lib/{s}", .{multi}));
        appendDirIfExists(b.allocator, &list, b.fmt("/lib/{s}", .{multi}));
        appendGccRuntimePaths(b, &list, "/usr/lib/gcc", multi);
    } else {
        appendGccRuntimePaths(b, &list, "/usr/lib/gcc", null);
    }

    appendClangRuntimePaths(b, &list, "/usr/lib/clang");
    appendClangRuntimePaths(b, &list, "/usr/local/lib/clang");
    appendClangRuntimePaths(b, &list, "/usr/lib64/clang");
    appendLlvmRuntimePaths(b, &list, "/usr/lib");
    appendLlvmRuntimePaths(b, &list, "/usr/local");

    return list.toOwnedSlice(b.allocator) catch @panic("oom");
}

fn appendDirIfExists(
    allocator: std.mem.Allocator,
    list: *std.ArrayList([]const u8),
    path: []const u8,
) void {
    if (!dirExists(path)) return;
    list.append(allocator, path) catch @panic("oom");
}

fn appendGccRuntimePaths(
    b: *std.Build,
    list: *std.ArrayList([]const u8),
    base: []const u8,
    multiarch: ?[]const u8,
) void {
    if (multiarch) |arch| {
        const gcc_arch = b.fmt("{s}/{s}", .{ base, arch });
        appendVersionedDirs(b, list, gcc_arch);
        return;
    }
    appendVersionedDirs(b, list, base);
}

fn appendLlvmRuntimePaths(
    b: *std.Build,
    list: *std.ArrayList([]const u8),
    root: []const u8,
) void {
    var dir = std.fs.openDirAbsolute(root, .{ .iterate = true }) catch return;
    defer dir.close();

    var it = dir.iterate();
    while (it.next() catch null) |entry| {
        if (entry.kind != .directory) continue;
        if (!std.mem.startsWith(u8, entry.name, "llvm-") and !std.mem.eql(u8, entry.name, "llvm")) {
            continue;
        }
        const clang_base = b.fmt("{s}/{s}/lib/clang", .{ root, entry.name });
        appendClangRuntimePaths(b, list, clang_base);
    }
}

fn appendClangRuntimePaths(
    b: *std.Build,
    list: *std.ArrayList([]const u8),
    base: []const u8,
) void {
    var dir = std.fs.openDirAbsolute(base, .{ .iterate = true }) catch return;
    defer dir.close();

    var it = dir.iterate();
    while (it.next() catch null) |entry| {
        if (entry.kind != .directory) continue;
        const runtime = b.fmt("{s}/{s}/lib/linux", .{ base, entry.name });
        if (dirExists(runtime)) {
            list.append(b.allocator, runtime) catch @panic("oom");
        }
    }
}

fn appendVersionedDirs(
    b: *std.Build,
    list: *std.ArrayList([]const u8),
    base: []const u8,
) void {
    var dir = std.fs.openDirAbsolute(base, .{ .iterate = true }) catch return;
    defer dir.close();

    var it = dir.iterate();
    while (it.next() catch null) |entry| {
        if (entry.kind != .directory) continue;
        const path = b.fmt("{s}/{s}", .{ base, entry.name });
        if (dirExists(path)) {
            list.append(b.allocator, path) catch @panic("oom");
        }
    }
}

fn findExactInPaths(
    b: *std.Build,
    paths: []const []const u8,
    file_name: []const u8,
) ?[]const u8 {
    for (paths) |dir_path| {
        const full_path = b.fmt("{s}/{s}", .{ dir_path, file_name });
        if (fileExists(full_path)) {
            return full_path;
        }
    }
    return null;
}

fn findVersionedInPaths(
    b: *std.Build,
    paths: []const []const u8,
    prefix: []const u8,
) ?[]const u8 {
    for (paths) |dir_path| {
        if (findVersioned(b, dir_path, prefix)) |entry_name| {
            return b.fmt("{s}/{s}", .{ dir_path, entry_name });
        }
    }
    return null;
}

fn findVersioned(
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

fn fileExists(path: []const u8) bool {
    std.fs.accessAbsolute(path, .{}) catch return false;
    return true;
}

fn dirExists(path: []const u8) bool {
    var dir = std.fs.openDirAbsolute(path, .{}) catch return false;
    dir.close();
    return true;
}

fn clangRtArch(target: std.Build.ResolvedTarget) ?[]const u8 {
    return switch (target.result.cpu.arch) {
        .x86_64 => "x86_64",
        .aarch64 => "aarch64",
        .arm => "arm",
        .riscv64 => "riscv64",
        else => null,
    };
}

fn gnuMultiArch(target: std.Build.ResolvedTarget) ?[]const u8 {
    return switch (target.result.cpu.arch) {
        .x86_64 => "x86_64-linux-gnu",
        .aarch64 => "aarch64-linux-gnu",
        .arm => "arm-linux-gnueabihf",
        .riscv64 => "riscv64-linux-gnu",
        else => null,
    };
}
