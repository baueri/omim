#pragma once

#include "transit/transit_types.hpp"

#include "indexer/geometry_coding.hpp"

#include "geometry/point2d.hpp"

#include "coding/point_to_integer.hpp"
#include "coding/read_write_utils.hpp"
#include "coding/reader.hpp"
#include "coding/varint.hpp"
#include "coding/write_to_sink.hpp"

#include "base/assert.hpp"
#include "base/exception.hpp"
#include "base/macros.hpp"
#include "base/newtype.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace routing
{
namespace transit
{
// Note. For the time being double in transit section is used only for saving weight of edges (in seconds).
// Let us assume that it takes less than 10^7 seconds (115 days) to get from one station to a neighboring one.
double constexpr kMinDoubleAtTransitSection = kInvalidWeight;
double constexpr kMaxDoubleAtTransitSection = 10000000.0;
uint32_t constexpr kDoubleBits = 32;

template <typename Sink>
class Serializer
{
public:
  explicit Serializer(Sink & sink) : m_sink(sink) {}

  template <typename T>
  typename std::enable_if<(std::is_integral<T>::value || std::is_enum<T>::value) &&
                          !std::is_same<T, uint32_t>::value && !std::is_same<T, uint64_t>::value &&
                          !std::is_same<T, int32_t>::value &&
                          !std::is_same<T, int64_t>::value>::type
  operator()(T const & t, char const * /* name */ = nullptr)
  {
    WriteToSink(m_sink, t);
  }

  template <typename T>
  typename std::enable_if<std::is_same<T, uint32_t>::value ||
                          std::is_same<T, uint64_t>::value>::type
  operator()(T t, char const * /* name */ = nullptr) const
  {
    WriteVarUint(m_sink, t);
  }

  template <typename T>
  typename std::enable_if<std::is_same<T, int32_t>::value || std::is_same<T, int64_t>::value>::type
  operator()(T t, char const * name = nullptr) const
  {
    WriteVarInt(m_sink, t);
  }

  template <typename T>
  typename std::enable_if<std::is_same<T, double>::value || std::is_same<T, float>::value>::type
  operator()(T d, char const * name = nullptr)
  {
    CHECK_GREATER_OR_EQUAL(d, kMinDoubleAtTransitSection, ());
    CHECK_LESS_OR_EQUAL(d, kMaxDoubleAtTransitSection, ());
    (*this)(DoubleToUint32(d, kMinDoubleAtTransitSection, kMaxDoubleAtTransitSection, kDoubleBits), name);
  }

  void operator()(std::string const & s, char const * /* name */ = nullptr)
  {
    rw::Write(m_sink, s);
  }

  void operator()(m2::PointD const & p, char const * /* name */ = nullptr)
  {
    WriteVarInt(m_sink, PointToInt64(p, POINT_COORD_BITS));
  }

  void operator()(std::vector<m2::PointD> const & vs, char const * /* name */ = nullptr)
  {
    CHECK_LESS_OR_EQUAL(vs.size(), std::numeric_limits<uint64_t>::max(), ());
    WriteVarUint(m_sink, static_cast<uint64_t>(vs.size()));
    m2::PointU lastEncodedPoint;
    for (auto const & p : vs)
    {
      m2::PointU const pointU = PointD2PointU(p, POINT_COORD_BITS);
      WriteVarUint(m_sink, EncodeDelta(pointU, lastEncodedPoint));
      lastEncodedPoint = pointU;
    }
  }

  void operator()(Edge::WrappedEdgeId const & id, char const * /* name */ = nullptr)
  {
    CHECK_GREATER_OR_EQUAL(id.Get(), m_lastWrappedEdgeId.Get(), ());
    WriteVarUint(m_sink, static_cast<uint64_t>(id.Get() - m_lastWrappedEdgeId.Get()));
    m_lastWrappedEdgeId = id;
  }

  void operator()(Stop::WrappedStopId const & id, char const * /* name */ = nullptr)
  {
    CHECK_GREATER_OR_EQUAL(id.Get(), m_lastWrappedStopId.Get(), ());
    WriteVarUint(m_sink, static_cast<uint64_t>(id.Get() - m_lastWrappedStopId.Get()));
    m_lastWrappedStopId = id;
  }

  void operator()(FeatureIdentifiers const & id, char const * name = nullptr)
  {
    if (id.IsSerializeFeatureIdOnly())
      (*this)(id.GetFeatureId(), name);
    else
      id.Visit(*this);
  }

  void operator()(Edge const & e, char const * /* name */ = nullptr)
  {
    (*this)(e.m_stop1Id);
    (*this)(e.m_stop2Id);
    (*this)(e.m_weight);
    (*this)(e.m_lineId);
    // Note. |Edge::m_flags| is not filled fully after deserialization from json.
    // So it's necessary to fill it here.
    EdgeFlags const flags =
        GetEdgeFlags(e.GetTransfer(), e.GetStop1Id(), e.GetStop2Id(), e.GetShapeIds());
    (*this)(flags);

    if (flags.m_isShapeIdsEmpty || flags.m_isShapeIdsSame || flags.m_isShapeIdsReversed)
      return;

    if (flags.m_isShapeIdsSingle)
    {
      CHECK_EQUAL(e.GetShapeIds().size(), 1, ());
      (*this)(e.GetShapeIds()[0]);
      return;
    }

    (*this)(e.GetShapeIds());
  }

  void operator()(EdgeFlags const & f, char const * /* name */ = nullptr) { (*this)(f.GetFlags()); }

  template <typename T>
  void operator()(std::vector<T> const & vs, char const * /* name */ = nullptr)
  {
    CHECK_LESS_OR_EQUAL(vs.size(), std::numeric_limits<uint64_t>::max(), ());
    WriteVarUint(m_sink, static_cast<uint64_t>(vs.size()));
    for (auto const & v : vs)
      (*this)(v);
  }

  template<typename T>
  typename std::enable_if<std::is_class<T>::value>::type operator()(T const & t, char const * /* name */ = nullptr)
  {
    t.Visit(*this);
  }

private:
  Sink & m_sink;
  Edge::WrappedEdgeId m_lastWrappedEdgeId = {};
  Stop::WrappedStopId m_lastWrappedStopId = {};
};

template <typename Source>
class Deserializer
{
public:
  explicit Deserializer(Source & source) : m_source(source) {}

  template <typename T>
  typename std::enable_if<(std::is_integral<T>::value || std::is_enum<T>::value) &&
                          !std::is_same<T, uint32_t>::value && !std::is_same<T, uint64_t>::value &&
                          !std::is_same<T, int32_t>::value &&
                          !std::is_same<T, int64_t>::value>::type
  operator()(T & t, char const * name = nullptr)
  {
    ReadPrimitiveFromSource(m_source, t);
  }

  template <typename T>
  typename std::enable_if<std::is_same<T, uint32_t>::value ||
                          std::is_same<T, uint64_t>::value>::type
  operator()(T & t, char const * name = nullptr)
  {
    t = ReadVarUint<T, Source>(m_source);
  }

  template <typename T>
  typename std::enable_if<std::is_same<T, int32_t>::value || std::is_same<T, int64_t>::value>::type
  operator()(T & t, char const * name = nullptr)
  {
    t = ReadVarInt<T, Source>(m_source);
  }

  template <typename T>
  typename std::enable_if<std::is_same<T, double>::value || std::is_same<T, float>::value>::type
  operator()(T & d, char const * name = nullptr)
  {
    uint32_t ui;
    (*this)(ui, name);
    d = Uint32ToDouble(ui, kMinDoubleAtTransitSection, kMaxDoubleAtTransitSection, kDoubleBits);
  }

  void operator()(std::string & s, char const * /* name */ = nullptr)
  {
    rw::Read(m_source, s);
  }

  void operator()(m2::PointD & p, char const * /* name */ = nullptr)
  {
    p = Int64ToPoint(ReadVarInt<int64_t, Source>(m_source), POINT_COORD_BITS);
  }

  void operator()(Edge::WrappedEdgeId & id, char const * /* name */ = nullptr)
  {
    id = m_lastWrappedEdgeId + Edge::WrappedEdgeId(ReadVarUint<uint64_t, Source>(m_source));
    m_lastWrappedEdgeId = id;
  }

  void operator()(Stop::WrappedStopId & id, char const * /* name */ = nullptr)
  {
    id = m_lastWrappedStopId + Stop::WrappedStopId(ReadVarUint<uint64_t, Source>(m_source));
    m_lastWrappedStopId = id;
  }

  void operator()(FeatureIdentifiers & id, char const * name = nullptr)
  {
    if (id.IsSerializeFeatureIdOnly())
    {
      FeatureId featureId;
      operator()(featureId, name);
      id.SetOsmId(kInvalidOsmId);
      id.SetFeatureId(featureId);
      return;
    }

    id.Visit(*this);
  }

  void operator()(Edge & e, char const * name = nullptr)
  {
    (*this)(e.m_stop1Id);
    (*this)(e.m_stop2Id);
    (*this)(e.m_weight);
    (*this)(e.m_lineId);
    (*this)(e.m_flags);

    e.m_shapeIds.clear();
    if (e.m_flags.m_isShapeIdsEmpty)
      return;

    if (e.m_flags.m_isShapeIdsSame)
    {
      e.m_shapeIds.emplace_back(e.GetStop1Id(), e.GetStop2Id());
      return;
    }

    if (e.m_flags.m_isShapeIdsReversed)
    {
      e.m_shapeIds.emplace_back(e.GetStop2Id(), e.GetStop1Id());
      return;
    }

    if (e.m_flags.m_isShapeIdsSingle)
    {
      e.m_shapeIds.resize(1 /* single shape id */);
      (*this)(e.m_shapeIds.back());
      return;
    }

    (*this)(e.m_shapeIds);
  }

  void operator()(EdgeFlags & f, char const * /* name */ = nullptr)
  {
    uint8_t flags = 0;
    (*this)(flags);
    f.SetFlags(flags);
  }

  void operator()(vector<m2::PointD> & vs, char const * /* name */ = nullptr)
  {
    auto const size = ReadVarUint<uint64_t, Source>(m_source);
    m2::PointU lastDecodedPoint;
    vs.resize(size);
    for (auto & p : vs)
    {
      m2::PointU const pointU = DecodeDelta(ReadVarUint<uint64_t, Source>(m_source), lastDecodedPoint);
      p = PointU2PointD(pointU, POINT_COORD_BITS);
      lastDecodedPoint = pointU;
    }
  }

  template <typename T>
  void operator()(vector<T> & vs, char const * /* name */ = nullptr)
  {
    auto const size = ReadVarUint<uint64_t, Source>(m_source);
    vs.resize(size);
    for (auto & v : vs)
      (*this)(v);
  }

  template <typename T>
  typename std::enable_if<std::is_class<T>::value>::type
  operator()(T & t, char const * /* name */ = nullptr)
  {
    t.Visit(*this);
  }

private:
  Source & m_source;
  Edge::WrappedEdgeId m_lastWrappedEdgeId = {};
  Stop::WrappedStopId m_lastWrappedStopId = {};
};

template <typename Sink>
class FixedSizeSerializer
{
public:
  explicit FixedSizeSerializer(Sink & sink) : m_sink(sink) {}

  template <typename T>
  typename std::enable_if<std::is_integral<T>::value || std::is_enum<T>::value, void>::type
  operator()(T const & t, char const * /* name */ = nullptr)
  {
    WriteToSink(m_sink, t);
  }

  void operator()(TransitHeader const & header) { header.Visit(*this); }

private:
  Sink & m_sink;
};

template <typename Source>
class FixedSizeDeserializer
{
public:
  explicit FixedSizeDeserializer(Source & source) : m_source(source) {}

  template <typename T>
  typename std::enable_if<std::is_integral<T>::value || std::is_enum<T>::value, void>::type
  operator()(T & t, char const * name = nullptr)
  {
    ReadPrimitiveFromSource(m_source, t);
  }

  void operator()(TransitHeader & header) { header.Visit(*this); }

private:
  Source & m_source;
};
}  // namespace transit
}  // namespace routing
