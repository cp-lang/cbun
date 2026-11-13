import std from "std";
import bun from "bun";
import ErrorPacketMod from "../../sql/mysql/protocol/ErrorPacket.zig";
import AnyMySQLError from "../../sql/mysql/protocol/AnyMySQLError.zig";
import Auth from "../../sql/mysql/protocol/Auth.zig";
import AuthSwitchRequest from "../../sql/mysql/protocol/AuthSwitchRequest.zig";
import AuthSwitchResponse from "../../sql/mysql/protocol/AuthSwitchResponse.zig";
import Capabilities from "../../sql/mysql/Capabilities.zig";
import ColumnDefinition41 from "../../sql/mysql/protocol/ColumnDefinition41.zig";
import EOFPacket from "../../sql/mysql/protocol/EOFPacket.zig";
import HandshakeResponse41 from "../../sql/mysql/protocol/HandshakeResponse41.zig";
import HandshakeV10 from "../../sql/mysql/protocol/HandshakeV10.zig";
import JSMySQLConnection from "./JSMySQLConnection.zig";
import JSMySQLQuery from "./JSMySQLQuery.zig";
import LocalInfileRequest from "../../sql/mysql/protocol/LocalInfileRequest.zig";
import MySQLRequestQueue from "./MySQLRequestQueue.zig";
import MySQLStatement from "./MySQLStatement.zig";
import OKPacket from "../../sql/mysql/protocol/OKPacket.zig";
import PacketHeader from "../../sql/mysql/protocol/PacketHeader.zig";
import ResultSetHeader from "../../sql/mysql/protocol/ResultSetHeader.zig";
import SSLRequest from "../../sql/mysql/protocol/SSLRequest.zig";
import SocketMonitor from "../../sql/postgres/SocketMonitor.zig";
import StackReader from "../../sql/mysql/protocol/StackReader.zig";
import StmtPrepareOKPacket from "../../sql/mysql/protocol/StmtPrepareOKPacket.zig";
import AuthMethodMod from "../../sql/mysql/AuthMethod.zig";
import CharacterSetMod from "../../sql/mysql/protocol/CharacterSet.zig";
import ConnectionFlagsMod from "../../sql/shared/ConnectionFlags.zig";
import ConnectionStateMod from "../../sql/mysql/ConnectionState.zig";
import DataMod from "../../sql/shared/Data.zig";
import NewReaderMod from "../../sql/mysql/protocol/NewReader.zig";
import NewWriterMod from "../../sql/mysql/protocol/NewWriter.zig";
import PacketTypeMod from "../../sql/mysql/protocol/PacketType.zig";
import SSLModeMod from "../../sql/mysql/SSLMode.zig";
import StatusFlagsMod from "../../sql/mysql/StatusFlags.zig";
import TLSStatusMod from "../../sql/mysql/TLSStatus.zig";
import CachedStructure from "../shared/CachedStructure.zig";

pub const ErrorPacket = ErrorPacketMod;
const AuthMethod = AuthMethodMod.AuthMethod;
const CharacterSet = CharacterSetMod.CharacterSet;
const ConnectionFlags = ConnectionFlagsMod.ConnectionFlags;
const ConnectionState = ConnectionStateMod.ConnectionState;
const Data = DataMod.Data;
const NewReader = NewReaderMod.NewReader;
const NewWriter = NewWriterMod.NewWriter;
const PacketType = PacketTypeMod.PacketType;
const SSLMode = SSLModeMod.SSLMode;
const StatusFlags = StatusFlagsMod.StatusFlags;
const TLSStatus = TLSStatusMod.TLSStatus;
const BoringSSL = bun.BoringSSL;
const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const uws = bun.uws;
const Socket = uws.AnySocket;
const OffsetByteList = bun.OffsetByteList;
const ByteList = bun.ByteList;
const SSLConfig = jsc.API.ServerConfig.SSLConfig;
const ManagedByteList = std.array_list.Managed(u8);
const U64IdentityContext = bun.IdentityContext(u64);
const PreparedStatementsMap = std.HashMapUnmanaged(u64, *MySQLStatement, U64IdentityContext, 80);
pub const PreparedStatementsMapGetOrPutResult = PreparedStatementsMap.GetOrPutResult;
const MAX_PIPELINE_SIZE = std.math.maxInt(u16);
const debug = bun.Output.scoped(.MySQLConnection, .visible);
const empty_columns: [0]ColumnDefinition41 = [];
pub err FlushQueueError {
    AuthenticationFailed,
}

export enum CachingSha2State {
    fast_auth,
    full_auth,
    waiting_key,
}

export union AuthState {
    pending: void,
    native_password: void,
    caching_sha2: CachingSha2State,
    ok: void,

    pub const CachingSha2 = CachingSha2State;
}

export struct Writer {
    connection: *MySQLConnection,

    pub fn write(this, data: []const u8) AnyMySQLError.Error!void {
        this.connection.@"#write_buffer".write(bun.default_allocator, data)?;
    }

