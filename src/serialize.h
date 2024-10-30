// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Copyright (c) 2024 TBCNODE DEV GROUP
// Distributed under the Open TBC software license, see the accompanying file LICENSE.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SERIALIZE_H
#define BITCOIN_SERIALIZE_H

//#include <attributes.h>
//#include <compat/assumptions.h> // IWYU pragma: keep
#include "compat/endian.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "span.h"
#include "prevector.h"

/**
 * The maximum size of a serialized object in bytes or number of elements
 * (for eg vectors) when the size is encoded as CompactSize.
 */
static constexpr uint64_t MAX_SIZE = std::numeric_limits<uint32_t>::max();

/** Maximum amount of memory (in bytes) to allocate at once when deserializing vectors. */
static const unsigned int MAX_VECTOR_ALLOCATE = 5000000;

/**
 * Dummy data type to identify deserializing constructors.
 *
 * By convention, a constructor of a type T with signature
 *
 *   template <typename Stream> T::T(deserialize_type, Stream& s)
 *
 * is a deserializing constructor, which builds the type by
 * deserializing it from s. If T contains const fields, this
 * is likely the only way to do so.
 */
struct deserialize_type {};
constexpr deserialize_type deserialize {};

/**
 * Used to bypass the rule against non-const reference to temporary
 * where it makes sense with wrappers.
 */
template<typename T>
inline T& REF(const T& val)
{
    return const_cast<T&>(val);
}

/**
 * Used to acquire a non-const pointer "this" to generate bodies
 * of const serialization operations from a template
 */
template<typename T>
inline T* NCONST_PTR(const T* val)
{
    return const_cast<T*>(val);
}
template <typename T> inline T *NCONST_PTR_OpNoCSize(const T *val) {
    return const_cast<T *>(val);
}

template<typename Stream, typename = void>
struct has_matching_write : std::false_type {};

template<typename Stream>
struct has_matching_write<Stream, std::void_t<decltype(std::declval<Stream>().write(std::declval<const char*>(), std::declval<size_t>()))>> : std::true_type {};

template<typename Stream,typename Object>
inline typename std::enable_if<has_matching_write<Stream>::value, void>::type
applyWrite(Stream &s, Object obj, uint32_t index)
{
    s.write((char *)&obj, index);
}

template<typename Stream,typename Object>
inline typename std::enable_if<!has_matching_write<Stream>::value, void>::type
applyWrite(Stream &s, Object obj, uint32_t index)
{
    s.write(AsBytes(bsv::span{&obj, 1}));
}

template<typename Stream, typename = void>
struct has_matching_read : std::false_type {};

template<typename Stream>
struct has_matching_read<Stream, std::void_t<decltype(std::declval<Stream>().read(std::declval<char*>(), std::declval<size_t>()))>> : std::true_type {};

template<typename Stream,typename Object>
inline typename std::enable_if<has_matching_read<Stream>::value, void>::type
applyRead(Stream &s, Object& obj, uint32_t index)
{
    s.read((char *)&obj, index);
}

template<typename Stream,typename Object>
inline typename std::enable_if<!has_matching_read<Stream>::value, void>::type
applyRead(Stream &s, Object& obj, uint32_t index)
{
    s.read(bsv::AsWritableBytes(bsv::span{&obj, 1}));
}

/*
 * Lowest-level serialization and conversion.
 * @note Sizes of these types are verified in the tests
 */
template<typename Stream> inline void ser_writedata8(Stream &s, uint8_t obj)
{
    applyWrite(s,obj,1);
}
template<typename Stream> inline void ser_writedata16(Stream &s, uint16_t obj)
{
    obj = htole16(obj);
    applyWrite(s,obj,2);
}
template<typename Stream> inline void ser_writedata32(Stream &s, uint32_t obj)
{
    obj = htole32(obj);
    applyWrite(s,obj,4);
}
template<typename Stream> inline void ser_writedata64(Stream &s, uint64_t obj)
{
    obj = htole64(obj);
    applyWrite(s,obj,8);
}
template<typename Stream> inline uint8_t ser_readdata8(Stream &s)
{
    uint8_t obj;
    applyRead(s,obj,1);
    return obj;
}
template<typename Stream> inline uint16_t ser_readdata16(Stream &s)
{
    uint16_t obj;
    applyRead(s,obj,2);
    return le16toh(obj);
}
template<typename Stream> inline uint32_t ser_readdata32(Stream &s)
{
    uint32_t obj;
    applyRead(s,obj,4);
    return le32toh(obj);
}
template<typename Stream> inline uint64_t ser_readdata64(Stream &s)
{
    uint64_t obj;
    applyRead(s,obj,8);
    return le64toh(obj);
}
inline uint64_t ser_double_to_uint64(double x) {
    union {
        double x;
        uint64_t y;
    } tmp;
    tmp.x = x;
    return tmp.y;
}
inline uint32_t ser_float_to_uint32(float x) {
    union {
        float x;
        uint32_t y;
    } tmp;
    tmp.x = x;
    return tmp.y;
}
inline double ser_uint64_to_double(uint64_t y) {
    union {
        double x;
        uint64_t y;
    } tmp;
    tmp.y = y;
    return tmp.x;
}
inline float ser_uint32_to_float(uint32_t y) {
    union {
        float x;
        uint32_t y;
    } tmp;
    tmp.y = y;
    return tmp.x;
}

