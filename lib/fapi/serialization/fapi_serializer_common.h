#pragma once

#include "fapi_serialization_buffer.h"
#include "ocudu/adt/bounded_bitset.h"
#include "ocudu/adt/interval.h"
#include "ocudu/adt/span.h"
#include "ocudu/adt/static_vector.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/harq_id.h"
#include "ocudu/ran/phy_time_unit.h"
#include "ocudu/ran/rnti.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/ran/slot_point_extended.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include "ocudu/support/shared_transport_block.h"
#include "ocudu/support/units.h"
#include <bitset>
#include <chrono>
#include <optional>
#include <variant>
#include <vector>

namespace ocudu {
namespace fapi_serial {


/// Serialize subcarrier_spacing (enum stored as uint8_t).
inline void serialize(buffer_writer& w, subcarrier_spacing scs) { w.write_u8(static_cast<uint8_t>(scs)); }
inline void deserialize(buffer_reader& r, subcarrier_spacing& scs) { scs = static_cast<subcarrier_spacing>(r.read_u8()); }

/// Serialize cyclic_prefix.
inline void serialize(buffer_writer& w, cyclic_prefix cp) { w.write_u8(static_cast<uint8_t>(cp.value)); }
inline void deserialize(buffer_reader& r, cyclic_prefix& cp) { cp = cyclic_prefix(static_cast<cyclic_prefix::options>(r.read_u8())); }

/// Serialize rnti_t (uint16_t enum).
inline void serialize(buffer_writer& w, rnti_t rnti) { w.write_u16(static_cast<uint16_t>(rnti)); }
inline void deserialize(buffer_reader& r, rnti_t& rnti) { rnti = static_cast<rnti_t>(r.read_u16()); }

/// Serialize harq_id_t (uint8_t enum).
inline void serialize(buffer_writer& w, harq_id_t h) { w.write_u8(static_cast<uint8_t>(h)); }
inline void deserialize(buffer_reader& r, harq_id_t& h) { h = static_cast<harq_id_t>(r.read_u8()); }

/// Serialize units::bytes (wraps unsigned).
inline void serialize(buffer_writer& w, units::bytes b) { w.write_u32(b.value()); }
inline void deserialize(buffer_reader& r, units::bytes& b) { b = units::bytes(r.read_u32()); }

/// Serialize units::bits (wraps unsigned).
inline void serialize(buffer_writer& w, units::bits b) { w.write_u32(b.value()); }
inline void deserialize(buffer_reader& r, units::bits& b) { b = units::bits(r.read_u32()); }


inline void serialize(buffer_writer& w, slot_point sp)
{
  if (sp.valid()) {
    w.write_u8(1); // valid flag
    w.write_u8(static_cast<uint8_t>(sp.numerology()));
    w.write_u32(sp.to_uint());
  } else {
    w.write_u8(0); // invalid
  }
}

inline void deserialize(buffer_reader& r, slot_point& sp)
{
  uint8_t valid = r.read_u8();
  if (valid) {
    uint8_t  numerology = r.read_u8();
    uint32_t count      = r.read_u32();

    const uint32_t max_count = 1024u * 1024u * 10u * (1u << numerology);
    if (numerology >= 5 || count >= max_count) {
      sp = slot_point();
    } else {
      sp = slot_point(numerology, count);
    }
  } else {
    sp = slot_point();
  }
}

inline void serialize(buffer_writer& w, const slot_point_extended& sp)
{
  w.write_u8(static_cast<uint8_t>(sp.numerology()));
  w.write_u32(sp.to_uint());
}

inline void deserialize(buffer_reader& r, slot_point_extended& sp)
{
  uint8_t  numerology = r.read_u8();
  uint32_t count      = r.read_u32();
  // Torn-read defence: drop to default on out-of-range numerology or count.
  const uint32_t max_count = 1024u * 1024u * 10u * (1u << numerology);
  if (numerology >= 5 || count >= max_count) {
    sp = slot_point_extended();
  } else {
    sp = slot_point_extended(static_cast<subcarrier_spacing>(numerology), count);
  }
}


inline void serialize(buffer_writer& w, const std::chrono::time_point<std::chrono::system_clock>& tp)
{
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
  w.write_i64(ns);
}

inline void deserialize(buffer_reader& r, std::chrono::time_point<std::chrono::system_clock>& tp)
{
  int64_t ns = r.read_i64();
  tp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::nanoseconds(ns));
}

/// Serialize std::chrono::milliseconds.
inline void serialize(buffer_writer& w, std::chrono::milliseconds ms) { w.write_i64(ms.count()); }
inline void deserialize(buffer_reader& r, std::chrono::milliseconds& ms) { ms = std::chrono::milliseconds(r.read_i64()); }


inline void serialize(buffer_writer& w, const phy_time_unit& t) { w.write_i64(t.to_Tc()); }
inline void deserialize(buffer_reader& r, phy_time_unit& t) { t = phy_time_unit::from_units_of_Tc(r.read_i64()); }


template <typename T, bool RightClosed, typename Tag>
void serialize(buffer_writer& w, const interval<T, RightClosed, Tag>& intv)
{
  if constexpr (sizeof(T) == 1) {
    w.write_u8(static_cast<uint8_t>(intv.start()));
    w.write_u8(static_cast<uint8_t>(intv.stop()));
  } else if constexpr (sizeof(T) == 2) {
    w.write_u16(static_cast<uint16_t>(intv.start()));
    w.write_u16(static_cast<uint16_t>(intv.stop()));
  } else {
    w.write_u32(static_cast<uint32_t>(intv.start()));
    w.write_u32(static_cast<uint32_t>(intv.stop()));
  }
}

template <typename T, bool RightClosed, typename Tag>
void deserialize(buffer_reader& r, interval<T, RightClosed, Tag>& intv)
{
  T start, stop;
  if constexpr (sizeof(T) == 1) {
    start = static_cast<T>(r.read_u8());
    stop  = static_cast<T>(r.read_u8());
  } else if constexpr (sizeof(T) == 2) {
    start = static_cast<T>(r.read_u16());
    stop  = static_cast<T>(r.read_u16());
  } else {
    start = static_cast<T>(r.read_u32());
    stop  = static_cast<T>(r.read_u32());
  }
  if (start <= stop) {
    intv.set(start, stop);
  } else {
    intv = interval<T, RightClosed, Tag>();
  }
}

// Use LSB-first chunks here — extract<uint64_t>() is MSB-first and would corrupt DCI/UCI on the wire.

template <size_t N, bool Reversed>
void serialize(buffer_writer& w, const bounded_bitset<N, Reversed>& bs)
{
  uint32_t nbits = static_cast<uint32_t>(bs.size());
  w.write_u32(nbits);
  for (uint32_t bit_pos = 0; bit_pos < nbits; bit_pos += 64) {
    uint32_t chunk_bits = std::min(64U, nbits - bit_pos);
    uint64_t chunk_val  = 0;
    for (uint32_t b = 0; b < chunk_bits; ++b) {
      if (bs.test(bit_pos + b)) {
        chunk_val |= (1ULL << b);
      }
    }
    w.write_u64(chunk_val);
  }
}

template <size_t N, bool Reversed>
void deserialize(buffer_reader& r, bounded_bitset<N, Reversed>& bs)
{
  uint32_t nbits = r.read_u32();
  bs.resize(nbits);
  bs.reset();
  for (uint32_t bit_pos = 0; bit_pos < nbits; bit_pos += 64) {
    uint32_t chunk_bits = std::min(64U, nbits - bit_pos);
    uint64_t chunk_val  = r.read_u64();
    for (uint32_t b = 0; b < chunk_bits; ++b) {
      if (chunk_val & (1ULL << b)) {
        bs.set(bit_pos + b);
      }
    }
  }
}


template <size_t N>
void serialize(buffer_writer& w, const std::bitset<N>& bs)
{
  static_assert(N <= 64, "std::bitset serialization only supports up to 64 bits");
  w.write_u64(bs.to_ullong());
}

template <size_t N>
void deserialize(buffer_reader& r, std::bitset<N>& bs)
{
  bs = std::bitset<N>(r.read_u64());
}


template <typename T>
void serialize_optional(buffer_writer& w, const std::optional<T>& opt)
{
  if (opt.has_value()) {
    w.write_u8(1);
    serialize(w, opt.value());
  } else {
    w.write_u8(0);
  }
}

template <typename T>
void deserialize_optional(buffer_reader& r, std::optional<T>& opt)
{
  uint8_t present = r.read_u8();
  if (present) {
    T val{};
    deserialize(r, val);
    opt = std::move(val);
  } else {
    opt = std::nullopt;
  }
}


template <typename T, size_t N>
void serialize_static_vector(buffer_writer& w, const static_vector<T, N>& vec)
{
  uint16_t count = static_cast<uint16_t>(vec.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    serialize(w, vec[i]);
  }
}

template <typename T, size_t N>
void deserialize_static_vector(buffer_reader& r, static_vector<T, N>& vec)
{
  uint16_t count = r.read_u16();
  vec.clear();
  for (uint16_t i = 0; i < count; ++i) {
    T val{};
    deserialize(r, val);
    vec.push_back(std::move(val));
  }
}

template <size_t N>
void serialize_static_vector_u8(buffer_writer& w, const static_vector<uint8_t, N>& vec)
{
  uint16_t count = static_cast<uint16_t>(vec.size());
  w.write_u16(count);
  if (count > 0) {
    w.write_bytes(vec.data(), count);
  }
}

template <size_t N>
void deserialize_static_vector_u8(buffer_reader& r, static_vector<uint8_t, N>& vec)
{
  uint16_t count = r.read_u16();
  // Clamp against torn-buffer reads where count decodes to garbage; drain and truncate.
  uint16_t effective = (count <= N) ? count : static_cast<uint16_t>(N);
  vec.resize(effective);
  if (effective > 0) {
    r.read_bytes(vec.data(), effective);
  }
  if (count > effective) {
    for (uint16_t i = effective; i < count; ++i) {
      (void) r.read_u8();
    }
  }
}

template <size_t N>
void serialize_static_vector_u16(buffer_writer& w, const static_vector<uint16_t, N>& vec)
{
  uint16_t count = static_cast<uint16_t>(vec.size());
  w.write_u16(count);
  for (uint16_t i = 0; i < count; ++i) {
    w.write_u16(vec[i]);
  }
}

template <size_t N>
void deserialize_static_vector_u16(buffer_reader& r, static_vector<uint16_t, N>& vec)
{
  uint16_t count = r.read_u16();
  uint16_t effective = (count <= N) ? count : static_cast<uint16_t>(N);
  vec.clear();
  for (uint16_t i = 0; i < effective; ++i) {
    vec.push_back(r.read_u16());
  }
  for (uint16_t i = effective; i < count; ++i) {
    (void) r.read_u16();
  }
}


template <typename T>
void serialize_vector(buffer_writer& w, const std::vector<T>& vec)
{
  uint32_t count = static_cast<uint32_t>(vec.size());
  w.write_u32(count);
  for (uint32_t i = 0; i < count; ++i) {
    serialize(w, vec[i]);
  }
}

template <typename T>
void deserialize_vector(buffer_reader& r, std::vector<T>& vec)
{
  uint32_t count = r.read_u32();
  vec.clear();
  vec.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    T val{};
    deserialize(r, val);
    vec.push_back(std::move(val));
  }
}

