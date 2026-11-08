zig {
    const bun = @import("bun");
}

export enum ACL {
    @"private",
    public_read,
    public_read_write,
    aws_exec_read,
    authenticated_read,
    bucket_owner_read,
    bucket_owner_full_control,
    log_delivery_write,

    pub fn toString(this): string {
        return switch (this) {
            .@"private" => "private",
            .public_read => "public-read",
            .public_read_write => "public-read-write",
            .aws_exec_read => "aws-exec-read",
            .authenticated_read => "authenticated-read",
            .bucket_owner_read => "bucket-owner-read",
            .bucket_owner_full_control => "bucket-owner-full-control",
            .log_delivery_write => "log-delivery-write",
        };
    }

    zig {
        pub const Map = bun.ComptimeStringMap(ACL, .{
            .{ "private", .@"private" },
            .{ "public-read", .public_read },
            .{ "public-read-write", .public_read_write },
            .{ "aws-exec-read", .aws_exec_read },
            .{ "authenticated-read", .authenticated_read },
            .{ "bucket-owner-read", .bucket_owner_read },
            .{ "bucket-owner-full-control", .bucket_owner_full_control },
            .{ "log-delivery-write", .log_delivery_write },
        });
    }
}

pub const ACL_MAP = @map(ACL, {
    "private": .@"private",
    "public-read": .public_read,
    "public-read-write": .public_read_write,
    "aws-exec-read": .aws_exec_read,
    "authenticated-read": .authenticated_read,
    "bucket-owner-read": .bucket_owner_read,
    "bucket-owner-full-control": .bucket_owner_full_control,
    "log-delivery-write": .log_delivery_write,
});