/////////////////////////////////////////////////////////////////
//
// Templates for serializing to anything that looks like a stream,
// i.e. anything that supports .read(bsv::span<std::byte>) and .write(bsv::span<const std::byte>)
//
class CSizeComputer;

enum
{
    // primary actions
    SER_NETWORK         = (1 << 0),
    SER_DISK            = (1 << 1),
    SER_GETHASH         = (1 << 2),
};

#define READWRITE_OpNoCSize(obj) (::SerReadWrite_OpNoCSize(s, (obj), ser_action))
#define READWRITE(obj) (::SerReadWrite(s, (obj), ser_action))
#define READWRITECOMPACTSIZE(obj) (::SerReadWriteCompactSize(s, (obj), ser_action))
#define READWRITEMANY(...) (::SerReadWriteMany(s, ser_action, __VA_ARGS__))

/**
 * Implement three methods for serializable objects. These are actually wrappers over
 * "SerializationOp" template, which implements the body of each class' serialization
 * code. Adding "ADD_SERIALIZE_METHODS" in the body of the class causes these wrappers to be
 * added as members.
 */
#define ADD_SERIALIZE_METHODS                                         \
    template<typename Stream>                                         \
    void Serialize(Stream& s) const {                                 \
        NCONST_PTR(this)->SerializationOp(s, CSerActionSerialize());  \
    }                                                                 \
    template<typename Stream>                                         \
    void Unserialize(Stream& s) {                                     \
        SerializationOp(s, CSerActionUnserialize());                  \
    }

/**
 * Implement the Ser and Unser methods needed for implementing a formatter (see Using below).
 *
 * Both Ser and Unser are delegated to a single static method SerializationOps, which is polymorphic
 * in the serialized/deserialized type (allowing it to be const when serializing, and non-const when
 * deserializing).
 *
 * Example use:
 *   struct FooFormatter {
 *     FORMATTER_METHODS(Class, obj) { READWRITE(obj.val1, VARINT(obj.val2)); }
 *   }
 *   would define a class FooFormatter that defines a serialization of Class objects consisting
 *   of serializing its val1 member using the default serialization, and its val2 member using
 *   VARINT serialization. That FooFormatter can then be used in statements like
 *   READWRITE(Using<FooFormatter>(obj.bla)).
 */
#define FORMATTER_METHODS(cls, obj) \
    template<typename Stream> \
    static void Ser(Stream& s, const cls& obj) { SerializationOps(obj, s, CSerActionSerialize()); } \
    template<typename Stream> \
    static void Unser(Stream& s, cls& obj) { SerializationOps(obj, s, CSerActionUnserialize()); } \
    template<typename Stream, typename Type, typename Operation> \
    static inline void SerializationOps(Type& obj, Stream& s, Operation ser_action) \

/**
 * Variant of FORMATTER_METHODS that supports a declared parameter type.
 *
 * If a formatter has a declared parameter type, it must be invoked directly or
 * indirectly with a parameter of that type. This permits making serialization
 * depend on run-time context in a type-safe way.
 *
 * Example use:
 *   struct BarParameter { bool fancy; ... };
 *   struct Bar { ... };
 *   struct FooFormatter {
 *     FORMATTER_METHODS(Bar, obj, BarParameter, param) {
 *       if (param.fancy) {
 *         READWRITE(VARINT(obj.value));
 *       } else {
 *         READWRITE(obj.value);
 *       }
 *     }
 *   };
 * which would then be invoked as
 *   READWRITE(WithParams(BarParameter{...}, Using<FooFormatter>(obj.foo)))
 *
 * WithParams(parameter, obj) can be invoked anywhere in the call stack; it is
 * passed down recursively into all serialization code, until another
 * WithParams overrides it.
 *
 * Parameters will be implicitly converted where appropriate. This means that
 * "parent" serialization code can use a parameter that derives from, or is
 * convertible to, a "child" formatter's parameter type.
 *
 * Compilation will fail in any context where serialization is invoked but
 * no parameter of a type convertible to BarParameter is provided.
 */
#define FORMATTER_METHODS_PARAMS(cls, obj, paramcls, paramobj)                                                 \
    template <typename Stream>                                                                                 \
    static void Ser(Stream& s, const cls& obj) { SerializationOps(obj, s, ActionSerialize{}, s.GetParams()); } \
    template <typename Stream>                                                                                 \
    static void Unser(Stream& s, cls& obj) { SerializationOps(obj, s, ActionUnserialize{}, s.GetParams()); }   \
    template <typename Stream, typename Type, typename Operation>                                              \
    static void SerializationOps(Type& obj, Stream& s, Operation ser_action, const paramcls& paramobj)

#define BASE_SERIALIZE_METHODS(cls)                                                                 \
    template <typename Stream>                                                                      \
    void Serialize(Stream& s) const                                                                 \
    {                                                                                               \
        static_assert(std::is_same<const cls&, decltype(*this)>::value, "Serialize type mismatch"); \
        Ser(s, *this);                                                                              \
    }                                                                                               \
    template <typename Stream>                                                                      \
    void Unserialize(Stream& s)                                                                     \
    {                                                                                               \
        static_assert(std::is_same<cls&, decltype(*this)>::value, "Unserialize type mismatch");     \
        Unser(s, *this);                                                                            \
    }