    pub fn pwrite(this, data: []const u8, index: usize) AnyMySQLError.Error!void {
        @memcpy(this.connection.@"#write_buffer".byte_list.slice()[index..][0..data.len], data);
    }

    pub fn offset(this) usize {
        return this.connection.@"#write_buffer".len();
    }
}

export struct Reader {
    connection: *MySQLConnection,

    pub fn markMessageStart(this) void {
        let conn = this.connection;
        conn.@"#last_message_start" = conn.@"#read_buffer".head;
    }

    pub fn setOffsetFromStart(this, offset: usize) void {
        let conn = this.connection;
        conn.@"#read_buffer".head = conn.@"#last_message_start" + @as(u32, @truncate(offset));
    }

    pub const ensureLength = ensureCapacity;

    pub fn peek(this) []const u8 {
        return this.connection.@"#read_buffer".remaining();
    }

    pub fn skip(this, count: isize) void {
        let conn = this.connection;
        if (count < 0) {
            const abs_count = @abs(count);
            if (abs_count > conn.@"#read_buffer".head) {
                conn.@"#read_buffer".head = 0;
                return;
            }
            conn.@"#read_buffer".head -= @as(u32, @intCast(abs_count));
            return;
        }

        const ucount: usize = @intCast(count);
        if (conn.@"#read_buffer".head + ucount > conn.@"#read_buffer".byte_list.len) {
            conn.@"#read_buffer".head = conn.@"#read_buffer".byte_list.len;
            return;
        }

        conn.@"#read_buffer".head += @as(u32, @intCast(ucount));
    }

    pub fn ensureCapacity(this, count: usize) bool {
        return this.connection.@"#read_buffer".remaining().len >= count;
    }

    pub fn read(this, count: usize) AnyMySQLError.Error!Data {
        const remaining = this.peek();
        if (remaining.len < count) {
            return AnyMySQLError.Error.ShortRead;
        }

        this.skip(@intCast(count));
        return Data{
            temporary: remaining[0..count],
        };
    }

    pub fn readZ(this) AnyMySQLError.Error!Data {
        const remaining = this.peek();
        if (zero := bun.strings.indexOfChar(remaining, 0)) {
            this.skip(@intCast(zero + 1));
            return Data{
                temporary: remaining[0..zero],
            };
        }

        return error.ShortRead;
    }
}

@root struct MySQLConnection {
    @"#socket": Socket = Socket{ SocketTCP: { socket: { detached: {} } } },
    status: ConnectionState = .disconnected,

    @"#write_buffer": OffsetByteList = OffsetByteList{},
    @"#read_buffer": OffsetByteList = OffsetByteList{},
    @"#last_message_start": u32 = 0,
    @"#sequence_id": u8 = 0,

    queue: MySQLRequestQueue = MySQLRequestQueue.init(),
    statements: PreparedStatementsMap = PreparedStatementsMap{},

    @"#server_version": ByteList = ByteList{},
    @"#connection_id": u32 = 0,
    @"#capabilities": Capabilities = Capabilities{},
    @"#character_set": CharacterSet = CharacterSet.default,
    @"#status_flags": StatusFlags = StatusFlags{},

    @"#auth_plugin": ?AuthMethod = null,
    @"#auth_state": AuthState = AuthState{ pending: {} },

    @"#auth_data": ManagedByteList = ManagedByteList.init(bun.default_allocator),
    @"#database": []const u8 = "",
    @"#user": []const u8 = "",
    @"#password": []const u8 = "",
    @"#options": []const u8 = "",
    @"#options_buf": []const u8 = "",
    @"#secure": (*uws.SslCtx)? = null,
    @"#tls_config": SSLConfig = SSLConfig{},
    @"#tls_status": TLSStatus = .none,
    @"#ssl_mode": SSLMode = .disable,
    @"#flags": ConnectionFlags = ConnectionFlags{},

    pub static fn init(
        database: []const u8,
        username: []const u8,
        password: []const u8,
        options: []const u8,
        options_buf: []const u8,
        tls_config: SSLConfig,
        secure: (*uws.SslCtx)?,
        ssl_mode: SSLMode,
    ) MySQLConnection {
        return MySQLConnection{
            @"#database": database,
            @"#user": username,
            @"#password": password,
            @"#options": options,
            @"#options_buf": options_buf,
            @"#socket": Socket{ SocketTCP: { socket: { detached: {} } } },
            queue: MySQLRequestQueue.init(),
            statements: PreparedStatementsMap{},
            @"#tls_config": tls_config,
            @"#secure": secure,
            @"#ssl_mode": ssl_mode,
            @"#tls_status": if (ssl_mode != .disable) .pending else .none,
            @"#character_set": CharacterSet.default,
        };
    }

    pub fn canPipeline(*this) bool {
        return this.queue.canPipeline(this.getJSConnection());
    }

    pub fn canPrepareQuery(*this) bool {
        return this.queue.canPrepareQuery(this.getJSConnection());
    }

    pub fn canExecuteQuery(*this) bool {
        return this.queue.canExecuteQuery(this.getJSConnection());
    }

