import * as std from "std";
import * as bun from "bun";
import * as paths from "./Path.zig";
import * as pools from "./path_buffer_pool.zig";
import * as env_path from "./EnvPath.zig";

const Environment = bun.Environment;

export const Path = paths.Path;
export const AbsPath = paths.AbsPath;
export const AutoAbsPath = paths.AutoAbsPath;
export const RelPath = paths.RelPath;
export const AutoRelPath = paths.AutoRelPath;

export const EnvPath = env_path.EnvPath;

export const path_buffer_pool = pools.path_buffer_pool;
export const w_path_buffer_pool = pools.w_path_buffer_pool;
export const os_path_buffer_pool = pools.os_path_buffer_pool;

export const MAX_PATH_BYTES: usize = if (Environment.isWasm) 1024 else std.fs.max_path_bytes;
export type PathBuffer = [MAX_PATH_BYTES]u8;
export const PATH_MAX_WIDE = std.os.windows.PATH_MAX_WIDE;
export type WPathBuffer = [PATH_MAX_WIDE]u16;
export type OSPathChar = if (Environment.isWindows) u16 else u8;
export type OSPathSliceZ = [:0]const OSPathChar;
export type OSPathSlice = []const OSPathChar;
export type OSPathBuffer = if (Environment.isWindows) WPathBuffer else PathBuffer;
