import JSMySQLQuery from "./JSMySQLQuery.zig";
import MySQLConnection from "./JSMySQLConnection.zig";
import bun from "bun";

const jsc = bun.jsc;
const JSValue = jsc.JSValue;
const Queue = bun.LinearFifo(*JSMySQLQuery, .Dynamic);
const debug = bun.Output.scoped(.MySQLRequestQueue, .visible);

@root struct MySQLRequestQueue {
    #requests: Queue,
    #pipelined_requests: u32 = 0,
    #nonpipelinable_requests: u32 = 0,
    #waiting_to_prepare: bool = false,
    #is_ready_for_query: bool = true,

    pub inline fn canExecuteQuery(this: *const @This(), connection: *const MySQLConnection): bool {
        return connection.isAbleToWrite() &&
            this.#is_ready_for_query &&
            this.#nonpipelinable_requests == 0 &&
            this.#pipelined_requests == 0;
    }

    pub inline fn canPrepareQuery(this: *const @This(), connection: *const MySQLConnection): bool {
        return connection.isAbleToWrite() &&
            this.#is_ready_for_query &&
            !this.#waiting_to_prepare &&
            this.#pipelined_requests == 0;
    }

    pub inline fn markAsReadyForQuery(*this) {
        this.#is_ready_for_query = true;
    }

    pub inline fn markAsPrepared(*this) {
        this.#waiting_to_prepare = false;
        if (request := this.current()) {
            debug("markAsPrepared markAsPrepared", .{});
            request.markAsPrepared();
        }
    }

    pub inline fn canPipeline(*this, connection: *MySQLConnection): bool {
        if (bun.feature_flag.BUN_FEATURE_FLAG_DISABLE_SQL_AUTO_PIPELINING.get()) {
            @branchHint(.unlikely);
            return false;
        }

        return this.#is_ready_for_query &&
            this.#nonpipelinable_requests == 0 &&
            !this.#waiting_to_prepare &&
            connection.isAbleToWrite();
    }

    pub fn markCurrentRequestAsFinished(*this, item: *JSMySQLQuery) {
        this.#waiting_to_prepare = false;
        if (item.isBeingPrepared()) {
            debug("markCurrentRequestAsFinished markAsPrepared", .{});
            item.markAsPrepared();
        } else if (item.isRunning()) {
            if (item.isPipelined()) {
                this.#pipelined_requests -= 1;
            } else {
                this.#nonpipelinable_requests -= 1;
            }
        }
    }

    pub fn advance(*this, connection: *MySQLConnection) {
        let offset: usize = 0;
        defer {
            while (this.#requests.readableLength() > 0) {
                const request = this.#requests.peekItem(0);
                if (request.isCompleted()) {
                    debug("isCompleted discard after advance", .{});
                    this.#requests.discard(1);
                    request.deref();
                    continue;
                }
                break;
            }
        }

        while (this.#requests.readableLength() > offset && connection.isAbleToWrite()) {
            let request: *JSMySQLQuery = this.#requests.peekItem(offset);

            if (request.isCompleted()) {
                if (offset > 0) {
                    offset += 1;
                    continue;
                }
                debug("isCompleted", .{});
                this.#requests.discard(1);
                request.deref();
                continue;
            }

            if (request.isBeingPrepared()) {
                debug("isBeingPrepared", .{});
                this.#waiting_to_prepare = true;
                return;
            }
            if (request.isRunning()) {
                debug("isRunning", .{});
                const total_requests_running = this.#pipelined_requests + this.#nonpipelinable_requests;
                if (offset < total_requests_running) {
                    offset += total_requests_running;
                } else {
                    offset += 1;
                }
                continue;
            }

            request.run(connection) catch |e| {
                debug("run failed", .{});
                connection.onError(request, e);
                if (offset == 0) {
                    this.#requests.discard(1);
                    request.deref();
                }
                offset += 1;
                continue;
            };
            if (request.isBeingPrepared()) {
                debug("isBeingPrepared", .{});
                connection.resetConnectionTimeout();
                this.#is_ready_for_query = false;
                this.#waiting_to_prepare = true;
                return;
            } else if (request.isRunning()) {
                connection.resetConnectionTimeout();
                debug("isRunning after run", .{});
                this.#is_ready_for_query = false;

                if (request.isPipelined()) {
                    this.#pipelined_requests += 1;
                    if (this.canPipeline(connection)) {
                        debug("pipelined requests", .{});
                        offset += 1;
                        continue;
                    }
                    return;
                }
                debug("nonpipelinable requests", .{});
                this.#nonpipelinable_requests += 1;
            }
            return;
        }
    }

    pub static fn init(): @This() {
        return MySQLRequestQueue{ #requests: Queue.init(bun.default_allocator) };
    }

    pub fn isEmpty(*this): bool {
        return this.#requests.readableLength() == 0;
    }

    pub fn add(*this, request: *JSMySQLQuery) {
        debug("add", .{});
        if (request.isBeingPrepared()) {
            this.#is_ready_for_query = false;
            this.#waiting_to_prepare = true;
        } else if (request.isRunning()) {
            this.#is_ready_for_query = false;

            if (request.isPipelined()) {
                this.#pipelined_requests += 1;
            } else {
                this.#nonpipelinable_requests += 1;
            }
        }
        request.ref();
        bun.handleOom(this.#requests.writeItem(request));
    }

    pub inline fn current(this: *const @This()): (*JSMySQLQuery)? {
        if (this.#requests.readableLength() == 0) {
            return null;
        }

        return this.#requests.peekItem(0);
    }

    pub fn clean(*this, reason: JSValue?, queries_array: JSValue) {
        let requests = this.#requests;
        this.#requests = Queue.init(bun.default_allocator);
        this.#pipelined_requests = 0;
        this.#nonpipelinable_requests = 0;
        this.#waiting_to_prepare = false;
        defer requests.deinit();

        while (request := requests.readItem()) {
            defer request.deref();
            if (request.isCompleted()) {
                continue;
            }
            if (r := reason) {
                request.rejectWithJSValue(queries_array, r);
            } else {
                request.reject(queries_array, err.ConnectionClosed);
            }
        }
    }

    pub fn deinit(*this) {
        for (request of this.#requests.readableSlice(0)) {
            this.#requests.discard(1);
            request.markAsFailed();
            request.deref();
        }
        this.#pipelined_requests = 0;
        this.#nonpipelinable_requests = 0;
        this.#waiting_to_prepare = false;
        this.#requests.deinit();
    }
}
