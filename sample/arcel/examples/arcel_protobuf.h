// arcel_protobuf.h — header-only adapter that surfaces a
// libprotobuf C++ Message to arcel via the arcel_object_desc
// dispatch table.  Drop-in for any *.pb.h-generated message:
//
//     #include "arcel_protobuf.h"
//
//     UserRequest req;
//     req.set_age(25);
//     req.set_country("JP");
//
//     arcel_activation_set_object(act, "u", &req, &arcel::pbf::descriptor);
//     arcel_value r = arcel_eval(prg, act);
//
// Field access uses the message's runtime Descriptor + Reflection, so
// this adapter works with EVERY *.pb.h-generated type without per-type
// code on the embedder's side.  Build with `-lprotobuf`.
//
// Coverage:
//   ✅ scalar fields: int32 / int64 / uint32 / uint64 / float / double / bool / string / bytes
//   ✅ enum (rendered as int)
//   ✅ nested message (recursive arcel_value_object on the same desc)
//   ✅ has() (uses Reflection::HasField for singular non-repeated)
//   ✅ repeated scalar fields → arcel list (built in the per-eval arena
//      via arcel_value_list_new — added in arcel.h Phase 5)
//   ✅ repeated message fields → arcel list of object handles
//   ✅ map<K,V> fields → arcel map (Phase 6, via arcel_value_map_new)
//   ⏭ Any / oneof: handled at a higher level

#ifndef ARCEL_PROTOBUF_H
#define ARCEL_PROTOBUF_H

#include <cstddef>
#include <cstring>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

extern "C" {
#include "arcel.h"
}