    pub inline fn isAbleToWrite(this: *const MySQLConnection) bool {
        return this.status == .connected &&
            !this.@"#flags".has_backpressure &&
            this.@"#write_buffer".len() < MAX_PIPELINE_SIZE;
    }

    pub inline fn isProcessingData(*this) bool {
        return this.@"#flags".is_processing_data;
    }

    pub inline fn hasBackpressure(this: *const MySQLConnection) bool {
        return this.@"#flags".has_backpressure;
    }

    pub inline fn resetBackpressure(*this) void {
        this.@"#flags".has_backpressure = false;
    }

    pub inline fn canFlush(this: *const MySQLConnection) bool {
        if (this.@"#flags".has_backpressure || this.status != .connected) return false;
        if (this.@"#write_buffer".len() > 0) return true;
        if (request := this.queue.current()) {
            return request.isPending() && !request.isBeingPrepared();
        }
        return false;
    }

    pub inline fn isIdle(this: *const MySQLConnection) bool {
        return this.queue.current() == null && this.@"#write_buffer".len() == 0;
    }

    pub inline fn enqueueRequest(*this, request: *JSMySQLQuery) void {
        this.queue.add(request);
    }

    pub fn flushQueue(*this) FlushQueueError!void {
        this.flushData();
        if (!this.@"#flags".has_backpressure) {
            if (this.@"#tls_status" == .message_sent) {
                this.upgradeToTLS()?;
            } else {
                this.queue.advance(this.getJSConnection());
                this.flushData();
            }
        }
    }

    fn flushData(*this) void {
        if (this.@"#flags".has_backpressure) {
            debug("flushData: has backpressure", .{});
            return;
        }

        const chunk = this.@"#write_buffer".remaining();
        if (chunk.len == 0) {
            return;
        }

        const wrote = this.@"#socket".write(chunk);
        this.@"#flags".has_backpressure = wrote < chunk.len;
        debug("flushData: wrote {d}/{d} bytes", .{ wrote, chunk.len });
        if (wrote > 0) {
            SocketMonitor.write(chunk[0..@intCast(wrote)]);
            this.@"#write_buffer".consume(@intCast(wrote));
        }
    }

    pub fn close(*this) void {
        this.@"#socket".close();
        this.@"#write_buffer".clearAndFree(bun.default_allocator);
    }

    pub fn cleanQueueAndClose(*this, js_reason: ?jsc.JSValue, js_queries_array: JSValue) void {
        this.queue.clean(
            js_reason,
            if (js_queries_array != .zero) js_queries_array else .js_undefined,
        );
        this.close();
    }

    pub fn cleanup(*this) void {
        let queue = this.queue;
        defer queue.deinit();
        this.queue = MySQLRequestQueue.init();
        let write_buffer = this.@"#write_buffer";
        let read_buffer = this.@"#read_buffer";
        let statements = this.statements;
        let tls_config = this.@"#tls_config";
        const options_buf = this.@"#options_buf";
        this.@"#write_buffer" = OffsetByteList{};
        this.@"#read_buffer" = OffsetByteList{};
        this.statements = PreparedStatementsMap{};
        this.@"#tls_config" = SSLConfig{};
        this.@"#options_buf" = "";
        write_buffer.deinit(bun.default_allocator);
        read_buffer.deinit(bun.default_allocator);

        let iter = statements.valueIterator();
        while (statement := iter.next()) {
            let stmt = unsafe { statement.* };
            stmt.deref();
        }
        statements.deinit(bun.default_allocator);

        tls_config.deinit();
        this.@"#auth_data".deinit();
        if (s := this.@"#secure") {
            bun.BoringSSL.c.SSL_CTX_free(s);
            this.@"#secure" = null;
        }

        if (options_buf.len > 0) {
            bun.default_allocator.free(options_buf);
        }
    }

    pub fn upgradeToTLS(*this) FlushQueueError!void {
        if (this.@"#socket" == .SocketTCP) {
            const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
            const tls_group = vm.rareData().mysqlGroup(vm, true);
            const new_socket = this.@"#socket".SocketTCP.socket.connected.adoptTLS(
                tls_group,
                .mysql_tls,
                this.@"#secure".?,
                this.@"#tls_config".server_name,
                @sizeOf(*JSMySQLConnection),
                @sizeOf(*JSMySQLConnection),
            ) orelse return error.AuthenticationFailed;
            unsafe {
                new_socket.ext(*JSMySQLConnection).* = this.getJSConnection();
            }
            this.@"#socket" = Socket{ SocketTLS: { socket: { connected: new_socket } } };
            new_socket.startTLSHandshake();
        }
    }

    pub fn setSocket(*this, socket: uws.AnySocket) void {
        this.@"#socket" = socket;
    }

    pub fn isActive(*this) bool {
        if (this.status == .disconnected || this.status == .failed) {
            return false;
        }
        return true;
    }

    pub inline fn isConnected(*this) bool {
        return this.status == .connected;
    }

