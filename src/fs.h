// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_FS_H
#define BITCOIN_FS_H

#include <stdio.h>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/filesystem/detail/utf8_codecvt_facet.hpp>
#include <boost/filesystem/fstream.hpp>

/** Filesystem operations and types */
namespace fs = boost::filesystem;

/** Bridge operations to C stdio */
namespace fsbridge {
FILE *fopen(const fs::path &p, const char *mode);
FILE *freopen(const fs::path &p, const char *mode, FILE *stream);
};

namespace bitcoinfs {
/**
 * Convert path object to a byte string. On POSIX, paths natively are byte
 * strings, so this is trivial. On Windows, paths natively are Unicode, so an
 * encoding step is necessary. The inverse of \ref PathToString is \ref
 * PathFromString. The strings returned and parsed by these functions can be
 * used to call POSIX APIs, and for roundtrip conversion, logging, and
 * debugging.
 *
 * Because \ref PathToString and \ref PathFromString functions don't specify an
 * encoding, they are meant to be used internally, not externally. They are not
 * appropriate to use in applications requiring UTF-8, where
 * fs::path::u8string() / fs::path::utf8string() and fs::u8path() methods should be used instead. Other
 * applications could require still different encodings. For example, JSON, XML,
 * or URI applications might prefer to use higher-level escapes (\uXXXX or
 * &XXXX; or %XX) instead of multibyte encoding. Rust, Python, Java applications
 * may require encoding paths with their respective UTF-8 derivatives WTF-8,
 * PEP-383, and CESU-8 (see https://en.wikipedia.org/wiki/UTF-8#Derivatives).
 */
static inline std::string PathToString(const fs::path& path)
{
    // Implementation note: On Windows, the std::filesystem::path(string)
    // constructor and std::filesystem::path::string() method are not safe to
    // use here, because these methods encode the path using C++'s narrow
    // multibyte encoding, which on Windows corresponds to the current "code
    // page", which is unpredictable and typically not able to represent all
    // valid paths. So fs::path::utf8string() and
    // fs::u8path() functions are used instead on Windows. On
    // POSIX, u8string/utf8string/u8path functions are not safe to use because paths are
    // not always valid UTF-8, so plain string methods which do not transform
    // the path there are used.
#ifdef WIN32
    const std::u8string& utf8_str{fs::path::u8string()};
    return std::string{utf8_str.begin(), utf8_str.end()};
#else
    static_assert(std::is_same<fs::path::string_type, std::string>::value, "PathToString not implemented on this platform");
    return path.string();
#endif
}
}

#endif