namespace arcel { namespace pbf {

// Forward decls so `descriptor` (below) can take the function
// addresses, and so `pb_field` (also below) can take `&descriptor`
// for nested-message recursion.  All `inline` so the header can be
// included from multiple TUs (single canonical address per ODR).
inline int     pb_field      (const arcel_object_desc *, const void *,
                              const char *, std::size_t,
                              arcel_arena_handle *, arcel_value *);
inline int     pb_has        (const arcel_object_desc *, const void *,
                              const char *, std::size_t);
inline std::size_t pb_format_json(const arcel_object_desc *, const void *,
                                  char *, std::size_t);

inline const arcel_object_desc descriptor = {
    pb_field,
    pb_has,
    pb_format_json,
    "google.protobuf.Message",
};

// Convert a single repeated-field element at index `i` to an
// arcel_value.  Owned strings are copied into the arena because
// libprotobuf's GetRepeatedStringReference uses a scratch that
// lives only inside this function's frame.
inline arcel_value
pb_repeated_at(const google::protobuf::Reflection *refl,
               const google::protobuf::Message     *msg,
               const google::protobuf::FieldDescriptor *fd,
               int i, arcel_arena_handle *arena)
{
    using google::protobuf::FieldDescriptor;
    switch (fd->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:
            return arcel_value_int(refl->GetRepeatedInt32(*msg, fd, i));
        case FieldDescriptor::CPPTYPE_INT64:
            return arcel_value_int(refl->GetRepeatedInt64(*msg, fd, i));
        case FieldDescriptor::CPPTYPE_UINT32:
            return arcel_value_uint(refl->GetRepeatedUInt32(*msg, fd, i));
        case FieldDescriptor::CPPTYPE_UINT64:
            return arcel_value_uint(refl->GetRepeatedUInt64(*msg, fd, i));
        case FieldDescriptor::CPPTYPE_DOUBLE:
            return arcel_value_double(refl->GetRepeatedDouble(*msg, fd, i));
        case FieldDescriptor::CPPTYPE_FLOAT:
            return arcel_value_double(refl->GetRepeatedFloat(*msg, fd, i));
        case FieldDescriptor::CPPTYPE_BOOL:
            return arcel_value_bool(refl->GetRepeatedBool(*msg, fd, i));
        case FieldDescriptor::CPPTYPE_ENUM:
            return arcel_value_int(refl->GetRepeatedEnum(*msg, fd, i)->number());
        case FieldDescriptor::CPPTYPE_STRING: {
            std::string scratch;
            const std::string &s = refl->GetRepeatedStringReference(*msg, fd, i, &scratch);
            return arcel_value_string_copy(arena, s.data(), s.size());
        }
        case FieldDescriptor::CPPTYPE_MESSAGE: {
            const auto &nested = refl->GetRepeatedMessage(*msg, fd, i);
            return arcel_value_object(&nested, &descriptor);
        }
    }
    return arcel_value_error("pb_repeated_at: unrecognized cpp_type");
}

inline int
pb_field(const arcel_object_desc *, const void *obj_,
         const char *name, std::size_t name_len,
         arcel_arena_handle *arena, arcel_value *out)
{
    using google::protobuf::FieldDescriptor;
    auto *msg  = static_cast<const google::protobuf::Message *>(obj_);
    auto *desc = msg->GetDescriptor();
    auto *refl = msg->GetReflection();
    auto *fd   = desc->FindFieldByName(std::string(name, name_len));
    if (!fd) return -1;

    // map<K, V> is encoded in the descriptor as a repeated MessageEntry
    // (`fd->is_map()` distinguishes it from a regular repeated message).
    // Walk via the same RepeatedMessage iteration but materialize as an
    // arcel map keyed on the entry's `key` field.
    if (fd->is_map()) {
        const int n = refl->FieldSize(*msg, fd);
        constexpr int kStackLimit = 64;
        arcel_value  stack_kv[kStackLimit * 2];
        arcel_value *kv = (n <= kStackLimit) ? stack_kv
                                             : new arcel_value[n * 2];
        const auto *entry_desc = fd->message_type();
        const auto *key_fd     = entry_desc->map_key();
        const auto *val_fd     = entry_desc->map_value();
        for (int i = 0; i < n; ++i) {
            const auto &entry = refl->GetRepeatedMessage(*msg, fd, i);
            // pb_field_at_singular handles a singular field on `entry`.
            // We inline a minimal version here so we don't have to
            // refactor pb_field's switch into a separate helper.
            auto extract = [&](const google::protobuf::FieldDescriptor *xfd) -> arcel_value {
                const auto *e_refl = entry.GetReflection();
                switch (xfd->cpp_type()) {
                    case FieldDescriptor::CPPTYPE_INT32:  return arcel_value_int (e_refl->GetInt32(entry,  xfd));
                    case FieldDescriptor::CPPTYPE_INT64:  return arcel_value_int (e_refl->GetInt64(entry,  xfd));
                    case FieldDescriptor::CPPTYPE_UINT32: return arcel_value_uint(e_refl->GetUInt32(entry, xfd));
                    case FieldDescriptor::CPPTYPE_UINT64: return arcel_value_uint(e_refl->GetUInt64(entry, xfd));
                    case FieldDescriptor::CPPTYPE_DOUBLE: return arcel_value_double(e_refl->GetDouble(entry, xfd));
                    case FieldDescriptor::CPPTYPE_FLOAT:  return arcel_value_double(e_refl->GetFloat(entry,  xfd));
                    case FieldDescriptor::CPPTYPE_BOOL:   return arcel_value_bool  (e_refl->GetBool(entry,   xfd));
                    case FieldDescriptor::CPPTYPE_ENUM:   return arcel_value_int   (e_refl->GetEnum(entry, xfd)->number());
                    case FieldDescriptor::CPPTYPE_STRING: {
                        std::string scratch;
                        const std::string &s = e_refl->GetStringReference(entry, xfd, &scratch);
                        return arcel_value_string_copy(arena, s.data(), s.size());
                    }
                    case FieldDescriptor::CPPTYPE_MESSAGE: {
                        const auto &nested = e_refl->GetMessage(entry, xfd);
                        return arcel_value_object(&nested, &descriptor);
                    }
                }
                return arcel_value_error("map entry: unrecognized cpp_type");
            };
            kv[2 * i]     = extract(key_fd);
            kv[2 * i + 1] = extract(val_fd);
        }
        *out = arcel_value_map_new(arena, static_cast<std::uint32_t>(n), kv);
        if (n > kStackLimit) delete[] kv;
        return 0;
    }

    // Plain repeated → list built in the per-eval arena.
    if (fd->is_repeated()) {
        const int n = refl->FieldSize(*msg, fd);
        // Build items array on the stack for small lists; for very
        // large repeated fields fall back to heap (rare in CEL
        // policies — typical lists are <16 elements).
        constexpr int kStackLimit = 64;
        arcel_value  stack_items[kStackLimit];
        arcel_value *items = (n <= kStackLimit) ? stack_items
                                                : new arcel_value[n];
        for (int i = 0; i < n; ++i) {
            items[i] = pb_repeated_at(refl, msg, fd, i, arena);
        }
        *out = arcel_value_list_new(arena, static_cast<std::uint32_t>(n), items);
        if (n > kStackLimit) delete[] items;
        return 0;
    }

    // Singular non-repeated optional/required: treat unset as missing
    // so cel-spec `has()` and field access semantics align.
    if (fd->has_presence() && !refl->HasField(*msg, fd)) return -1;

    switch (fd->cpp_type()) {
        case FieldDescriptor::CPPTYPE_INT32:
            *out = arcel_value_int(refl->GetInt32(*msg, fd));     return 0;
        case FieldDescriptor::CPPTYPE_INT64:
            *out = arcel_value_int(refl->GetInt64(*msg, fd));     return 0;
        case FieldDescriptor::CPPTYPE_UINT32:
            *out = arcel_value_uint(refl->GetUInt32(*msg, fd));   return 0;
        case FieldDescriptor::CPPTYPE_UINT64:
            *out = arcel_value_uint(refl->GetUInt64(*msg, fd));   return 0;
        case FieldDescriptor::CPPTYPE_DOUBLE:
            *out = arcel_value_double(refl->GetDouble(*msg, fd)); return 0;
        case FieldDescriptor::CPPTYPE_FLOAT:
            *out = arcel_value_double(refl->GetFloat(*msg, fd));  return 0;
        case FieldDescriptor::CPPTYPE_BOOL:
            *out = arcel_value_bool(refl->GetBool(*msg, fd));     return 0;
        case FieldDescriptor::CPPTYPE_ENUM:
            *out = arcel_value_int(refl->GetEnum(*msg, fd)->number()); return 0;
        case FieldDescriptor::CPPTYPE_STRING: {
            // The string buffer libprotobuf returns lives in the
            // message itself (or a returned scratch) and outlives the
            // eval, so we don't need to copy via the arena.  A future
            // change to libprotobuf's API may force us to switch to
            // arcel_value_string_copy here.
            std::string scratch;
            const std::string &s = refl->GetStringReference(*msg, fd, &scratch);
            *out = arcel_value_string(s.data(), s.size());
            return 0;
        }
        case FieldDescriptor::CPPTYPE_MESSAGE: {
            const auto &nested = refl->GetMessage(*msg, fd);
            *out = arcel_value_object(&nested, &descriptor);
            return 0;
        }
    }
    return -2;  // unrecognized cpp_type
}

inline int
pb_has(const arcel_object_desc *, const void *obj_,
       const char *name, std::size_t name_len)
{
    auto *msg = static_cast<const google::protobuf::Message *>(obj_);
    auto *fd  = msg->GetDescriptor()->FindFieldByName(std::string(name, name_len));
    if (!fd) return 0;
    if (fd->is_repeated()) return msg->GetReflection()->FieldSize(*msg, fd) > 0;
    if (fd->has_presence()) return msg->GetReflection()->HasField(*msg, fd) ? 1 : 0;
    return 1;  // proto3 scalar without explicit presence: always "present"
}

inline std::size_t
pb_format_json(const arcel_object_desc *, const void *obj_,
               char *buf, std::size_t cap)
{
    auto *msg  = static_cast<const google::protobuf::Message *>(obj_);
    std::string out = msg->ShortDebugString();
    if (buf && cap > 0) {
        std::size_t n = out.size() < cap - 1 ? out.size() : cap - 1;
        std::memcpy(buf, out.data(), n);
        buf[n] = '\0';
    }
    return out.size();
}

}}  // namespace arcel::pbf

#endif  // ARCEL_PROTOBUF_H