/**
 * Implement the Serialize and Unserialize methods by delegating to a single templated
 * static method that takes the to-be-(de)serialized object as a parameter. This approach
 * has the advantage that the constness of the object becomes a template parameter, and
 * thus allows a single implementation that sees the object as const for serializing
 * and non-const for deserializing, without casts.
 */
#define SERIALIZE_METHODS(cls, obj) \
    BASE_SERIALIZE_METHODS(cls)     \
    FORMATTER_METHODS(cls, obj)

template <typename Stream> void Serialize(Stream& s, char a) { ser_writedata8(s, a);}
template <typename Stream> void Serialize(Stream& s, std::byte a) { ser_writedata8(s, uint8_t(a)); }
template<typename Stream> inline void Serialize(Stream& s, int8_t a  ) { ser_writedata8(s, a); }
template<typename Stream> inline void Serialize(Stream& s, uint8_t a ) { ser_writedata8(s, a); }
template<typename Stream> inline void Serialize(Stream& s, int16_t a ) { ser_writedata16(s, a); }
template<typename Stream> inline void Serialize(Stream& s, uint16_t a) { ser_writedata16(s, a); }
template<typename Stream> inline void Serialize(Stream& s, int32_t a ) { ser_writedata32(s, a); }
template<typename Stream> inline void Serialize(Stream& s, uint32_t a) { ser_writedata32(s, a); }
template<typename Stream> inline void Serialize(Stream& s, int64_t a ) { ser_writedata64(s, a); }
template<typename Stream> inline void Serialize(Stream& s, uint64_t a) { ser_writedata64(s, a); }
template<typename Stream> inline void Serialize(Stream& s, float a   ) { ser_writedata32(s, ser_float_to_uint32(a)); }
template<typename Stream> inline void Serialize(Stream& s, double a  ) { ser_writedata64(s, ser_double_to_uint64(a)); }
template<typename Stream, int N> inline void Serialize(Stream& s, const char (&a)[N]) { s.write(bsv::MakeByteSpan(a)); }
template<typename Stream, int N> inline void Serialize(Stream& s, const unsigned char (&a)[N]) { s.write(bsv::MakeByteSpan(a)); }
template <typename Stream, typename B, std::size_t N> void Serialize(Stream& s, const std::array<B, N>& a) { (void)/* force byte-type */bsv::UCharCast(a.data()); s.write(bsv::MakeByteSpan(a)); }
template <typename Stream, typename B> void Serialize(Stream& s, bsv::span<B> span) { (void)/* force byte-type */bsv::UCharCast(span.data()); s.write(AsBytes(span)); }

template <typename Stream> void Unserialize(Stream& s, char& a) { a = ser_readdata8(s);}
template <typename Stream> void Unserialize(Stream& s, std::byte& a) { a = std::byte{ser_readdata8(s)}; }
template<typename Stream> inline void Unserialize(Stream& s, int8_t& a  ) { a = ser_readdata8(s); }
template<typename Stream> inline void Unserialize(Stream& s, uint8_t& a ) { a = ser_readdata8(s); }
template<typename Stream> inline void Unserialize(Stream& s, int16_t& a ) { a = ser_readdata16(s); }
template<typename Stream> inline void Unserialize(Stream& s, uint16_t& a) { a = ser_readdata16(s); }
template<typename Stream> inline void Unserialize(Stream& s, int32_t& a ) { a = ser_readdata32(s); }
template<typename Stream> inline void Unserialize(Stream& s, uint32_t& a) { a = ser_readdata32(s); }
template<typename Stream> inline void Unserialize(Stream& s, int64_t& a ) { a = ser_readdata64(s); }
template<typename Stream> inline void Unserialize(Stream& s, uint64_t& a) { a = ser_readdata64(s); }
template<typename Stream> inline void Unserialize(Stream& s, float& a   ) { a = ser_uint32_to_float(ser_readdata32(s)); }
template<typename Stream> inline void Unserialize(Stream& s, double& a  ) { a = ser_uint64_to_double(ser_readdata64(s)); }
template<typename Stream, int N> inline void Unserialize(Stream& s, char (&a)[N]) { s.read(bsv::MakeWritableByteSpan(a)); }
template<typename Stream, int N> inline void Unserialize(Stream& s, unsigned char (&a)[N]) { s.read(bsv::MakeWritableByteSpan(a)); }
template <typename Stream, typename B, std::size_t N> void Unserialize(Stream& s, std::array<B, N>& a) { (void)/* force byte-type */bsv::UCharCast(a.data()); s.read(bsv::MakeWritableByteSpan(a)); }
template <typename Stream, typename B> void Unserialize(Stream& s, bsv::span<B> span) { (void)/* force byte-type */bsv::UCharCast(span.data()); s.read(bsv::AsWritableBytes(span)); }