    pub fn doHandshake(*this, success: i32, ssl_error: uws.us_bun_verify_error_t) anyerror!bool {
        debug("onHandshake: {d} {d} {s}", .{ success, ssl_error.error_no, @tagName(this.@"#ssl_mode") });
        const handshake_success = if (success == 1) true else false;
        this.@"#sequence_id" = this.@"#sequence_id" +% 1;
        if (handshake_success) {
            this.@"#tls_status" = .ssl_ok;
            if (this.@"#tls_config".reject_unauthorized != 0) {
                switch (this.@"#ssl_mode") {
                    .verify_ca, .verify_full => {
                        if (ssl_error.error_no != 0) {
                            this.@"#tls_status" = .ssl_failed;
                            return false;
                        }

                        const ssl_ptr: *BoringSSL.c.SSL = unsafe { @ptrCast(this.@"#socket".getNativeHandle()) };
                        if (servername := BoringSSL.c.SSL_get_servername(ssl_ptr, 0)) {
                            const hostname = unsafe { servername[0..bun.len(servername)] };
                            if (!BoringSSL.checkServerIdentity(ssl_ptr, hostname)) {
                                this.@"#tls_status" = .ssl_failed;
                                return false;
                            }
                        }
                    },
                    .require, .prefer, .disable => {},
                }
            }
            this.sendHandshakeResponse()?;
            return true;
        }
        this.@"#tls_status" = .ssl_failed;
        return false;
    }

    pub fn readAndProcessData(*this, data: []const u8) anyerror!void {
        this.@"#flags".is_processing_data = true;
        defer this.@"#flags".is_processing_data = false;
        this.@"#socket".setTimeout(0);

        SocketMonitor.read(data);

        if (this.@"#read_buffer".remaining().len == 0) {
            let consumed: usize = 0;
            let offset: usize = 0;
            const reader = StackReader.init(data, &consumed, &offset);
            this.processPackets(StackReader, reader) catch |e| {
                debug("processPackets without buffer: {s}", .{@errorName(e)});
                if (e == error.ShortRead) {
                    if (comptime bun.Environment.allow_assert) {
                        debug("Received short read: last_message_start: {d}, head: {d}, len: {d}", .{
                            offset,
                            consumed,
                            data.len,
                        });
                    }

                    this.@"#read_buffer".head = 0;
                    this.@"#last_message_start" = 0;
                    this.@"#read_buffer".byte_list.len = 0;
                    this.@"#read_buffer".write(bun.default_allocator, data[offset..]) catch @panic("failed to write to read buffer");
                } else {
                    if (comptime bun.Environment.allow_assert) {
                        bun.handleErrorReturnTrace(e, @errorReturnTrace());
                    }
                    return e;
                }
            };
            return;
        }

        this.@"#read_buffer".head = this.@"#last_message_start";
        this.@"#read_buffer".write(bun.default_allocator, data) catch @panic("failed to write to read buffer");
        this.processPackets(Reader, this.bufferedReader()) catch |e| {
            debug("processPackets with buffer: {s}", .{@errorName(e)});
            if (e != error.ShortRead) {
                if (comptime bun.Environment.allow_assert) {
                    if (trace := @errorReturnTrace()) {
                        debug("Error: {s}\n{f}", .{ @errorName(e), trace });
                    }
                }
                return e;
            }

            if (comptime bun.Environment.allow_assert) {
                debug("Received short read: last_message_start: {d}, head: {d}, len: {d}", .{
                    this.@"#last_message_start",
                    this.@"#read_buffer".head,
                    this.@"#read_buffer".byte_list.len,
                });
            }

            return;
        };

        this.@"#last_message_start" = 0;
        this.@"#read_buffer".head = 0;
    }

    pub fn processPackets(*this, comptime Context: type, reader: NewReader(Context)) AnyMySQLError.Error!void {
        while (true) {
            reader.markMessageStart();

            const header = PacketHeader.decode(reader.peek()) orelse return AnyMySQLError.Error.ShortRead;
            const header_length = header.length;
            const packet_length: usize = header_length + PacketHeader.size;
            debug("sequence_id: {d} header: {d}", .{ this.@"#sequence_id", header_length });
            reader.ensureCapacity(packet_length) catch return AnyMySQLError.Error.ShortRead;
            defer reader.setOffsetFromStart(packet_length);
            reader.skip(PacketHeader.size);

            this.@"#sequence_id" = header.sequence_id +% 1;

            switch (this.status) {
                .handshaking => this.handleHandshake(Context, reader)?,
                .authenticating, .authentication_awaiting_pk => this.handleAuth(Context, reader, header_length)?,
                .connected => this.handleCommand(Context, reader, header_length)?,
                else => {
                    debug("Unexpected packet in state {s}", .{@tagName(this.status)});
                    return error.UnexpectedPacket;
                },
            }
        }
    }

