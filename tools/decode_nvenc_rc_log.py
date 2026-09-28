"""Read-only decoder for RC_GET_ERROR_V2 hex already present in a boot log.

No driver loading, GPU access, boot, or test execution. The envelope is
rmcd.h/RmProtoBuf_RECORD, containing Dcl.ErrorBlock.data messages. Dcl tag
312 -> Rc.GenericData was verified from the supplied 595.99.02 proprietary
nv-kernel.o_binary descriptor relocations; its fields are in the matching
open driver's generated g_rc_pb.c. Dcl 302 field 5 contains Regs.RegsAndMem,
verified through the same binary's descriptor relocations. Other Dcl contents
remain explicitly unknown; register snapshots are not encoder failure codes.
An engine-wide RC type (e.g. 65) is NOT a precise firmware failure reason.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


class DecodeError(ValueError):
    pass


def varint(data, offset):
    value = 0
    for index in range(10):
        if offset >= len(data):
            raise DecodeError("truncated varint")
        byte = data[offset]
        offset += 1
        if index == 9 and byte > 1:
            raise DecodeError("varint exceeds uint64")
        value |= (byte & 127) << (index * 7)
        if not byte & 128:
            return value, offset
    raise DecodeError("unterminated varint")


def fields(data):
    offset = 0
    result = []
    while offset < len(data):
        key, offset = varint(data, offset)
        number, wire = key >> 3, key & 7
        if not 0 < number < (1 << 29):
            raise DecodeError("invalid protobuf field number")
        if wire == 0:
            value, offset = varint(data, offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                length, offset = varint(data, offset)
            else:
                length = 8 if wire == 1 else 4
            if length > len(data) - offset:
                raise DecodeError("truncated protobuf field")
            value = data[offset:offset + length]
            offset += length
        else:
            raise DecodeError("unsupported protobuf wire type")
        result.append((number, wire, value))
    return result


GENERIC_NAMES = {
    1: "nv_agpconf_cmd", 2: "nb_agpconf_cmd", 3: "error_context",
    4: "channel_id", 5: "error_type", 6: "pushbuffer_space",
    7: "time", 8: "gpu_id", 9: "error_number", 10: "system_time",
}


def wire_json(message):
    return [{"field": number, "wire": wire,
             "value": value.hex() if isinstance(value, bytes) else str(value)}
            for number, wire, value in fields(message)]


def decode_register_snapshot(message):
    """Decode the verified Regs.RegsAndMem schema, including packed varints."""
    scalars, values = {}, []
    for number, wire, value in fields(message):
        if number in (1, 2, 3):
            if wire != 0 or number in scalars:
                raise DecodeError("invalid/duplicate register metadata")
            if number != 2 and value > 0xffffffff:
                raise DecodeError("register metadata uint32 overflow")
            scalars[number] = value
        elif number == 4:
            if wire == 0:  # protobuf accepts unpacked occurrences too
                packed = [value]
            elif wire == 2:
                packed, offset = [], 0
                while offset < len(value):
                    item, offset = varint(value, offset)
                    packed.append(item)
            else:
                raise DecodeError("invalid register value wire type")
            if any(item > 0xffffffff for item in packed):
                raise DecodeError("register value uint32 overflow")
            values.extend(packed)
        else:
            raise DecodeError("unknown Regs.RegsAndMem field")
    if 1 not in scalars:
        raise DecodeError("register snapshot lacks required memory type")
    base, stride = scalars.get(2, 0), scalars.get(3, 4)
    if values and base + (len(values) - 1) * stride > 0xffffffffffffffff:
        raise DecodeError("register snapshot address overflow")
    return {"schema": "Regs.RegsAndMem", "memory_type": scalars[1],
            "offset": hex(base), "stride": stride,
            "values": [{"offset": hex(base + i * stride), "value": hex(v)}
                       for i, v in enumerate(values)]}


def decode_record(data):
    if not 8 <= len(data) <= 8192:
        raise DecodeError("record outside RC_GET_ERROR_V2 bounds")
    group, kind, header, payload = struct.unpack_from("<BBHI", data)
    if (group, kind, header, payload) != (1, 131, 8, len(data) - 8):
        raise DecodeError("unrecognized or incomplete RmProtoBuf envelope")
    decoded = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
               "generic_rc": [], "other_dcl": [], "register_snapshots": []}
    for number, wire, block in fields(data[8:]):
        if number != 1 or wire != 2:
            raise DecodeError("unexpected Dcl.ErrorBlock field")
        for tag, tag_wire, message in fields(block):
            if tag_wire != 2:
                raise DecodeError("Dcl message is not length-delimited")
            if tag != 312:
                if tag == 302:
                    for field, field_wire, payload in fields(message):
                        if field == 5:
                            if field_wire != 2:
                                raise DecodeError("Dcl 302 register field is not a message")
                            snapshot = decode_register_snapshot(payload)
                            snapshot["source"] = "Dcl tag 302 field 5"
                            decoded["register_snapshots"].append(snapshot)
                decoded["other_dcl"].append({"tag": tag,
                    "schema": "unknown parent; field 5 register schema verified" if tag == 302 else
                              "unknown; no firmware interpretation",
                    "fields": wire_json(message)})
                continue
            generic = {}
            for field, field_wire, value in fields(message):
                if field not in GENERIC_NAMES:
                    raise DecodeError("unknown Rc.GenericData field")
                name = GENERIC_NAMES[field]
                if field_wire != 0 or name in generic:
                    raise DecodeError("invalid/duplicate Rc.GenericData field")
                if field not in (7, 10) and value > 0xffffffff:
                    raise DecodeError("Rc.GenericData uint32 overflow")
                generic[name] = value
            # Timestamps exceed JavaScript's exact-integer range. Emit strings.
            decoded["generic_rc"].append({name: {
                "decimal": str(value), "hex": hex(value)}
                for name, value in generic.items()})
    return decoded


def decode_log(path):
    if path.stat().st_size > 32 * 1024 * 1024:
        raise DecodeError("log exceeds 32 MiB analysis bound")
    raw = path.read_bytes()
    lines = raw.decode("utf-8", errors="replace").splitlines()
    captures = []
    active = None
    for line_number, line in enumerate(lines, 1):
        head = re.search(r"nvenc-rc\s+global journal record=(\d+).*bytes=(\d+)", line)
        if head:
            active = {"index": int(head[1]), "expected": int(head[2]),
                      "line": line_number, "data": bytearray(), "errors": []}
            captures.append(active)
            if active["expected"] > 8192:
                active["errors"].append("declared record exceeds 8192 bytes")
            continue
        chunk = re.search(r"nvenc-rc\s+record=(\d+) offset=(0x[0-9a-f]+|\d+) "
                          r"bytes=(\d+) hex=([0-9a-f]+)\s*$", line)
        if not chunk:
            continue
        if active is None or active["index"] != int(chunk[1]):
            raise DecodeError(f"orphan journal chunk at log line {line_number}")
        if active["errors"]:
            continue
        offset, length = int(chunk[2], 0), int(chunk[3])
        if offset != len(active["data"]) or not 1 <= length <= 32 or \
                len(chunk[4]) != length * 2 or offset + length > active["expected"]:
            active["errors"].append(f"invalid/noncontiguous chunk at line {line_number}")
            continue
        active["data"].extend(bytes.fromhex(chunk[4]))
    result = {"log": str(path), "log_sha256": hashlib.sha256(raw).hexdigest(),
              "warning": "Global journal; correlate identity and time separately. "
                         "RC error_type is not the precise firmware cause.",
              "records": []}
    for capture in captures:
        entry = {"index": capture["index"], "line": capture["line"]}
        if capture["errors"]:
            entry["errors"] = capture["errors"]
        elif len(capture["data"]) != capture["expected"]:
            entry["errors"] = ["incomplete log capture"]
        else:
            try:
                entry.update(decode_record(bytes(capture["data"])))
            except DecodeError as error:
                entry["errors"] = [str(error)]
        result["records"].append(entry)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    try:
        result = decode_log(args.log)
    except (OSError, DecodeError) as error:
        parser.exit(2, f"Cannot decode journal: {error}\n")
    print(json.dumps(result, indent=2))
    return 1 if any("errors" in record for record in result["records"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
