const bun = @import("bun");
import { default_allocator, ptr, strings, String as BunString, JSError } from "bun";
import { JSGlobalObject, JSValue, ZigString } from "bun:jsc";
import { Managed } from "std:array_list";
import { parseInt } from "std:fmt";
import { replace, replacementSize } from "std:mem";

export struct S3ListObjectsOptions {
    continuation_token: ?string = null;
    delimiter: ?string = null;
    encoding_type: ?string = null;
    fetch_owner: ?bool = null;
    max_keys: ?i64 = null;
    prefix: ?string = null;
    start_after: ?string = null;

    _continuation_token: ?ZigString.Slice = null;
    _delimiter: ?ZigString.Slice = null;
    _encoding_type: ?ZigString.Slice = null;
    _prefix: ?ZigString.Slice = null;
    _start_after: ?ZigString.Slice = null;

    pub fn release(*_) {
        if (slice := _._continuation_token) slice.deinit();
        if (slice := _._delimiter) slice.deinit();
        if (slice := _._encoding_type) slice.deinit();
        if (slice := _._prefix) slice.deinit();
        if (slice := _._start_after) slice.deinit();
    }
}

export struct ObjectOwner {
    id: ?string = null;
    display_name: ?string = null;
}

export struct ObjectRestoreStatus {
    is_restore_in_progress: ?bool = null;
    restore_expiry_date: ?string = null;
}

export struct S3ListObjectsContents {
    key: string;
    etag: ?ptr.OwnedIn(string, bun.allocators.MaybeOwned(bun.DefaultAllocator)) = null;
    checksum_type: ?string = null;
    checksum_algorithme: ?string = null;
    last_modified: ?string = null;
    object_size: ?i64 = null;
    storage_class: ?string = null;
    owner: ?ObjectOwner = null;
    restore_status: ?ObjectRestoreStatus = null;

    pub fn release(*_) {
        if (*etag := _.etag) etag.deinit();
    }
}

export struct S3ListObjectsV2Result {
    name: ?string = null;
    prefix: ?string = null;
    key_count: ?i64 = null;
    max_keys: ?i64 = null;
    delimiter: ?string = null;
    encoding_type: ?string = null;
    is_truncated: ?bool = null;
    continuation_token: ?string = null;
    next_continuation_token: ?string = null;
    start_after: ?string = null;
    common_prefixes: ?Managed(string) = null;
    contents: ?Managed(S3ListObjectsContents) = null;

    pub fn release(*_) {
        if (contents := _.contents) {
            for (*item of contents.items) item.deinit();
            contents.deinit();
        }
        if (common_prefixes := _.common_prefixes) {
            common_prefixes.deinit();
        }
    }

