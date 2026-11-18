/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "onnxdims.h"

#include <cstdint>
#include <vector>

namespace rtengine
{

namespace
{

// Just enough of the protobuf wire format to walk an ONNX file: a message is
// a run of (field number, wire type, payload) records, and a nested message
// is a length-delimited payload holding the same again.
//
// The path rewritten here, by field number (onnx.proto):
//   ModelProto.graph = 7
//   GraphProto.input = 11, .output = 12, .value_info = 13
//   ValueInfoProto.type = 2
//   TypeProto.tensor_type = 1
//   TypeProto.Tensor.shape = 2
//   TensorShapeProto.dim = 1
//   Dimension.dim_value = 1 (varint), .dim_param = 2 (string)

struct Field {
    std::uint32_t number = 0;
    std::uint32_t wire = 0;
    const char* record = nullptr;    // start of the whole record, tag included
    std::size_t recordSize = 0;
    const char* payload = nullptr;   // length-delimited: the bytes; varint: unused
    std::size_t payloadSize = 0;
    std::uint64_t value = 0;         // varint fields
};

bool readVarint(const char*& p, const char* end, std::uint64_t& value)
{
    value = 0;
    for (int shift = 0; shift < 64 && p < end; shift += 7) {
        const unsigned char byte = static_cast<unsigned char>(*p++);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

bool readFields(const char* p, std::size_t size, std::vector<Field>& fields)
{
    const char* const end = p + size;
    while (p < end) {
        Field field;
        field.record = p;
        std::uint64_t tag = 0;
        if (!readVarint(p, end, tag)) return false;
        field.number = static_cast<std::uint32_t>(tag >> 3);
        field.wire = static_cast<std::uint32_t>(tag & 7);
        switch (field.wire) {
            case 0:
                if (!readVarint(p, end, field.value)) return false;
                break;
            case 1:
                if (end - p < 8) return false;
                p += 8;
                break;
            case 2: {
                std::uint64_t length = 0;
                if (!readVarint(p, end, length) || length > static_cast<std::uint64_t>(end - p)) return false;
                field.payload = p;
                field.payloadSize = static_cast<std::size_t>(length);
                p += length;
                break;
            }
            case 5:
                if (end - p < 4) return false;
                p += 4;
                break;
            default:
                return false;   // groups: not used by ONNX
        }
        field.recordSize = static_cast<std::size_t>(p - field.record);
        fields.push_back(field);
    }
    return true;
}

void writeVarint(std::string& out, std::uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void writeDelimited(std::string& out, std::uint32_t number, const std::string& payload)
{
    writeVarint(out, (static_cast<std::uint64_t>(number) << 3) | 2);
    writeVarint(out, payload.size());
    out.append(payload);
}

// Rebuilds a message, replacing the first field `number` (length-delimited)
// with whatever `rewrite` makes of its payload. Returns false when the field
// is absent, unparsable, or `rewrite` had nothing to change.
template<typename Rewrite>
bool replaceField(const char* data, std::size_t size, std::uint32_t number, std::string& out, const Rewrite& rewrite)
{
    std::vector<Field> fields;
    if (!readFields(data, size, fields)) return false;
    bool changed = false;
    std::string result;
    for (const auto& field : fields) {
        std::string inner;
        if (!changed && field.number == number && field.wire == 2
                && rewrite(field.payload, field.payloadSize, inner)) {
            writeDelimited(result, number, inner);
            changed = true;
        } else {
            result.append(field.record, field.recordSize);
        }
    }
    if (changed) out.swap(result);
    return changed;
}

// TensorShapeProto: an image tensor is [1, 1 or 3, height, width]. Anything
// else (a weight listed among the inputs by an old exporter, say) is left
// exactly as it is.
bool freeShape(const char* data, std::size_t size, std::string& out)
{
    std::vector<Field> dims;
    if (!readFields(data, size, dims) || dims.size() != 4) return false;
    std::uint64_t values[4] = {};
    bool fixed[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (dims[i].number != 1 || dims[i].wire != 2) return false;
        std::vector<Field> parts;
        if (!readFields(dims[i].payload, dims[i].payloadSize, parts)) return false;
        for (const auto& part : parts) {
            if (part.number == 1 && part.wire == 0) {
                values[i] = part.value;
                fixed[i] = true;
            }
        }
    }
    // The batch may itself be free; the channel count must be an image's.
    if ((fixed[0] && values[0] != 1) || !fixed[1] || (values[1] != 1 && values[1] != 3)) return false;
    if (!fixed[2] && !fixed[3]) return false;

    std::string result;
    static const char* const names[4] = {nullptr, nullptr, "height", "width"};
    for (int i = 0; i < 4; ++i) {
        if (i >= 2 && fixed[i]) {
            std::string dim;
            writeDelimited(dim, 2, names[i]);   // dim_param
            writeDelimited(result, 1, dim);
        } else {
            result.append(dims[i].record, dims[i].recordSize);
        }
    }
    out.swap(result);
    return true;
}

// ValueInfoProto -> type -> tensor_type -> shape.
bool freeValueInfo(const char* data, std::size_t size, std::string& out)
{
    return replaceField(data, size, 2, out, [](const char* type, std::size_t typeSize, std::string& typeOut) {
        return replaceField(type, typeSize, 1, typeOut, [](const char* tensor, std::size_t tensorSize, std::string& tensorOut) {
            return replaceField(tensor, tensorSize, 2, tensorOut, freeShape);
        });
    });
}

bool freeGraph(const char* data, std::size_t size, std::string& out)
{
    std::vector<Field> fields;
    if (!readFields(data, size, fields)) return false;
    bool changed = false;
    std::string result;
    result.reserve(size);
    for (const auto& field : fields) {
        if (field.wire == 2 && (field.number == 11 || field.number == 12)) {
            std::string info;
            if (freeValueInfo(field.payload, field.payloadSize, info)) {
                writeDelimited(result, field.number, info);
                changed = true;
                continue;
            }
        }
        result.append(field.record, field.recordSize);
    }
    if (!changed) return false;

    // The recorded intermediate shapes were computed for the fixed size.
    std::string pruned;
    pruned.reserve(result.size());
    fields.clear();
    if (!readFields(result.data(), result.size(), fields)) return false;
    for (const auto& field : fields) {
        if (field.number != 13) pruned.append(field.record, field.recordSize);
    }
    out.swap(pruned);
    return true;
}

}

bool onnxFreeImageDimensions(const std::string& model, std::string& out)
{
    std::string result;
    if (!replaceField(model.data(), model.size(), 7, result, freeGraph)) return false;
    out.swap(result);
    return true;
}

}