inline void serialize_span_u8(buffer_writer& w, span<const uint8_t> s)
{
  w.write_u32(static_cast<uint32_t>(s.size()));
  if (!s.empty()) {
    w.write_bytes(s.data(), static_cast<uint32_t>(s.size()));
  }
}

inline void deserialize_span_u8(buffer_reader& r, std::vector<uint8_t>& storage, span<const uint8_t>& s)
{
  uint32_t size = r.read_u32();
  storage.resize(size);
  if (size > 0) {
    r.read_bytes(storage.data(), size);
  }
  s = span<const uint8_t>(storage.data(), storage.size());
}

inline void serialize_shared_transport_block(buffer_writer& w, const shared_transport_block& tb)
{
  serialize_span_u8(w, tb.get_buffer());
}

template <typename EnumT>
void serialize_enum_u8(buffer_writer& w, EnumT e) { w.write_u8(static_cast<uint8_t>(e)); }

template <typename EnumT>
void deserialize_enum_u8(buffer_reader& r, EnumT& e) { e = static_cast<EnumT>(r.read_u8()); }

template <typename EnumT>
void serialize_enum_u16(buffer_writer& w, EnumT e) { w.write_u16(static_cast<uint16_t>(e)); }

template <typename EnumT>
void deserialize_enum_u16(buffer_reader& r, EnumT& e) { e = static_cast<EnumT>(r.read_u16()); }

} // namespace fapi_serial
} // namespace ocudu
