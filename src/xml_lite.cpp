#include "xml_lite.hpp"

#include <cstdlib>

namespace slims3::detail {
namespace {

// Inner text of the first <tag>...</tag> at or after `from`.
// S3 list/error documents never put attributes on these tags, so the plain
// form "<tag>" is matched; this is a targeted extractor, not an XML parser.
bool innerText(std::string_view xml, std::string_view tag, std::size_t from, std::string_view& out,
               std::size_t& endPos) {
    std::string open = "<" + std::string(tag) + ">";
    std::string close = "</" + std::string(tag) + ">";
    std::size_t s = xml.find(open, from);
    if (s == std::string_view::npos)
        return false;
    s += open.size();
    std::size_t e = xml.find(close, s);
    if (e == std::string_view::npos)
        return false;
    out = xml.substr(s, e - s);
    endPos = e + close.size();
    return true;
}

std::string stripQuotes(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

} // namespace

std::string xmlUnescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') {
            out += s[i++];
            continue;
        }
        std::size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 10) {
            out += s[i++];
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "lt")
            out += '<';
        else if (ent == "gt")
            out += '>';
        else if (ent == "amp")
            out += '&';
        else if (ent == "quot")
            out += '"';
        else if (ent == "apos")
            out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            long code = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                            ? std::strtol(std::string(ent.substr(2)).c_str(), nullptr, 16)
                            : std::strtol(std::string(ent.substr(1)).c_str(), nullptr, 10);
            // Surrogates (0xD800-0xDFFF) are not valid Unicode scalar values and
            // must never be encoded to UTF-8; reject them the same way as any
            // other out-of-range code point.
            if (code <= 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
                out += s[i++];
                continue;
            }
            // Encode the code point as UTF-8.
            unsigned cp = unsigned(code);
            if (cp < 0x80)
                out += char(cp);
            else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            } else {
                out += char(0xF0 | (cp >> 18));
                out += char(0x80 | ((cp >> 12) & 0x3F));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
        } else {
            out += s[i++]; // unknown entity: emit '&' and continue scanning
            continue;
        }
        i = semi + 1;
    }
    return out;
}

bool parseErrorBody(std::string_view xml, S3ErrorBody& out) {
    if (xml.find("<Error>") == std::string_view::npos &&
        xml.find("<Error ") == std::string_view::npos)
        return false;
    std::string_view v;
    std::size_t end;
    if (innerText(xml, "Code", 0, v, end))
        out.code = xmlUnescape(v);
    if (innerText(xml, "Message", 0, v, end))
        out.message = xmlUnescape(v);
    return !out.code.empty();
}

bool parseListPage(std::string_view xml, ListPage& out) {
    if (xml.find("<ListBucketResult") == std::string_view::npos)
        return false;
    // Reject documents with an opened-but-unclosed Contents block.
    std::size_t pos = 0;
    std::string_view block;
    std::size_t end;
    std::size_t contentsOpens = 0, contentsParsed = 0;
    for (std::size_t p = xml.find("<Contents>"); p != std::string_view::npos;
         p = xml.find("<Contents>", p + 1))
        ++contentsOpens;
    while (innerText(xml, "Contents", pos, block, end)) {
        pos = end;
        ++contentsParsed;
        ListEntry e;
        std::string_view v;
        std::size_t be;
        if (!innerText(block, "Key", 0, v, be))
            return false;
        e.key = xmlUnescape(v);
        if (innerText(block, "Size", 0, v, be))
            e.size = std::strtoull(std::string(v).c_str(), nullptr, 10);
        if (innerText(block, "ETag", 0, v, be))
            e.etag = stripQuotes(xmlUnescape(v));
        out.entries.push_back(std::move(e));
    }
    if (contentsParsed != contentsOpens)
        return false;
    // Reject documents with an opened-but-unclosed CommonPrefixes block, same
    // as the Contents check above.
    std::size_t prefixOpens = 0, prefixParsed = 0;
    for (std::size_t p = xml.find("<CommonPrefixes>"); p != std::string_view::npos;
         p = xml.find("<CommonPrefixes>", p + 1))
        ++prefixOpens;
    pos = 0;
    while (innerText(xml, "CommonPrefixes", pos, block, end)) {
        pos = end;
        ++prefixParsed;
        std::string_view v;
        std::size_t be;
        if (!innerText(block, "Prefix", 0, v, be))
            return false;
        ListEntry e;
        e.key = xmlUnescape(v);
        e.isPrefix = true;
        out.entries.push_back(std::move(e));
    }
    if (prefixParsed != prefixOpens)
        return false;
    std::string_view v;
    if (innerText(xml, "IsTruncated", 0, v, end))
        out.truncated = (v == "true");
    if (innerText(xml, "NextContinuationToken", 0, v, end))
        out.nextToken = xmlUnescape(v);
    return true;
}

} // namespace slims3::detail