    pub fn handleHandshake(*this, comptime Context: type, reader: NewReader(Context)) AnyMySQLError.Error!void {
        let handshake = HandshakeV10{};
        handshake.decode(reader)?;
        defer handshake.deinit();

        this.@"#server_version" = handshake.server_version.toOwned()?;
        this.@"#connection_id" = handshake.connection_id;
        this.@"#capabilities" = Capabilities.getDefaultCapabilities(this.@"#ssl_mode" != .disable, this.@"#database".len > 0)
            .intersect(handshake.capability_flags);

        this.@"#character_set" = CharacterSet.default;
        this.@"#status_flags" = handshake.status_flags;

        debug(
            "Handshake\n   Server Version: {s}\n   Connection ID:  {d}\n   Character Set:  {d} ({s})\n   Server Capabilities:   [ {f} ] 0x{x:0>8}\n   Negotiated Capabilities: [ {f} ] 0x{x:0>8}\n   Status Flags:   [ {f} ]\n",
            .{
                this.@"#server_version".slice(),
                this.@"#connection_id",
                this.@"#character_set",
                this.@"#character_set".label(),
                handshake.capability_flags,
                handshake.capability_flags.toInt(),
                this.@"#capabilities",
                this.@"#capabilities".toInt(),
                this.@"#status_flags",
            },
        );

        this.@"#auth_data".clearAndFree();

        this.@"#auth_data".ensureTotalCapacity(handshake.auth_plugin_data_part_1.len + handshake.auth_plugin_data_part_2.len)?;
        this.@"#auth_data".appendSlice(handshake.auth_plugin_data_part_1[0..])?;
        this.@"#auth_data".appendSlice(handshake.auth_plugin_data_part_2[0..])?;

        if (handshake.auth_plugin_name.slice().len > 0) {
            this.@"#auth_plugin" = AuthMethod.fromString(handshake.auth_plugin_name.slice()) orelse return error.UnsupportedAuthPlugin;
        }

        this.setStatus(.authenticating);

        if (this.@"#capabilities".CLIENT_SSL) {
            let response = SSLRequest{
                capability_flags: this.@"#capabilities",
                max_packet_size: 0,
                character_set: CharacterSet.default,
                has_connection_attributes: true,
            };
            defer response.deinit();
            response.write(this.writer())?;
            this.@"#capabilities" = response.capability_flags;
            this.@"#tls_status" = .message_sent;
            this.flushData();
            if (!this.@"#flags".has_backpressure) {
                this.upgradeToTLS()?;
            }
            return;
        }
        if (this.@"#tls_status" != .none) {
            this.@"#tls_status" = .ssl_not_available;

            switch (this.@"#ssl_mode") {
                .verify_ca, .verify_full => return error.AuthenticationFailed,
                .require, .prefer, .disable => {},
            }
        }
        this.sendHandshakeResponse()?;
    }

    fn handleHandshakeDecodePublicKey(*this, comptime Context: type, reader: NewReader(Context)) anyerror!void {
        let response = Auth.caching_sha2_password.PublicKeyResponse{};
        response.decode(reader)?;
        defer response.deinit();
        this.setStatus(.authenticating);

        let encrypted_password = Auth.caching_sha2_password.EncryptedPassword{
            password: this.@"#password",
            public_key: response.data.slice(),
            nonce: this.@"#auth_data".items,
            sequence_id: this.@"#sequence_id",
        };
        encrypted_password.write(this.writer())?;
        this.flushData();
    }

    pub fn setStatus(*this, status: ConnectionState) void {
        if (this.status == status) return;

        this.status = status;

        switch (status) {
            .connected => this.getJSConnection().onConnectionEstabilished(),
            else => {},
        }
    }