template <typename Stream> inline void Serialize(Stream& s, bool a) { uint8_t f = a; ser_writedata8(s, f); }
template <typename Stream> inline void Unserialize(Stream& s, bool& a) { uint8_t f = ser_readdata8(s); a = f; }
// clang-format on

/**
 * Compact Size
 * size <  253        -- 1 byte
 * size <= USHRT_MAX  -- 3 bytes  (253 + 2 bytes)
 * size <= UINT_MAX   -- 5 bytes  (254 + 4 bytes)
 * size >  UINT_MAX   -- 9 bytes  (255 + 8 bytes)
 */
inline uint32_t GetSizeOfCompactSize(uint64_t nSize)
{
    if (nSize < 253)
    {return sizeof(uint8_t);}
    else if (nSize <= std::numeric_limits<uint16_t>::max()) return sizeof(uint8_t) + sizeof(uint16_t);
    else if (nSize <= std::numeric_limits<uint32_t>::max()) return sizeof(uint8_t) + sizeof(uint32_t);
    else                             return sizeof(uint8_t) + sizeof(uint64_t);
}

inline void WriteCompactSize(CSizeComputer& os, uint64_t nSize);

template<typename Stream>
void WriteCompactSize(Stream& os, uint64_t nSize)
{
    if (nSize > MAX_SIZE) {
          throw std::ios_base::failure("WriteCompactSize(): size too large");
    }
    if (nSize < 253)
    {
        ser_writedata8(os, nSize);
    }
    else if (nSize <= std::numeric_limits<uint16_t>::max())
    {
        ser_writedata8(os, 253);
        ser_writedata16(os, nSize);
    }
    else if (nSize <= std::numeric_limits<uint32_t>::max())
    {
        ser_writedata8(os, 254);
        ser_writedata32(os, nSize);
    }
    else
    {
        ser_writedata8(os, 255);
        ser_writedata64(os, nSize);
    }
    return;
}

/**
 * Decode a CompactSize-encoded variable-length integer.
 *
 * As these are primarily used to encode the size of vector-like serializations, by default a range
 * check is performed. When used as a generic number encoding, range_check should be set to false.
 */
template<typename Stream>
uint64_t ReadCompactSize(Stream& is, bool range_check = true)
{
    uint8_t chSize = ser_readdata8(is);
    uint64_t nSizeRet = 0;
    if (chSize < 253)
    {
        nSizeRet = chSize;
    }
    else if (chSize == 253)
    {
        nSizeRet = ser_readdata16(is);
        if (nSizeRet < 253)
            throw std::ios_base::failure("non-canonical ReadCompactSize()");
    }
    else if (chSize == 254)
    {
        nSizeRet = ser_readdata32(is);
        if (nSizeRet < 0x10000u)
            throw std::ios_base::failure("non-canonical ReadCompactSize()");
    }
    else
    {
        nSizeRet = ser_readdata64(is);
        if (nSizeRet < 0x100000000ULL)
            throw std::ios_base::failure("non-canonical ReadCompactSize()");
    }
    if (range_check && nSizeRet > MAX_SIZE) {
        throw std::ios_base::failure("ReadCompactSize(): size too large");
    }
    return nSizeRet;
}

/**
 * Variable-length integers: bytes are a MSB base-128 encoding of the number.
 * The high bit in each byte signifies whether another digit follows. To make
 * sure the encoding is one-to-one, one is subtracted from all but the last digit.
 * Thus, the byte sequence a[] with length len, where all but the last byte
 * has bit 128 set, encodes the number:
 *
 *  (a[len-1] & 0x7F) + sum(i=1..len-1, 128^i*((a[len-i-1] & 0x7F)+1))
 *
 * Properties:
 * * Very small (0-127: 1 byte, 128-16511: 2 bytes, 16512-2113663: 3 bytes)
 * * Every integer has exactly one encoding
 * * Encoding does not depend on size of original integer type
 * * No redundancy: every (infinite) byte sequence corresponds to a list
 *   of encoded integers.
 *
 * 0:         [0x00]  256:        [0x81 0x00]
 * 1:         [0x01]  16383:      [0xFE 0x7F]
 * 127:       [0x7F]  16384:      [0xFF 0x00]
 * 128:  [0x80 0x00]  16511:      [0xFF 0x7F]
 * 255:  [0x80 0x7F]  65535: [0x82 0xFE 0x7F]
 * 2^32:           [0x8E 0xFE 0xFE 0xFF 0x00]
 */
template<typename I>
inline unsigned int GetSizeOfVarInt(I n)
{
    int nRet = 0;
    while(true) {
        nRet++;
        if (n <= 0x7F)
            break;
        n = (n >> 7) - 1;
    }
    return nRet;
}

template<typename I>
inline void WriteVarInt(CSizeComputer& os, I n);

template <typename Stream, typename I, class = typename std::enable_if<std::is_integral<I>::value>::type>
void WriteVarInt(Stream& os, I n)
{
    uint8_t tmp[(sizeof(n)*8+6)/7];
    int len = 0;
    while(true) {
        tmp[len] = (n & 0x7F) | (len ? 0x80 : 0x00);
        if (n <= 0x7F)
        {
            break;
        }
        n = (n >> 7) - 1;
        len++;
    }
    do {
        ser_writedata8(os, tmp[len]);
    } while(len--);
}

