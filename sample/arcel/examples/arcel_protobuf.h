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
// Coverage (MVP):
//   ✅ scalar fields: int32 / int64 / uint32 / uint64 / float / double / bool / string / bytes
//   ✅ enum (rendered as int)
//   ✅ nested message (recursive arcel_value_object on the same desc)
//   ✅ has() (uses Reflection::HasField for singular non-repeated)
//   ⏭ repeated fields: skip for now (would need an arcel_list builder
//     callback with an arena handle; future API addition)
//   ⏭ map<K,V>: same blocker
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
                              const char *, std::size_t, arcel_value *);
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

inline int
pb_field(const arcel_object_desc *, const void *obj_,
         const char *name, std::size_t name_len, arcel_value *out)
{
    using google::protobuf::FieldDescriptor;
    auto *msg  = static_cast<const google::protobuf::Message *>(obj_);
    auto *desc = msg->GetDescriptor();
    auto *refl = msg->GetReflection();
    auto *fd   = desc->FindFieldByName(std::string(name, name_len));
    if (!fd) return -1;

    // Repeated / map fields not yet supported (need arena-backed list).
    if (fd->is_repeated()) return -2;

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
            // GetStringReference avoids copy when underlying repr already
            // matches; valid until next call on the same field.
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