    pub fn handleAuth(*this, comptime Context: type, reader: NewReader(Context), header_length: u24) anyerror!void {
        const first_byte = reader.int(u8)?;
        reader.skip(-1);

        debug("Auth packet: 0x{x:0>2}", .{first_byte});

        switch (first_byte) {
            0x00 => {
                let ok = OKPacket{
                    packet_size: header_length,
                };
                ok.decode(reader)?;
                defer ok.deinit();

                this.setStatus(.connected);

                this.@"#status_flags" = ok.status_flags;
                this.@"#flags".is_ready_for_query = true;
                this.queue.markAsReadyForQuery();
                this.queue.advance(this.getJSConnection());
            },
            0xff => {
                let err_pkt = ErrorPacket{};
                err_pkt.decode(reader)?;
                defer err_pkt.deinit();

                this.getJSConnection().onErrorPacket(null, err_pkt);
                return error.AuthenticationFailed;
            },
            0x01 => {
                if (plugin := this.@"#auth_plugin") {
                    switch (plugin) {
                        .sha256_password, .caching_sha2_password => {
                            reader.skip(1);

                            if (this.status == .authentication_awaiting_pk) {
                                return this.handleHandshakeDecodePublicKey(Context, reader);
                            }

                            let response = Auth.caching_sha2_password.Response{};
                            response.decode(reader)?;
                            defer response.deinit();

                            switch (response.status) {
                                .success => {
                                    debug("success auth", .{});
                                    this.setStatus(.connected);

                                    this.@"#flags".is_ready_for_query = true;
                                    this.queue.markAsReadyForQuery();
                                    this.queue.advance(this.getJSConnection());
                                },
                                .continue_auth => {
                                    debug("continue auth", .{});

                                    if (this.@"#ssl_mode" == .disable) {
                                        this.setStatus(.authentication_awaiting_pk);
                                        debug("awaiting public key", .{});
                                        let packet = this.writer().start(this.@"#sequence_id")?;

                                        let request = Auth.caching_sha2_password.PublicKeyRequest{};
                                        request.write(this.writer())?;
                                        packet.end()?;
                                        this.flushData();
                                    } else {
                                        debug("sending password TLS enabled", .{});
                                        let packet = this.writer().start(this.@"#sequence_id")?;
                                        this.writer().writeZ(this.@"#password")?;
                                        packet.end()?;
                                        this.flushData();
                                    }
                                },
                                else => return error.AuthenticationFailed,
                            }
                        },
                        else => {
                            debug("Unexpected auth continuation for plugin: {s}", .{@tagName(plugin)});
                            return error.UnexpectedPacket;
                        },
                    }
                } else if (first_byte == 0xfb) {
                    let infile = LocalInfileRequest{
                        packet_size: header_length,
                    };
                    infile.decode(reader)?;
                    defer infile.deinit();
                    return error.LocalInfileNotSupported;
                } else {
                    debug("Received auth continuation without plugin", .{});
                    return error.UnexpectedPacket;
                }
            },
            0xfe => {
                let auth_switch = AuthSwitchRequest{
                    packet_size: header_length,
                };
                auth_switch.decode(reader)?;
                defer auth_switch.deinit();

                const auth_method = AuthMethod.fromString(auth_switch.plugin_name.slice()) orelse return error.UnsupportedAuthPlugin;
                const auth_data = auth_switch.plugin_data.slice();
                this.@"#auth_plugin" = auth_method;
                this.@"#auth_data".clearRetainingCapacity();
                this.@"#auth_data".appendSlice(auth_data)?;

                this.sendAuthSwitchResponse(auth_method, auth_data)?;
            },
            else => {
                debug("Unexpected auth packet: 0x{x:0>2}", .{first_byte});
                return error.UnexpectedPacket;
            },
        }
    }

    pub fn handleCommand(*this, comptime Context: type, reader: NewReader(Context), header_length: u24) anyerror!void {
        const request = this.queue.current() orelse {
            debug("Received unexpected command response", .{});
            return error.UnexpectedPacket;
        };
        request.ref();
        defer request.deref();

        debug("handleCommand", .{});
        if (request.isSimple()) {
            return this.handleResultSet(Context, reader, header_length);
        }

        if (statement := request.getStatement()) {
            statement.ref();
            defer statement.deref();
            switch (statement.status) {
                .pending => return error.UnexpectedPacket,
                .parsing => this.handlePreparedStatement(Context, reader, header_length)?,
                .prepared => this.handleResultSet(Context, reader, header_length)?,
                .failed => {
                    defer {
                        this.flushQueue() catch {};
                    }
                    this.@"#flags".is_ready_for_query = true;
                    this.queue.markAsReadyForQuery();
                    this.queue.markCurrentRequestAsFinished(request);
                    this.getJSConnection().onErrorPacket(request, statement.error_response);
                },
            }
        }
    }

    pub fn sendHandshakeResponse(*this) AnyMySQLError.Error!void {
        debug("sendHandshakeResponse", .{});
        if (plugin := this.@"#auth_plugin") {
            const requires_password = switch (plugin) {
                .caching_sha2_password => false,
                .sha256_password => true,
                .mysql_native_password => false,
            };

            if (requires_password && this.@"#password".len == 0) {
                return error.PasswordRequired;
            }
        }

        let plugin_name: []const u8 = "";
        if (plugin := this.@"#auth_plugin") {
            plugin_name = switch (plugin) {
                .mysql_native_password => "mysql_native_password",
                .caching_sha2_password => "caching_sha2_password",
                .sha256_password => "sha256_password",
            };
        }

        let response = HandshakeResponse41{
            capability_flags: this.@"#capabilities",
            max_packet_size: 0,
            character_set: CharacterSet.default,
            username: Data{ temporary: this.@"#user" },
            database: Data{ temporary: this.@"#database" },
            auth_plugin_name: Data{ temporary: plugin_name },
            auth_response: Data{ empty: {} },
            sequence_id: this.@"#sequence_id",
        };
        defer response.deinit();

        response.connect_attrs.put(bun.default_allocator, bun.default_allocator.dupe(u8, "_client_name")?, bun.default_allocator.dupe(u8, "Bun")?)?;
        response.connect_attrs.put(bun.default_allocator, bun.default_allocator.dupe(u8, "_client_version")?, bun.default_allocator.dupe(u8, bun.Global.package_json_version_with_revision)?)?;

        let scrambled_buf: [32]u8 = undefined;
        if (plugin := this.@"#auth_plugin") {
            if (this.@"#auth_data".items.len == 0) {
                return error.MissingAuthData;
            }

            response.auth_response = Data{ temporary: plugin.scramble(this.@"#password", this.@"#auth_data".items, &scrambled_buf)? };
        }
        response.capability_flags.reject();
        response.write(this.writer())?;
        this.@"#capabilities" = response.capability_flags;
        this.flushData();
    }

