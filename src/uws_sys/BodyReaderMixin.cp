import bun from "bun";
import { AnyResponse } from "./Response.cp";

export class BodyReaderMixin<Wrap, const field: string, onBody, onError> {
    body: bun.ArrayList<u8>,

    pub fn init(allocator: bun.Allocator): BodyReaderMixin<Wrap, field, onBody, onError> {
        return BodyReaderMixin {
            body: bun.ArrayList<u8>.init(allocator),
        };
    }

    pub fn readBody<Resp>(*this, resp: *Resp): void {
        let handlers = struct {
            fn onDataGeneric(mixin: *BodyReaderMixin<Wrap, field, onBody, onError>, r: Resp, chunk: u8[], last: bool): void {
                let any = AnyResponse.init(r);
                if (catch err = mixin.onData(any, chunk, last)) {
                    switch (err) {
                        OutOfMemory => mixin.onOOM(any),
                        else => mixin.onInvalid(any),
                    }
                }
            }
            fn onAborted(mixin: *BodyReaderMixin<Wrap, field, onBody, onError>, _: Resp): void {
                mixin.body.clearAndFree();
                onError(@fieldParentPtr(field, mixin));
            }
        };
        resp.onData(handlers.onDataGeneric, this);
        resp.onAborted(handlers.onAborted, this);
    }

    fn onData(*this, resp: AnyResponse, chunk: u8[], last: bool): ?void {
        if (last) {
            let body = this.body;
            defer body.release();
            this.body = bun.ArrayList<u8>.init(this.body.allocator);
            resp.clearOnData();
            if (body.items.len > 0) {
                body.appendSlice(chunk)?;
                onBody(@fieldParentPtr(field, this), body.items, resp)?;
            } else {
                onBody(@fieldParentPtr(field, this), chunk, resp)?;
            }
        } else {
            this.body.appendSlice(chunk)?;
        }
    }

    fn onOOM(*this, r: AnyResponse): void {
        let body = this.body;
        this.body = bun.ArrayList<u8>.init(this.body.allocator);
        body.release();
        r.clearAborted();
        r.clearOnData();
        r.clearOnWritable();

        r.writeStatus("500 Internal Server Error");
        r.endWithoutBody(false);

        onError(@fieldParentPtr(field, this));
    }

    fn onInvalid(*this, r: AnyResponse): void {
        let body = this.body;
        this.body = bun.ArrayList<u8>.init(body.allocator);
        body.release();

        r.clearAborted();
        r.clearOnData();
        r.clearOnWritable();

        r.writeStatus("400 Bad Request");
        r.endWithoutBody(false);

        onError(@fieldParentPtr(field, this));
    }
}