template <typename Stream, typename I,class = typename std::enable_if<std::is_integral<I>::value>::type>
I ReadVarInt(Stream &is) {
    uintmax_t n {0};
    // VarInt encoding is only defined for unsigned integers. However there are places in source code
    // where ReadVarInt is called with a signed integer type (such as when serializing CDiskBlockPos)
    // Those places need to make sure that the actual values are always non-negative. 
    // Static cast in the following line if required to make MSVC compiler happy.
    // It is safe, because the value that is being casted is always positive and will always
    // fit in the unsigned version of type. 
    static uintmax_t overflow { static_cast<uintmax_t>(std::numeric_limits<I>::max() >> 7) };

    unsigned int maxSize = (sizeof(n) * 8 + 6) / 7;
    for (unsigned int i = 0; i<maxSize; ++i){
        if (n > overflow){
            throw std::runtime_error ("Deserialisation Error ReadVarInt");
        }
        uint8_t chData = ser_readdata8(is);
        n = (n << 7) | (chData & 0x7F);
        if ((chData & 0x80) == 0) {
            return n ;
        }
        n++;
    }
    // If we make it to hear its a deserialisation error
    // throw an exception
    throw std::runtime_error ("Deserialisation Error ReadVarInt");
}

#define FLATDATA(obj)                                                          \
    REF(CFlatData((char *)&(obj), (char *)&(obj) + sizeof(obj)))
#define VARINT(obj) REF(WrapVarInt(REF(obj)))
#define COMPACTSIZE(obj) REF(CCompactSize(REF(obj)))
#define LIMITED_STRING(obj, n) REF(LimitedString<n>(REF(obj)))

/**
 * Wrapper for serializing arrays and POD.
 */
class CFlatData
{
protected:
    char* pbegin;
    char* pend;
public:
    CFlatData(void* pbeginIn, void* pendIn) : pbegin((char*)pbeginIn), pend((char*)pendIn) { }
    template <class T, class TAl>
    explicit CFlatData(std::vector<T,TAl> &v)
    {
        pbegin = (char*)v.data();
        pend = (char*)(v.data() + v.size());
    }
    template <unsigned int N, typename T, typename S, typename D>
    explicit CFlatData(prevector<N, T, S, D> &v)
    {
        pbegin = (char*)v.data();
        pend = (char*)(v.data() + v.size());
    }
    char* begin() { return pbegin; }
    const char* begin() const { return pbegin; }
    char* end() { return pend; }
    const char* end() const { return pend; }

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(pbegin, pend - pbegin);
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        s.read(pbegin, pend - pbegin);
    }
};

template <typename I> 
class CVarInt 
{
protected:
    I &n;
public:
    explicit CVarInt(I& nIn) : n(nIn) { }

    template<typename Stream>
    void Serialize(Stream &s) const {
        WriteVarInt<Stream, I>(s, n);
    }

    template<typename Stream>
    void Unserialize(Stream& s) {
        n = ReadVarInt<Stream, I>(s);
    }
};

class CCompactSize {
protected:
    uint64_t &n;

public:
    CCompactSize(uint64_t &nIn) : n(nIn) {}

    template <typename Stream> void Serialize(Stream &s) const {
        WriteCompactSize<Stream>(s, n);
    }

    template <typename Stream> void Unserialize(Stream &s) {
        n = ReadCompactSize<Stream>(s);
    }
};

template <size_t Limit> class LimitedString {
protected:
    std::string &string;

public:
    LimitedString(std::string &_string) : string(_string) {}

    template <typename Stream> void Unserialize(Stream &s) {
        size_t size = ReadCompactSize(s);
        if (size > Limit) {
            throw std::ios_base::failure("String length limit exceeded");
        }
        string.resize(size);
        if (size != 0) {
            s.read(bsv::MakeWritableByteSpan(string));
        }
    }

    template <typename Stream> void Serialize(Stream &s) const {
        WriteCompactSize(s, string.size());
        if (!string.empty()) {
            s.write(bsv::MakeByteSpan(string));
        }
    }
};

template <typename I> CVarInt<I> WrapVarInt(I &n) {
    return CVarInt<I>(n);
}

/**
 * Forward declarations
 */

/**
 *  string
 */
template<typename Stream, typename C> void Serialize(Stream& os, const std::basic_string<C>& str);
template<typename Stream, typename C> void Unserialize(Stream& is, std::basic_string<C>& str);

/**
 * prevector
 * prevectors of uint8_t are a special case and are intended to be serialized as
 * a single opaque blob.
 */
template <typename Stream, unsigned int N, typename T>
void Serialize_impl(Stream &os, const prevector<N, T> &v, const uint8_t &);
template <typename Stream, unsigned int N, typename T, typename V>
void Serialize_impl(Stream &os, const prevector<N, T> &v, const V &);
template <typename Stream, unsigned int N, typename T>
inline void Serialize(Stream &os, const prevector<N, T> &v);
template <typename Stream, unsigned int N, typename T>
void Unserialize_impl(Stream &is, prevector<N, T> &v, const uint8_t &);
template <typename Stream, unsigned int N, typename T, typename V>
void Unserialize_impl(Stream &is, prevector<N, T> &v, const V &);
template <typename Stream, unsigned int N, typename T>
inline void Unserialize(Stream &is, prevector<N, T> &v);