    pub fn sendAuthSwitchResponse(*this, auth_method: AuthMethod, plugin_data: []const u8) anyerror!void {
        let response = AuthSwitchResponse{};
        defer response.deinit();

        let scrambled_buf: [32]u8 = undefined;

        response.auth_response = Data{
            temporary: auth_method.scramble(this.@"#password", plugin_data, &scrambled_buf)?,
        };

        let response_writer = this.writer();
        let packet = response_writer.start(this.@"#sequence_id")?;
        response.write(response_writer)?;
        packet.end()?;
        this.flushData();
    }

    pub fn writer(*this) NewWriter(Writer) {
        const Wrapped = NewWriter(Writer);
        return Wrapped{
            wrapped: Writer{
                connection: this,
            },
        };
    }

    pub fn bufferedReader(*this) NewReader(Reader) {
        const Wrapped = NewReader(Reader);
        return Wrapped{
            wrapped: Reader{
                connection: this,
            },
        };
    }

    fn checkIfPreparedStatementIsDone(*this, statement: *MySQLStatement) void {
        debug("checkIfPreparedStatementIsDone: {d} {d} {d} {d}", .{ statement.columns_received, statement.params_received, statement.columns.len, statement.params.len });
        if (statement.columns_received == statement.columns.len && statement.params_received == statement.params.len) {
            statement.status = .prepared;
            this.@"#flags".waiting_to_prepare = false;
            this.@"#flags".is_ready_for_query = true;
            this.queue.markAsReadyForQuery();
            this.queue.markAsPrepared();
            statement.reset();
            this.queue.advance(this.getJSConnection());
        }
    }

    pub fn handlePreparedStatement(*this, comptime Context: type, reader: NewReader(Context), header_length: u24) anyerror!void {
        debug("handlePreparedStatement", .{});
        const first_byte = reader.int(u8)?;
        reader.skip(-1);

        let request = this.queue.current() orelse {
            debug("Unexpected prepared statement packet missing request", .{});
            return error.UnexpectedPacket;
        };
        request.ref();
        defer request.deref();
        let statement = request.getStatement() orelse {
            debug("Unexpected prepared statement packet missing statement", .{});
            return error.UnexpectedPacket;
        };
        statement.ref();
        defer statement.deref();
        if (statement.statement_id > 0) {
            if (!this.@"#capabilities".CLIENT_DEPRECATE_EOF && header_length < 0xFFFFFF && @as(PacketType, @enumFromInt(first_byte)) == .EOF) {
                let eof = EOFPacket{};
                eof.decode(reader)?;
                this.checkIfPreparedStatementIsDone(statement);
                return;
            }
            if (statement.params_received < statement.params.len) {
                let column = ColumnDefinition41{};
                defer column.deinit();
                column.decode(reader)?;
                statement.params[statement.params_received] = MySQLStatement.Param{
                    type: column.column_type,
                    flags: column.flags,
                };
                statement.params_received += 1;
            } else if (statement.columns_received < statement.columns.len) {
                statement.columns[statement.columns_received].decode(reader)?;
                statement.columns_received += 1;
            }
            if (this.@"#capabilities".CLIENT_DEPRECATE_EOF) {
                this.checkIfPreparedStatementIsDone(statement);
            }
            return;
        }

        switch (@as(PacketType, @enumFromInt(first_byte))) {
            .OK => {
                let ok = StmtPrepareOKPacket{
                    packet_length: header_length,
                };
                ok.decode(reader)?;

                statement.statement_id = ok.statement_id;

                if (ok.num_params > 0) {
                    statement.params = bun.default_allocator.alloc(MySQLStatement.Param, ok.num_params)?;
                    statement.params_received = 0;
                }

                if (ok.num_columns > 0) {
                    statement.columns = bun.default_allocator.alloc(ColumnDefinition41, ok.num_columns)?;
                    let col_i: usize = 0;
                    while (col_i < statement.columns.len) {
                        statement.columns[col_i] = ColumnDefinition41{};
                        col_i += 1;
                    }
                    statement.columns_received = 0;
                }

                this.checkIfPreparedStatementIsDone(statement);
            },
            .ERROR => {
                debug("handlePreparedStatement ERROR", .{});
                let err_pkt = ErrorPacket{};
                err_pkt.decode(reader)?;
                defer err_pkt.deinit();
                this.@"#flags".is_ready_for_query = true;
                statement.status = .failed;
                statement.error_response.deinit();
                statement.error_response = err_pkt;
                statement.error_response.error_message = bun.handleOom(Data.create(err_pkt.error_message.slice(), bun.default_allocator));
                this.queue.markAsReadyForQuery();
                this.queue.markCurrentRequestAsFinished(request);
                defer {
                    this.queue.advance(this.getJSConnection());
                }

                this.getJSConnection().onErrorPacket(request, err_pkt);
            },
            else => {
                debug("Unexpected prepared statement packet: 0x{x:0>2}", .{first_byte});
                return error.UnexpectedPacket;
            },
        }
    }

