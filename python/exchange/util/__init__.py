import re
import struct
from enum import IntEnum
from pathlib import Path
from dataclasses import dataclass

class Side(IntEnum):
    SELL = 0
    BUY = 1

class Lifespan(IntEnum):
    FILL_AND_KILL = 0
    GOOD_FOR_DAY = 1

class MessageType(IntEnum):
    CONNECT = 1
    DISCONNECT = 2
    INSERT_ORDER = 3
    CANCEL_ORDER = 4
    AMEND_ORDER = 5
    SUBSCRIBE = 6
    UNSUBSCRIBE = 7
    ORDER_STATUS_REQUEST = 8

    CONFIRM_CONNECTED = 11
    CONFIRM_ORDER_INSERTED = 12
    CONFIRM_ORDER_CANCELLED = 13
    CONFIRM_ORDER_AMENDED = 14
    PARTIAL_FILL_ORDER = 15
    ORDER_STATUS = 16
    ERROR_MSG = 17

    ORDER_BOOK_SNAPSHOT = 21
    TRADE_TICKS = 22
    TRADE_EVENT = 23
    ORDER_INSERTED_EVENT = 24
    ORDER_CANCELLED_EVENT = 25
    ORDER_AMENDED_EVENT = 26
    PRICE_LEVEL_UPDATE = 27

# ----------------------------
# C++ → struct type mapping
# ----------------------------

CPP_TO_STRUCT = {
    "uint8_t": "B",
    "uint16_t": "H",
    "uint32_t": "I",
    "uint64_t": "Q",
    "int64_t": "q",
    "char": "s", # technically wrong

    # typedefs from types.hpp
    "Id_t": "I",
    "Price_t": "q",
    "Volume_t": "I",
    "Time_t": "Q",
    "Seq_t": "Q",
    "Message_t": "B",
    "MessageType": "B",

    # enums
    "Side": "B",
    "Lifespan": "B",
}

HEADER_STRUCT = struct.Struct("<BH")  # message_type (uint8), payload_size (uint16)

@dataclass(frozen=True)
class PayloadSchema:
    struct: struct.Struct
    field_names: list[str]

# ----------------------------
# Mapping message type to payload struct
# ----------------------------

MESSAGE_TO_PAYLOAD = {
    "DISCONNECT": "PayloadDisconnect",
    "INSERT_ORDER": "PayloadInsertOrder",
    "CANCEL_ORDER": "PayloadCancelOrder",
    "AMEND_ORDER": "PayloadAmendOrder",
    "SUBSCRIBE": "PayloadSubscribe",
    "UNSUBSCRIBE": "PayloadUnsubscribe",
    "ORDER_STATUS_REQUEST": "PayloadOrderStatusRequest",

    "CONFIRM_ORDER_INSERTED": "PayloadConfirmOrderInserted",
    "CONFIRM_ORDER_CANCELLED": "PayloadConfirmOrderCancelled",
    "CONFIRM_ORDER_AMENDED": "PayloadConfirmOrderAmended",
    "PARTIAL_FILL_ORDER": "PayloadPartialFill",
    "ORDER_STATUS": "PayloadOrderStatus",
    "ERROR_MSG": "PayloadError",

    "ORDER_BOOK_SNAPSHOT": "PayloadOrderBookSnapshot",
    "TRADE_TICKS": "PayloadTradeTicks",
    "TRADE_EVENT": "PayloadTradeEvent",
    "ORDER_INSERTED_EVENT": "PayloadOrderInsertedEvent",
    "ORDER_CANCELLED_EVENT": "PayloadOrderCancelledEvent",
    "ORDER_AMENDED_EVENT": "PayloadOrderAmendedEvent",
    "PRICE_LEVEL_UPDATE": "PayloadPriceLevelUpdate",
}

def parse_message_types(protocol_hpp: str) -> dict[int, str]:
    """
    Parses enum class MessageType : Message_t { ... }
    Returns: { numeric_value: "ENUM_NAME" }
    """
    enum_re = re.compile(
        r"enum\s+class\s+MessageType\s*:\s*\w+\s*\{([^}]*)\}",
        re.S,
    )

    m = enum_re.search(protocol_hpp)
    if not m:
        raise ValueError("MessageType enum not found")

    body = m.group(1)

    value = 0
    mapping: dict[int, str] = {}

    for entry in body.split(","):
        entry = entry.strip()
        if not entry:
            continue

        if "=" in entry:
            name, val = entry.split("=")
            value = int(val.strip())
        else:
            name = entry

        mapping[value] = name.strip()
        value += 1

    return mapping



# ----------------------------
# Struct parser
# ----------------------------

def decode_c_string(b: bytes, encoding="ascii") -> str:
    return b.rstrip(b"\x00").decode(encoding, errors="ignore")

STRUCT_DEF_RE = re.compile(r"struct\s+(\w+)\s*\{([^}]*)\};", re.S)
ARRAY_RE = re.compile(r"std::array<\s*(\w+)\s*,\s*(\w+)\s*>")
FIELD_RE = re.compile(
    r"(\w+)\s+(\w+)(?:\[(\w+)\])?;"
)


def parse_constants(types_hpp: str) -> dict[str, int]:
    """
    Parses constexpr constants of any integral type
    (size_t, Price_t, uint32_t, etc.).
    """
    constants = {}

    constexpr_re = re.compile(
        r"constexpr\s+(\w+)\s+(\w+)\s*=\s*([^;]+);"
    )

    for ctype, name, value in constexpr_re.findall(types_hpp):
        # Remove digit separators (1'000 → 1000)
        value = value.replace("'", "")

        # Evaluate simple integer expressions
        try:
            constants[name] = int(eval(value, {}, constants))
        except Exception as e:
            raise ValueError(
                f"Failed to evaluate constexpr {ctype} {name} = {value}"
            ) from e

    return constants