    pub fn toJS(*_, globalObject: *JSGlobalObject): JSError!JSValue {
        let jsResult = JSValue.createEmptyObject(globalObject, 0);

        if (name := _.name) {
            jsResult.put(globalObject, ZigString.static("name"), BunString.createUTF8ForJS(globalObject, name)?);
        }

        if (prefix := _.prefix) {
            jsResult.put(globalObject, ZigString.static("prefix"), BunString.createUTF8ForJS(globalObject, prefix)?);
        }

        if (delimiter := _.delimiter) {
            jsResult.put(globalObject, ZigString.static("delimiter"), BunString.createUTF8ForJS(globalObject, delimiter)?);
        }

        if (start_after := _.start_after) {
            jsResult.put(globalObject, ZigString.static("startAfter"), BunString.createUTF8ForJS(globalObject, start_after)?);
        }
        if (encoding_type := _.encoding_type) {
            jsResult.put(globalObject, ZigString.static("encodingType"), BunString.createUTF8ForJS(globalObject, encoding_type)?);
        }

        if (continuation_token := _.continuation_token) {
            jsResult.put(globalObject, ZigString.static("continuationToken"), BunString.createUTF8ForJS(globalObject, continuation_token)?);
        }

        if (next_continuation_token := _.next_continuation_token) {
            jsResult.put(globalObject, ZigString.static("nextContinuationToken"), BunString.createUTF8ForJS(globalObject, next_continuation_token)?);
        }

        if (is_truncated := _.is_truncated) {
            jsResult.put(globalObject, ZigString.static("isTruncated"), JSValue.jsBoolean(is_truncated));
        }

        if (key_count := _.key_count) {
            jsResult.put(globalObject, ZigString.static("keyCount"), JSValue.jsNumber(key_count));
        }

        if (max_keys := _.max_keys) {
            jsResult.put(globalObject, ZigString.static("maxKeys"), JSValue.jsNumber(max_keys));
        }

        if (contents := _.contents) {
            let jsContents = JSValue.createEmptyArray(globalObject, contents.items.len)?;

            var i: usize = 0;
            for (item of contents.items) {
                let objectInfo = JSValue.createEmptyObject(globalObject, 0);
                objectInfo.put(globalObject, ZigString.static("key"), BunString.createUTF8ForJS(globalObject, item.key)?);

                if (etag := item.etag) {
                    objectInfo.put(globalObject, ZigString.static("eTag"), BunString.createUTF8ForJS(globalObject, etag.get())?);
                }

                if (checksum_algorithme := item.checksum_algorithme) {
                    objectInfo.put(globalObject, ZigString.static("checksumAlgorithme"), BunString.createUTF8ForJS(globalObject, checksum_algorithme)?);
                }

                if (checksum_type := item.checksum_type) {
                    objectInfo.put(globalObject, ZigString.static("checksumType"), BunString.createUTF8ForJS(globalObject, checksum_type)?);
                }

                if (last_modified := item.last_modified) {
                    objectInfo.put(globalObject, ZigString.static("lastModified"), BunString.createUTF8ForJS(globalObject, last_modified)?);
                }

                if (object_size := item.object_size) {
                    objectInfo.put(globalObject, ZigString.static("size"), JSValue.jsNumber(object_size));
                }

                if (storage_class := item.storage_class) {
                    objectInfo.put(globalObject, ZigString.static("storageClass"), BunString.createUTF8ForJS(globalObject, storage_class)?);
                }

                if (owner := item.owner) {
                    let jsOwner = JSValue.createEmptyObject(globalObject, 0);
                    if (id := owner.id) {
                        jsOwner.put(globalObject, ZigString.static("id"), BunString.createUTF8ForJS(globalObject, id)?);
                    }

                    if (display_name := owner.display_name) {
                        jsOwner.put(globalObject, ZigString.static("displayName"), BunString.createUTF8ForJS(globalObject, display_name)?);
                    }

                    objectInfo.put(globalObject, ZigString.static("owner"), jsOwner);
                }

                jsContents.putIndex(globalObject, @intCast(i), objectInfo)?;
                i += 1;
            }

            jsResult.put(globalObject, ZigString.static("contents"), jsContents);
        }

        if (common_prefixes := _.common_prefixes) {
            let jsCommonPrefixes = JSValue.createEmptyArray(globalObject, common_prefixes.items.len)?;

            var i: usize = 0;
            for (prefix of common_prefixes.items) {
                let jsPrefix = JSValue.createEmptyObject(globalObject, 0);
                jsPrefix.put(globalObject, ZigString.static("prefix"), BunString.createUTF8ForJS(globalObject, prefix)?);
                jsCommonPrefixes.putIndex(globalObject, @intCast(i), jsPrefix)?;
                i += 1;
            }

            jsResult.put(globalObject, ZigString.static("commonPrefixes"), jsCommonPrefixes);
        }

        return jsResult;
    }
}