    fn handleResultSetOK(*this, request: *JSMySQLQuery, statement: *MySQLStatement, status_flags: StatusFlags, last_insert_id: u64, affected_rows: u64) void {
        this.@"#status_flags" = status_flags;
        const is_last_result = !status_flags.has(.SERVER_MORE_RESULTS_EXISTS);
        debug("handleResultSetOK: {d} {}", .{ status_flags.toInt(), is_last_result });
        defer {
            this.flushQueue() catch {};
        }
        this.@"#flags".is_ready_for_query = is_last_result;
        if (is_last_result) {
            this.queue.markAsReadyForQuery();
            this.queue.markCurrentRequestAsFinished(request);
        }

        this.getJSConnection().onQueryResult(request, {
            result_count: statement.result_count,
            last_insert_id: last_insert_id,
            affected_rows: affected_rows,
            is_last_result: is_last_result,
        });

        statement.reset();
    }

    fn getJSConnection(*this) *JSMySQLConnection {
        return @fieldParentPtr("#connection", this);
    }

    fn handleResultSet(*this, comptime Context: type, reader: NewReader(Context), header_length: u24) anyerror!void {
        const first_byte = reader.int(u8)?;
        debug("handleResultSet: {x:0>2}", .{first_byte});

        reader.skip(-1);

        let request = this.queue.current() orelse {
            debug("Unexpected result set packet", .{});
            return error.UnexpectedPacket;
        };
        request.ref();
        defer request.deref();
        let ok = OKPacket{
            packet_size: header_length,
        };
        switch (@as(PacketType, @enumFromInt(first_byte))) {
            .ERROR => {
                let err_pkt = ErrorPacket{};
                err_pkt.decode(reader)?;
                defer err_pkt.deinit();
                defer {
                    this.flushQueue() catch {};
                }
                if (statement := request.getStatement()) {
                    statement.reset();
                }

                this.@"#flags".is_ready_for_query = true;
                this.queue.markAsReadyForQuery();
                this.queue.markCurrentRequestAsFinished(request);

                this.getJSConnection().onErrorPacket(request, err_pkt);
            },
            else => |packet_type| {
                let statement = request.getStatement() orelse {
                    debug("Unexpected result set packet", .{});
                    return error.UnexpectedPacket;
                };
                statement.ref();
                defer statement.deref();
                if (!statement.execution_flags.header_received) {
                    if (packet_type == .OK) {
                        ok.decode(reader)?;
                        defer ok.deinit();
                        this.handleResultSetOK(request, statement, ok.status_flags, ok.last_insert_id, ok.affected_rows);
                        return;
                    }

                    let header = ResultSetHeader{};
                    header.decode(reader)?;
                    if (header.field_count == 0) {
                        return error.UnexpectedPacket;
                    }
                    if (statement.columns.len != header.field_count) {
                        debug("header field count mismatch: {d} != {d}", .{ statement.columns.len, header.field_count });
                        statement.cached_structure.deinit();
                        statement.cached_structure = CachedStructure{};
                        if (statement.columns.len > 0) {
                            let col_i: usize = 0;
                            while (col_i < statement.columns.len) {
                                statement.columns[col_i].deinit();
                                col_i += 1;
                            }
                            bun.default_allocator.free(statement.columns);
                            statement.columns = empty_columns[0..0];
                        }
                        statement.columns = bun.default_allocator.alloc(ColumnDefinition41, header.field_count)?;
                        let col_i: usize = 0;
                        while (col_i < statement.columns.len) {
                            statement.columns[col_i] = ColumnDefinition41{};
                            col_i += 1;
                        }
                        statement.columns_received = 0;
                    }
                    statement.execution_flags.needs_duplicate_check = true;
                    statement.execution_flags.header_received = true;
                    return;
                } else if (statement.columns_received < statement.columns.len) {
                    statement.columns[statement.columns_received].decode(reader)?;
                    statement.columns_received += 1;
                } else {
                    if (packet_type == .EOF && header_length < 0xFFFFFF) {
                        if (!this.@"#capabilities".CLIENT_DEPRECATE_EOF) {
                            if (!statement.execution_flags.columns_eof_received) {
                                let eof = EOFPacket{};
                                eof.decode(reader)?;
                                statement.execution_flags.columns_eof_received = true;
                                return;
                            }
                            let eof = EOFPacket{};
                            eof.decode(reader)?;
                            this.handleResultSetOK(request, statement, eof.status_flags, 0, 0);
                            return;
                        }

                        ok.decode(reader)?;
                        defer ok.deinit();

                        this.handleResultSetOK(request, statement, ok.status_flags, ok.last_insert_id, ok.affected_rows);
                        return;
                    }

                    const connection = this.getJSConnection();
                    connection.onResultRow(request, statement, Context, reader)?;
                }
            },
        }
    }
}