def parse_structs(protocol_hpp: str, constants: dict[str, int]) -> dict[str, PayloadSchema]:
    schemas: dict[str, PayloadSchema] = {}

    for struct_name, body in STRUCT_DEF_RE.findall(protocol_hpp):
        fmt = "<"
        field_names: list[str] = []

        for line in body.splitlines():
            line = line.strip()
            if not line:
                continue

            # std::array<T, N>
            arr = ARRAY_RE.search(line)
            if arr:
                ctype, count = arr.groups()
                count = constants.get(count)
                if count is None:
                    raise ValueError(f"count not found in {constants}")
                fmt += CPP_TO_STRUCT[ctype] * count

                base_name = line.split(">")[-1].split(";")[0].strip()
                for i in range(count):
                    field_names.append(f"{base_name}[{i}]")
                continue

            # normal field
            m = FIELD_RE.match(line)
            if not m:
                continue

            ctype, name, arr_count = m.groups()
            if arr_count:
                try:
                    arr_count = int(arr_count)
                except ValueError:
                    arr_count = constants[arr_count]
                if ctype == "char":
                    fmt += f"{arr_count}{CPP_TO_STRUCT[ctype]}"
                    field_names.append(name)
                else:
                    for i in range(int(arr_count)):
                        fmt += CPP_TO_STRUCT[ctype]
                        field_names.append(f"{name}[{i}]")
            else:
                fmt += CPP_TO_STRUCT[ctype]
                field_names.append(name)

        schemas[struct_name] = PayloadSchema(
            struct=struct.Struct(fmt),
            field_names=field_names,
        )

    return schemas


class ProtocolCodec:
    def __init__(self, types_path: Path, protocol_path: Path):
        types_text = types_path.read_text()
        protocol_text = protocol_path.read_text()

        self.constants = parse_constants(types_text)
        self.payload_schemas = parse_structs(protocol_text, self.constants)

        self.message_types = parse_message_types(protocol_text)
        self.message_name_to_id = {
            name: mid for mid, name in self.message_types.items()
        }


    def encode(self, message_type: MessageType, *args, **fields) -> bytes:
        payload_name = MESSAGE_TO_PAYLOAD[message_type.name]
        schema = self.payload_schemas[payload_name]

        if len(fields) == 0:
            values = args
        else:
            values = []
            for fname in schema.field_names:
                if fname not in fields:
                    raise ValueError(f"Missing field '{fname}' for {payload_name}")
                values.append(fields[fname])

        payload = schema.struct.pack(*values)

        header = HEADER_STRUCT.pack(message_type, len(payload))
        return header + payload

    def decode_from_header(self, message_type: int, payload_size: int, payload_bytes: bytes) -> dict:
        message_name = self.message_types.get(message_type)
        if message_name is None:
            raise ValueError(f"Unknown message type {message_type}")

        payload_name = MESSAGE_TO_PAYLOAD.get(message_name)
        if payload_name is None:
            return {
                "message_type": message_name,
                "payload": None,
            }

        schema = self.payload_schemas[payload_name]

        if schema.struct.size != payload_size:
            raise ValueError(
                f"Payload size mismatch for {message_name}: "
                f"{schema.struct.size} != {payload_size}"
            )

        values = schema.struct.unpack(payload_bytes)

        payload = dict(zip(schema.field_names, values))

        return {
            "message_type": message_name,
            "payload": payload,
        }


    def decode(self, data: bytes) -> dict:
        msg_type, payload_size = HEADER_STRUCT.unpack_from(data, 0)
        payload_bytes = data[HEADER_STRUCT.size:HEADER_STRUCT.size + payload_size]
        return self.decode_from_header(message_type=msg_type, payload_size=payload_size, payload_bytes=payload_bytes)

    
def get_codec() -> ProtocolCodec:
    return ProtocolCodec(Path("src/types.hpp"), Path("src/protocol.hpp"))

if __name__ == "__main__":
    codec = get_codec()
    fields = {
        "client_request_id": 123,
        "side": Side.BUY,
        "price": 1000,
        "quantity": 10,
        "lifespan": Lifespan.GOOD_FOR_DAY
    }
    msg = codec.encode(
        MessageType.INSERT_ORDER,
        *list(fields.values())
    )
    print("Encoded bytes:", msg)

    # Decode header
    decoded = codec.decode(msg)
    print("Decoded message:", decoded)

    # --- example data ---
    ASK_PRICES  = [1010, 1011, 1012, 1013, 1014,
                1015, 1016, 1017, 1018, 1019]

    ASK_VOLUMES = [10, 9, 8, 7, 6,
                5, 4, 3, 2, 1]

    BID_PRICES  = [1009, 1008, 1007, 1006, 1005,
                1004, 1003, 1002, 1001, 1000]

    BID_VOLUMES = [1, 2, 3, 4, 5,
                6, 7, 8, 9, 10]

    SEQ = 42


    fields = {}

    for i, v in enumerate(ASK_PRICES):
        fields[f"ask_prices[{i}]"] = v

    for i, v in enumerate(ASK_VOLUMES):
        fields[f"ask_volumes[{i}]"] = v

    for i, v in enumerate(BID_PRICES):
        fields[f"bid_prices[{i}]"] = v

    for i, v in enumerate(BID_VOLUMES):
        fields[f"bid_volumes[{i}]"] = v

    fields["sequence_number"] = SEQ
    encoded = codec.encode(
        message_type=MessageType.ORDER_BOOK_SNAPSHOT,
        **fields
    )

    decoded = codec.decode(encoded)
    print(decoded)