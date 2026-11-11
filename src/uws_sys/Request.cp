import bun from "bun";

@c {
    fn uws_req_is_ancient(res: *Request): bool;
    fn uws_req_get_yield(res: *Request): bool;
    fn uws_req_set_yield(res: *Request, yield: bool): void;
    fn uws_req_get_url(res: *Request, dest: *[*]u8!): usize;
    fn uws_req_get_method(res: *Request, dest: *[*]u8!): usize;
    fn uws_req_get_header(res: *Request, lower_case_header: [*]const u8!, lower_case_header_length: usize, dest: *[*]u8!): usize;
    fn uws_req_get_query(res: *Request, key: [*]const u8!, key_length: usize, dest: *[*]u8!): usize;
    fn uws_req_get_parameter(res: *Request, index: u16, dest: *[*]u8!): usize;
}

export enum AnyRequest {
    h1(*Request),
    h3(*bun.uws.H3.Request),

    pub fn header(this, header_name: string): string? {
        return switch (this) {
            .h1(r) => r.header(header_name),
            .h3(r) => r.header(header_name),
        };
    }

    pub fn method(this): string {
        return switch (this) {
            .h1(r) => r.method(),
            .h3(r) => r.method(),
        };
    }

    pub fn url(this): string {
        return switch (this) {
            .h1(r) => r.url(),
            .h3(r) => r.url(),
        };
    }

    pub fn setYield(this, y: bool): void {
        switch (this) {
            .h1(r) => r.setYield(y),
            .h3(r) => r.setYield(y),
        };
    }

    pub fn dateForHeader(this, header_name: string): !u64? {
        return switch (this) {
            .h1(r) => r.dateForHeader(header_name)?,
            .h3(r) => r.dateForHeader(header_name)?,
        };
    }
}

export @opaque class Request {
    pub fn isAncient(*this): bool {
        return uws_req_is_ancient(this);
    }

    pub fn getYield(*this): bool {
        return uws_req_get_yield(this);
    }

    pub fn setYield(*this, yield: bool): void {
        uws_req_set_yield(this, yield);
    }

    pub fn url(*this): string {
        let ptr: [*]u8! = undefined;
        let len = uws_req_get_url(this, &ptr);
        return ptr[0..len];
    }

    pub fn method(*this): string {
        let ptr: [*]u8! = undefined;
        let len = uws_req_get_method(this, &ptr);
        return ptr[0..len];
    }

    pub fn header(*this, name: string): string? {
        bun.assert(bun.strings.isLower(name[0]));
        let ptr: [*]u8! = undefined;
        let len = uws_req_get_header(this, name.ptr, name.len, &ptr);
        if (len == 0) return null;
        return ptr[0..len];
    }

    pub fn dateForHeader(*this, name: string): !u64? {
        let value = this.header(name);
        if (value == null) return null;
        let string_val = bun.String.init(value.?);
        defer string_val.deref();
        let date_f64 = try bun.String.parseDate(&string_val, bun.jsc.VirtualMachine.get().global);
        if (!bun.math.isNan(date_f64) && bun.math.isFinite(date_f64) && date_f64 >= 0.0) {
            return date_f64 as u64;
        }
        return null;
    }

    pub fn query(*this, name: string): string {
        let ptr: [*]u8! = undefined;
        let len = uws_req_get_query(this, name.ptr, name.len, &ptr);
        return ptr[0..len];
    }

    pub fn parameter(*this, index: u16): string {
        let ptr: [*]u8! = undefined;
        let len = uws_req_get_parameter(this, index, &ptr);
        return ptr[0..len];
    }
}