/**
 * vector
 * vectors of uint8_t are a special case and are intended to be serialized as a
 * single opaque blob.
 */
template <typename Stream, typename T, typename A>
void Serialize_impl(Stream &os, const std::vector<T, A> &v, const uint8_t &);
template <typename Stream, typename T, typename A, typename V>
void Serialize_impl(Stream &os, const std::vector<T, A> &v, const V &);
template <typename Stream, typename T, typename A>
inline void Serialize(Stream &os, const std::vector<T, A> &v);
template <typename Stream, typename T, typename A>
void Unserialize_impl(Stream &is, std::vector<T, A> &v, const uint8_t &);
template <typename Stream, typename T, typename A, typename V>
void Unserialize_impl(Stream &is, std::vector<T, A> &v, const V &);
template <typename Stream, typename T, typename A>
inline void Unserialize(Stream &is, std::vector<T, A> &v);

/**
 * pair
 */
template <typename Stream, typename K, typename T>
void Serialize(Stream &os, const std::pair<K, T> &item);
template <typename Stream, typename K, typename T>
void Unserialize(Stream &is, std::pair<K, T> &item);

/**
 * map
 */
template <typename Stream, typename K, typename T, typename Pred, typename A>
void Serialize(Stream &os, const std::map<K, T, Pred, A> &m);
template <typename Stream, typename K, typename T, typename Pred, typename A>
void Unserialize(Stream &is, std::map<K, T, Pred, A> &m);

/**
 * set
 */
template <typename Stream, typename K, typename Pred, typename A>
void Serialize(Stream &os, const std::set<K, Pred, A> &m);
template <typename Stream, typename K, typename Pred, typename A>
void Unserialize(Stream &is, std::set<K, Pred, A> &m);

/**
 * shared_ptr
 */
template <typename Stream, typename T>
void Serialize(Stream &os, const std::shared_ptr<const T> &p);
template <typename Stream, typename T>
void Unserialize(Stream &os, std::shared_ptr<const T> &p);

/**
 * unique_ptr
 */
template <typename Stream, typename T>
void Serialize(Stream &os, const std::unique_ptr<const T> &p);
template <typename Stream, typename T>
void Unserialize(Stream &os, std::unique_ptr<const T> &p);

/**
 * If none of the specialized versions above matched, default to calling member
 * function.
 */
template<typename Stream, typename T>
inline void Serialize(Stream& os, const T& a)
{
    a.Serialize(os);
}

template<typename Stream, typename T>
inline void Unserialize(Stream& is, T&& a)
{
    a.Unserialize(is);
}

/**
 * string
 */
template<typename Stream, typename C>
void Serialize(Stream& os, const std::basic_string<C>& str)
{
    WriteCompactSize(os, str.size());
    if (!str.empty())
        os.write(bsv::MakeByteSpan(str));
}

template<typename Stream, typename C>
void Unserialize(Stream& is, std::basic_string<C>& str)
{
    size_t nSize = ReadCompactSize(is);
    str.resize(nSize);
    if (nSize != 0)
        is.read(bsv::MakeWritableByteSpan(str));
}

/**
 * prevector
 */
template <typename Stream, unsigned int N, typename T>
void Serialize_impl(Stream &os, const prevector<N, T> &v, const uint8_t &) {
    WriteCompactSize(os, v.size());
    if (!v.empty())
    {
        os.write(bsv::MakeByteSpan(v));
    }
}

template <typename Stream, unsigned int N, typename T, typename V>
void Serialize_impl(Stream &os, const prevector<N, T> &v, const V &) {
    WriteCompactSize(os, v.size());
    for (const T &i : v) {
        ::Serialize(os, i);
    }
}
template <typename Stream, unsigned int N, typename T, typename V>
void Serialize_impl_OpNoCSize(Stream &os, const prevector<N, T> &v, const V &) {
    for (const T &i : v) {
        ::Serialize(os, i);
    }
}

template <typename Stream, unsigned int N, typename T>
inline void Serialize(Stream &os, const prevector<N, T> &v) {
    Serialize_impl(os, v, T());
}

template <typename Stream, unsigned int N, typename T>
inline void Serialize_OpNoCSize(Stream &os, const prevector<N, T> &v) {
    Serialize_impl_OpNoCSize(os, v, T());
}

constexpr size_t STARTING_CHUNK_SIZE = 16000000; // 16MB
constexpr size_t CHUNK_GROWTH_RATE = 3;

template <typename Stream, unsigned int N, typename T>
void Unserialize_impl(Stream &is, prevector<N, T> &v, const uint8_t &) {
    // Limit size per read so bogus size value won't cause out of memory
    v.clear();
    size_t nSize = ReadCompactSize(is);
    size_t i = 0;
    size_t chunkSize = STARTING_CHUNK_SIZE;
    while (i < nSize) {
        size_t blk = std::min(nSize - i, size_t(1 + (chunkSize - 1) / sizeof(T)));
        chunkSize *= CHUNK_GROWTH_RATE;
        v.resize(i + blk);
        applyRead(is,v[i],blk * sizeof(T));
        i += blk;
    }
}

