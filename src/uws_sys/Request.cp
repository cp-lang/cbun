import bun from "bun";

zig {
    const std = @import("std");
    const uws = bun.uws;

    const c = struct {
        pub extern fn uws_req_is_ancient(res: *Request) bool;
        pub extern fn uws_req_get_yield(res: *Request) bool;
        pub extern fn uws_req_set_yield(res: *Request, yield: bool) void;
        pub extern fn uws_req_get_url(res: *Request, dest: *[*]const u8) usize;
        pub extern fn uws_req_get_method(res: *Request, dest: *[*]const u8) usize;
        pub extern fn uws_req_get_header(res: *Request, lower_case_header: [*]const u8, lower_case_header_length: usize, dest: *[*]const u8) usize;
        pub extern fn uws_req_get_query(res: *Request, key: [*c]const u8, key_length: usize, dest: *[*]const u8) usize;
        pub extern fn uws_req_get_parameter(res: *Request, index: c_ushort, dest: *[*]const u8) usize;
    };
}

export enum AnyRequest {
    h1(*Request),
    h3(*bun.uws.H3.Request),

    pub fn header(this, name: string): string? {
        zig {
            return switch (self) {
                inline else => |r| r.header(name),
            };
        }
    }

    pub fn method(this): string {
        zig {
            return switch (self) {
                inline else => |r| r.method(),
            };
        }
    }

    pub fn url(this): string {
        zig {
            return switch (self) {
                inline else => |r| r.url(),
            };
        }
    }

    pub fn setYield(this, y: bool): void {
        zig {
            switch (self) {
                inline else => |r| r.setYield(y),
            }
        }
    }

    pub fn dateForHeader(this, name: string): !u64? {
        zig {
            return switch (self) {
                inline else => |r| r.dateForHeader(name),
            };
        }
    }
}

export @opaque class Request {
    pub fn isAncient(*this): bool {
        return c.uws_req_is_ancient(this);
    }

    pub fn getYield(*this): bool {
        return c.uws_req_get_yield(this);
    }

    pub fn setYield(*this, yield: bool): void {
        c.uws_req_set_yield(this, yield);
    }

    pub fn url(*this): string {
        zig {
            var ptr: [*]const u8 = undefined;
            return ptr[0..c.uws_req_get_url(self, &ptr)];
        }
    }

    pub fn method(*this): string {
        zig {
            var ptr: [*]const u8 = undefined;
            return ptr[0..c.uws_req_get_method(self, &ptr)];
        }
    }

    pub fn header(*this, name: string): string? {
        zig {
            bun.assert(std.ascii.isLower(name[0]));
            var ptr: [*]const u8 = undefined;
            const len = c.uws_req_get_header(self, name.ptr, name.len, &ptr);
            if (len == 0) return null;
            return ptr[0..len];
        }
    }

    pub fn dateForHeader(*this, name: string): !u64? {
        zig {
            const value = self.header(name);
            if (value == null) return null;
            var string_val = bun.String.init(value.?);
            defer string_val.deref();
            const date_f64 = try bun.String.parseDate(&string_val, bun.jsc.VirtualMachine.get().global);
            if (!std.math.isNan(date_f64) and std.math.isFinite(date_f64) and date_f64 >= 0) {
                return @intFromFloat(date_f64);
            }
            return null;
        }
    }

    pub fn query(*this, name: string): string {
        zig {
            var ptr: [*]const u8 = undefined;
            return ptr[0..c.uws_req_get_query(self, name.ptr, name.len, &ptr)];
        }
    }

    pub fn parameter(*this, index: u16): string {
        zig {
            var ptr: [*]const u8 = undefined;
            return ptr[0..c.uws_req_get_parameter(self, @as(c_ushort, @intCast(index)), &ptr)];
        }
    }
}