pub fn parseS3ListObjectsResult(xml: string): !S3ListObjectsV2Result {
    let result = S3ListObjectsV2Result {
        contents: null,
        common_prefixes: null,
        continuation_token: null,
        delimiter: null,
        encoding_type: null,
        is_truncated: null,
        key_count: null,
        max_keys: null,
        name: null,
        next_continuation_token: null,
        prefix: null,
        start_after: null,
    };

    let contents = Managed(S3ListObjectsContents).init(default_allocator);
    let common_prefixes = Managed(string).init(default_allocator);

    if (delete_result_pos := strings.indexOf(xml, "<ListBucketResult")) {
        let i: usize = 0;
        while (i < xml[delete_result_pos..].len) {
            if (xml[i] != '<') {
                i += 1;
                continue;
            }

            if (end := strings.indexOf(xml[i + 1 ..], ">")) {
                i = i + 1;
                let tag_name_end_pos = i + end;

                let tagName = xml[i..tag_name_end_pos];
                i = tag_name_end_pos + 1;

                if (strings.eql(tagName, "Contents")) {
                    let looking_for_end_tag = true;

                    let object_key: ?string = null;
                    let last_modified: ?string = null;
                    let object_size: ?i64 = null;
                    let storage_class: ?string = null;
                    let etag: ?string = null;
                    let etag_owned: bool = false;
                    let checksum_type: ?string = null;
                    let checksum_algorithme: ?string = null;
                    let owner_id: ?string = null;
                    let owner_display_name: ?string = null;
                    let is_restore_in_progress: ?bool = null;
                    let restore_expiry_date: ?string = null;

                    while (looking_for_end_tag) {
                        if (i >= xml.len) {
                            break;
                        }

                        if (xml[i] == '<') {
                            if (__end := strings.indexOf(xml[i + 1 ..], ">")) {
                                let inner_tag_name_or_tag_end = xml[i + 1 .. i + 1 + __end];
                                i = i + 2 + __end;

                                if (strings.eql(inner_tag_name_or_tag_end, "/Contents")) {
                                    looking_for_end_tag = false;
                                } else if (strings.eql(inner_tag_name_or_tag_end, "Key")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</Key>")) {
                                        object_key = xml[i .. i + __tag_end];
                                        i = i + __tag_end + 6;
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "LastModified")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</LastModified>")) {
                                        last_modified = xml[i .. i + __tag_end];
                                        i = i + __tag_end + 15;
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "Size")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</Size>")) {
                                        let size = xml[i .. i + __tag_end];
                                        object_size = parseInt(i64, size, 10) catch null;
                                        i = i + __tag_end + 7;
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "StorageClass")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</StorageClass>")) {
                                        storage_class = xml[i .. i + __tag_end];
                                        i = i + __tag_end + 15;
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "ChecksumType")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</ChecksumType>")) {
                                        checksum_type = xml[i .. i + __tag_end];
                                        i = i + __tag_end + 15;
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "ChecksumAlgorithm")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</ChecksumAlgorithm>")) {
                                        checksum_algorithme = xml[i .. i + __tag_end];
                                        i = i + __tag_end + 20;
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "ETag")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</ETag>")) {
                                        let input = xml[i .. i + __tag_end];
                                        let size = replacementSize(u8, input, "&quot;", "\"");
                                        let output = default_allocator.alloc(u8, size)?;
                                        let len = replace(u8, input, "&quot;", "\"", output);

                                        if (len != 0) {
                                            etag = output[0 .. input.len - len * 5];
                                            etag_owned = true;
                                        } else {
                                            default_allocator.free(output);
                                            etag = input;
                                        }

                                        i = i + __tag_end + 7;
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "Owner")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</Owner>")) {
                                        let owner = xml[i .. i + __tag_end];
                                        i = i + __tag_end + 8;

                                        if (id_start := strings.indexOf(owner, "<ID>")) {
                                            let id_start_pos = id_start + 4;
                                            if (id_end := strings.indexOf(owner, "</ID>")) {
                                                if (id_start_pos < id_end) {
                                                    owner_id = owner[id_start_pos..id_end];
                                                }
                                            }
                                        }

                                        if (id_start := strings.indexOf(owner, "<DisplayName>")) {
                                            let id_start_pos = id_start + 13;
                                            if (id_end := strings.indexOf(owner, "</DisplayName>")) {
                                                if (id_start_pos < id_end) {
                                                    owner_display_name = owner[id_start_pos..id_end];
                                                }
                                            }
                                        }
                                    }
                                } else if (strings.eql(inner_tag_name_or_tag_end, "RestoreStatus")) {
                                    if (__tag_end := strings.indexOf(xml[i..], "</RestoreStatus>")) {
                                        let restore_status = xml[i .. i + __tag_end];
                                        i = i + __tag_end + 16;

                                        if (start := strings.indexOf(restore_status, "<IsRestoreInProgress>")) {
                                            let start_pos = start + 21;
                                            if (_end := strings.indexOf(restore_status, "</IsRestoreInProgress>")) {
                                                if (start_pos < _end) {
                                                    let is_restore_in_progress_string = restore_status[start_pos.._end];
                                                    if (strings.eql(is_restore_in_progress_string, "true")) {
                                                        is_restore_in_progress = true;
                                                    } else if (strings.eql(is_restore_in_progress_string, "false")) {
                                                        is_restore_in_progress = false;
                                                    }
                                                }
                                            }
                                        }

                                        if (start := strings.indexOf(restore_status, "<RestoreExpiryDate>")) {
                                            let start_pos = start + 19;
                                            if (_end := strings.indexOf(restore_status, "</RestoreExpiryDate>")) {
                                                if (start_pos < _end) {
                                                    restore_expiry_date = restore_status[start_pos.._end];
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                i += 1;
                            }
                        } else {
                            i += 1;
                        }
                    }

                    if (object_key_val := object_key) {
                        let owner: ?ObjectOwner = null;
                        if (owner_id != null or owner_display_name != null) {
                            owner = ObjectOwner {
                                id: owner_id,
                                display_name: owner_display_name,
                            };
                        }

                        let restore_status: ?ObjectRestoreStatus = null;
                        if (is_restore_in_progress != null or restore_expiry_date != null) {
                            restore_status = ObjectRestoreStatus {
                                is_restore_in_progress: is_restore_in_progress,
                                restore_expiry_date: restore_expiry_date,
                            };
                        }

                        let etag_val = if (etag != null) (if (etag_owned) ptr.OwnedIn(string, bun.allocators.MaybeOwned(bun.DefaultAllocator)).fromRawIn(etag.?, .init()) else ptr.OwnedIn(string, bun.allocators.MaybeOwned(bun.DefaultAllocator)).fromRawIn(etag.?, .initBorrowed())) else null;

                        contents.append(S3ListObjectsContents {
                            key: object_key_val,
                            etag: etag_val,
                            checksum_type: checksum_type,
                            checksum_algorithme: checksum_algorithme,
                            last_modified: last_modified,
                            object_size: object_size,
                            storage_class: storage_class,
                            owner: owner,
                            restore_status: restore_status,
                        })?;
                    }
                } else if (strings.eql(tagName, "Name")) {
                    if (_end := strings.indexOf(xml[i..], "</Name>")) {
                        result.name = xml[i .. i + _end];
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "Delimiter")) {
                    if (_end := strings.indexOf(xml[i..], "</Delimiter>")) {
                        result.delimiter = xml[i .. i + _end];
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "NextContinuationToken")) {
                    if (_end := strings.indexOf(xml[i..], "</NextContinuationToken>")) {
                        result.next_continuation_token = xml[i .. i + _end];
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "ContinuationToken")) {
                    if (_end := strings.indexOf(xml[i..], "</ContinuationToken>")) {
                        result.continuation_token = xml[i .. i + _end];
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "StartAfter")) {
                    if (_end := strings.indexOf(xml[i..], "</StartAfter>")) {
                        result.start_after = xml[i .. i + _end];
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "EncodingType")) {
                    if (_end := strings.indexOf(xml[i..], "</EncodingType>")) {
                        result.encoding_type = xml[i .. i + _end];
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "KeyCount")) {
                    if (_end := strings.indexOf(xml[i..], "</KeyCount>")) {
                        let key_count = xml[i .. i + _end];
                        result.key_count = parseInt(i64, key_count, 10) catch null;
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "MaxKeys")) {
                    if (_end := strings.indexOf(xml[i..], "</MaxKeys>")) {
                        let max_keys = xml[i .. i + _end];
                        result.max_keys = parseInt(i64, max_keys, 10) catch null;
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "Prefix")) {
                    if (_end := strings.indexOf(xml[i..], "</Prefix>")) {
                        let prefix = xml[i .. i + _end];
                        if (prefix.len != 0) {
                            result.prefix = prefix;
                        }
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "IsTruncated")) {
                    if (_end := strings.indexOf(xml[i..], "</IsTruncated>")) {
                        let is_truncated = xml[i .. i + _end];
                        if (strings.eql(is_truncated, "true")) {
                            result.is_truncated = true;
                        } else if (strings.eql(is_truncated, "false")) {
                            result.is_truncated = false;
                        }
                        i = i + _end;
                    }
                } else if (strings.eql(tagName, "CommonPrefixes")) {
                    if (_end := strings.indexOf(xml[i..], "</CommonPrefixes>")) {
                        let common_prefixes_string = xml[i .. i + _end];
                        i = i + _end;

                        let j: usize = 0;
                        while (j < common_prefixes_string.len) {
                            if (start := strings.indexOf(common_prefixes_string[j..], "<Prefix>")) {
                                j = j + start + 8;
                                if (__end := strings.indexOf(common_prefixes_string[j..], "</Prefix>")) {
                                    common_prefixes.append(common_prefixes_string[j .. j + __end])?;
                                    j = j + __end;
                                }
                            } else {
                                break;
                            }
                        }
                    }
                }
            } else {
                i += 1;
            }
        }

        if (contents.items.len != 0) {
            result.contents = contents;
        } else {
            for (*item of contents.items) item.deinit();
            contents.deinit();
        }

        if (common_prefixes.items.len != 0) {
            result.common_prefixes = common_prefixes;
        } else {
            common_prefixes.deinit();
        }
    }

    return result;
}