template <typename Stream, unsigned int N, typename T, typename V>
void Unserialize_impl(Stream &is, prevector<N, T> &v, const V &) {
    v.clear();
    size_t nSize = ReadCompactSize(is);
    size_t i = 0;
    size_t nMid = 0;
    size_t chunkSize = STARTING_CHUNK_SIZE;
    while (nMid < nSize) {
        nMid += std::min(nSize, size_t(1 + (chunkSize - 1) / sizeof(T)));
        chunkSize *= CHUNK_GROWTH_RATE;
        if (nMid > nSize) {
            nMid = nSize;
        }
        v.resize(nMid);
        for (; i < nMid; i++) {
            Unserialize(is, v[i]);
        }
    }
}

template<typename Stream, unsigned int N, typename T>
inline void Unserialize(Stream& is, prevector<N, T>& v)
{
    Unserialize_impl(is, v, T());
}

/**
 * vector
 */
template <typename Stream, typename T, typename A>
void Serialize_impl(Stream &os, const std::vector<T, A> &v, const uint8_t &) {
    WriteCompactSize(os, v.size());
    if (!v.empty()) {
        os.write((char *)&v[0], v.size() * sizeof(T));
    }
}

template <typename Stream, typename T, typename A, typename V>
void Serialize_impl(Stream &os, const std::vector<T, A> &v, const V &) {
    WriteCompactSize(os, v.size());
    for (const T &i : v) {
        ::Serialize(os, i);
    }
}

template <typename Stream, typename T, typename A>
inline void Serialize(Stream &os, const std::vector<T, A> &v) {
    Serialize_impl(os, v, T());
}

template <typename Stream, typename T, typename A>
void Unserialize_impl(Stream &is, std::vector<T, A> &v, const uint8_t &) {
    // Limit size per read so bogus size value won't cause out of memory
    v.clear();
    size_t nSize = ReadCompactSize(is);
    size_t i = 0;
    size_t chunkSize = STARTING_CHUNK_SIZE;
    while (i < nSize) {
        size_t blk = std::min(nSize - i, size_t(1 + (chunkSize - 1) / sizeof(T)));
        chunkSize *= CHUNK_GROWTH_RATE;
        v.resize(i + blk);
        applyRead(is,v[i],blk * sizeof(T));
        i += blk;
    }
}


template <typename Stream, typename T, typename A, typename V>
void Unserialize_impl(Stream &is, std::vector<T, A> &v, const V &) {
    v.clear();
    size_t nSize = ReadCompactSize(is);
    size_t i = 0;
    size_t nMid = 0;
    size_t chunkSize = STARTING_CHUNK_SIZE;
    while (nMid < nSize) {
        nMid += std::min(nSize, size_t(1 + (chunkSize - 1) / sizeof(T)));
        chunkSize *= CHUNK_GROWTH_RATE;
        if (nMid > nSize) {
            nMid = nSize;
        }
        v.resize(nMid);
        for (; i < nMid; i++) {
            Unserialize(is, v[i]);
        }
    }
}


template <typename Stream, typename T, typename A>
inline void Unserialize(Stream &is, std::vector<T, A> &v) {
    Unserialize_impl(is, v, T());
}

/**
 * pair
 */
template<typename Stream, typename K, typename T>
void Serialize(Stream& os, const std::pair<K, T>& item)
{
    Serialize(os, item.first);
    Serialize(os, item.second);
}

template<typename Stream, typename K, typename T>
void Unserialize(Stream& is, std::pair<K, T>& item)
{
    Unserialize(is, item.first);
    Unserialize(is, item.second);
}

/**
 * map
 */
template<typename Stream, typename K, typename T, typename Pred, typename A>
void Serialize(Stream& os, const std::map<K, T, Pred, A>& m)
{
    WriteCompactSize(os, m.size());
    for (const std::pair<K, T> &p : m) {
        Serialize(os, p);
    }
}

template<typename Stream, typename K, typename T, typename Pred, typename A>
void Unserialize(Stream& is, std::map<K, T, Pred, A>& m)
{
    m.clear();
    size_t nSize = ReadCompactSize(is);
    typename std::map<K, T, Pred, A>::iterator mi = m.begin();
    for (size_t i = 0; i < nSize; i++) {
        std::pair<K, T> item;
        Unserialize(is, item);
        mi = m.insert(mi, item);
    }
}

/**
 * set
 */
template<typename Stream, typename K, typename Pred, typename A>
void Serialize(Stream& os, const std::set<K, Pred, A>& m)
{
    WriteCompactSize(os, m.size());
    for (const K &i : m) {
        Serialize(os, i);
    }
}

template<typename Stream, typename K, typename Pred, typename A>
void Unserialize(Stream& is, std::set<K, Pred, A>& m)
{
    m.clear();
    size_t nSize = ReadCompactSize(is);
    typename std::set<K, Pred, A>::iterator it = m.begin();
    for (size_t i = 0; i < nSize; i++) 
    {
        K key;
        Unserialize(is, key);
        it = m.insert(it, key);
    }
}

/**
 * unique_ptr
 */
