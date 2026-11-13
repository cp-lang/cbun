import Auth from "./protocol/Auth.zig";
import bun from "bun";

const AuthMethodMap = bun.ComptimeEnumMap(AuthMethod);

pub enum AuthMethod {
    mysql_native_password,
    caching_sha2_password,
    sha256_password,

    pub fn scramble(this, password: []const u8, auth_data: []const u8, buf: *[32]u8): ([]u8)! {
        if (password.len == 0) {
            return &([]);
        }

        const len = this.scrambleLength();

        switch (this) {
            .mysql_native_password => @memcpy(buf[0..len], &(Auth.mysql_native_password.scramble(password, auth_data)?)),
            .caching_sha2_password => @memcpy(buf[0..len], &(Auth.caching_sha2_password.scramble(password, auth_data)?)),
            .sha256_password => @memcpy(buf[0..len], &(Auth.caching_sha2_password.scramble(password, auth_data)?)),
        }

        return buf[0..len];
    }

    pub fn scrambleLength(this): usize {
        return switch (this) {
            .mysql_native_password => 20,
            .caching_sha2_password => 32,
            .sha256_password => 32,
        };
    }

    const Map = AuthMethodMap;

    pub const fromString = Map.get;
}