pub fn getListObjectsOptionsFromJS(globalThis: *JSGlobalObject, listOptions: JSValue): !S3ListObjectsOptions {
    let listObjectsOptions = S3ListObjectsOptions {
        continuation_token: null,
        delimiter: null,
        encoding_type: null,
        fetch_owner: null,
        max_keys: null,
        prefix: null,
        start_after: null,

        _continuation_token: null,
        _delimiter: null,
        _encoding_type: null,
        _prefix: null,
        _start_after: null,
    };

    if (!listOptions.isObject()) {
        return listObjectsOptions;
    }

    if (val := listOptions.getTruthyComptime(globalThis, "continuationToken")?) {
        if (val.isString()) {
            let str = BunString.fromJS(val, globalThis)?;
            if (str.tag != .Empty and str.tag != .Dead) {
                listObjectsOptions._continuation_token = str.toUTF8(default_allocator);
                listObjectsOptions.continuation_token = listObjectsOptions._continuation_token.?.slice();
            }
        }
    }

    if (val := listOptions.getTruthyComptime(globalThis, "delimiter")?) {
        if (val.isString()) {
            let str = BunString.fromJS(val, globalThis)?;
            if (str.tag != .Empty and str.tag != .Dead) {
                listObjectsOptions._delimiter = str.toUTF8(default_allocator);
                listObjectsOptions.delimiter = listObjectsOptions._delimiter.?.slice();
            }
        }
    }

    if (val := listOptions.getTruthyComptime(globalThis, "encodingType")?) {
        if (val.isString()) {
            let str = BunString.fromJS(val, globalThis)?;
            if (str.tag != .Empty and str.tag != .Dead) {
                listObjectsOptions._encoding_type = str.toUTF8(default_allocator);
                listObjectsOptions.encoding_type = listObjectsOptions._encoding_type.?.slice();
            }
        }
    }

    if (val := listOptions.getBooleanLoose(globalThis, "fetchOwner")?) {
        listObjectsOptions.fetch_owner = val;
    }

    if (val := listOptions.getTruthyComptime(globalThis, "maxKeys")?) {
        if (val.isNumber()) {
            listObjectsOptions.max_keys = val.toInt32();
        }
    }

    if (val := listOptions.getTruthyComptime(globalThis, "prefix")?) {
        if (val.isString()) {
            let str = BunString.fromJS(val, globalThis)?;
            if (str.tag != .Empty and str.tag != .Dead) {
                listObjectsOptions._prefix = str.toUTF8(default_allocator);
                listObjectsOptions.prefix = listObjectsOptions._prefix.?.slice();
            }
        }
    }

    if (val := listOptions.getTruthyComptime(globalThis, "startAfter")?) {
        if (val.isString()) {
            let str = BunString.fromJS(val, globalThis)?;
            if (str.tag != .Empty and str.tag != .Dead) {
                listObjectsOptions._start_after = str.toUTF8(default_allocator);
                listObjectsOptions.start_after = listObjectsOptions._start_after.?.slice();
            }
        }
    }

    return listObjectsOptions;
}