template<typename Stream, typename T> void
Serialize(Stream& os, const std::unique_ptr<const T>& p)
{
    Serialize(os, *p);
}

template<typename Stream, typename T>
void Unserialize(Stream& is, std::unique_ptr<const T>& p)
{
    p.reset(new T(deserialize, is));
}

/**
 * shared_ptr
 */
template<typename Stream, typename T> void
Serialize(Stream& os, const std::shared_ptr<const T>& p)
{
    Serialize(os, *p);
}

template<typename Stream, typename T>
void Unserialize(Stream& is, std::shared_ptr<const T>& p)
{
    p = std::make_shared<const T>(deserialize, is);
}

/**
 * Support for ADD_SERIALIZE_METHODS and READWRITE macro
 */
struct CSerActionSerialize
{
    constexpr bool ForRead() const { return false; }
};
struct CSerActionUnserialize
{
    constexpr bool ForRead() const { return true; }
};

template<typename Stream, typename T>
inline void SerReadWrite(Stream& s, const T& obj, CSerActionSerialize ser_action)
{
    ::Serialize(s, obj);
}
template <typename Stream, typename T>
inline void SerReadWrite_OpNoCSize(Stream &s, const T &obj,
                         CSerActionSerialize ser_action) {
    ::Serialize_OpNoCSize(s, obj);
}


template <typename Stream, typename T>
inline void SerReadWrite(Stream &s, T &obj, CSerActionUnserialize ser_action) {
    ::Unserialize(s, obj);
}

/**
 * Support for READWRITECOMPACTSIZE macro
 */

template <typename Stream>
inline void SerReadWriteCompactSize(Stream &s, const uint64_t &obj,
                         CSerActionSerialize ser_action) {
    ::WriteCompactSize(s, obj);
}

template <typename Stream>
inline void SerReadWriteCompactSize(Stream &s, uint64_t &obj, CSerActionUnserialize ser_action) {
    obj = ::ReadCompactSize(s);
}

/**
 * ::GetSerializeSize implementations
 *
 * Computing the serialized size of objects is done through a special stream
 * object of type CSizeComputer, which only records the number of bytes written
 * to it.
 *
 * If your Serialize or SerializationOp method has non-trivial overhead for
 * serialization, it may be worthwhile to implement a specialized version for
 * CSizeComputer, which uses the s.seek() method to record bytes that would
 * be written instead.
 */
class CSizeComputer
{
protected:
    size_t nSize;

    const int nType;
    const int nVersion;

public:
    CSizeComputer(int nTypeIn, int nVersionIn)
        : nSize(0), nType(nTypeIn), nVersion(nVersionIn) {}

    void write(const char *psz, size_t _nSize) { this->nSize += _nSize; }

    void write(bsv::span<const std::byte> src)
    {
        this->nSize += src.size();
    }

    /** Pretend _nSize bytes are written, without specifying them. */
    void seek(size_t _nSize)
    {
        this->nSize += _nSize;
    }

    template<typename T>
    CSizeComputer& operator<<(const T& obj)
    {
        ::Serialize(*this, obj);
        return (*this);
    }

    size_t size() const {
        return nSize;
    }

    int GetVersion() const { return nVersion; }
    int GetType() const { return nType; }
};

template <typename Stream> void SerializeMany(Stream &s) {}

template<typename Stream, typename Arg>
void SerializeMany(Stream& s, Arg&& arg)
{
    ::Serialize(s, std::forward<Arg>(arg));
}

template <typename Stream, typename Arg, typename... Args>
void SerializeMany(Stream &s, Arg &&arg, Args &&... args) {
    ::Serialize(s, std::forward<Arg>(arg));
    ::SerializeMany(s, std::forward<Args>(args)...);
}

template <typename Stream> inline void UnserializeMany(Stream &s) {}

template <typename Stream, typename Arg>
inline void UnserializeMany(Stream &s, Arg &arg) {
    ::Unserialize(s, arg);
}

template <typename Stream, typename Arg, typename... Args>
inline void UnserializeMany(Stream &s, Arg &arg, Args &... args) {
    ::Unserialize(s, arg);
    ::UnserializeMany(s, args...);
}

template <typename Stream, typename... Args>
inline void SerReadWriteMany(Stream &s, CSerActionSerialize ser_action,
                             Args &&... args) {
    ::SerializeMany(s, std::forward<Args>(args)...);
}

template <typename Stream, typename... Args>
inline void SerReadWriteMany(Stream &s, CSerActionUnserialize ser_action,
                             Args &... args) {
    ::UnserializeMany(s, args...);
}

template <typename I> inline void WriteVarInt(CSizeComputer &s, I n) {
    s.seek(GetSizeOfVarInt<I>(n));
}

inline void WriteCompactSize(CSizeComputer &s, uint64_t nSize)
{
    s.seek(GetSizeOfCompactSize(nSize));
}

template <typename T>
size_t GetSerializeSize(const T& t, int nType, int nVersion = 0)
{
    return (CSizeComputer(nType, nVersion) << t).size();
}

template <typename S, typename T>
size_t GetSerializeSize(const S& s, const T& t)
{
    return (CSizeComputer(s.GetType(), s.GetVersion()) << t).size();
}

#endif // BITCOIN_SERIALIZE_H
