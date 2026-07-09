#include "doctest.h"
#include "xml_lite.hpp"

using namespace slims3::detail;

static const char* kListSample = R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>bkt</Name><Prefix>data/</Prefix><KeyCount>2</KeyCount><MaxKeys>1000</MaxKeys>
  <EncodingType>url</EncodingType>
  <IsTruncated>true</IsTruncated>
  <NextContinuationToken>1/wJ+=abc</NextContinuationToken>
  <Contents>
    <Key>data/file%20one.rss</Key>
    <LastModified>2026-07-09T12:00:00.000Z</LastModified>
    <ETag>&quot;9bb58f26192e4ba00f01e2e7b136bbd8&quot;</ETag>
    <Size>9672</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
  <Contents>
    <Key>data/two</Key><ETag>"abc"</ETag><Size>0</Size>
  </Contents>
  <CommonPrefixes><Prefix>data/sub%2Fdir/</Prefix></CommonPrefixes>
</ListBucketResult>)";

TEST_CASE("parseListPage: entries, prefixes, truncation") {
    ListPage p;
    REQUIRE(parseListPage(kListSample, p));
    REQUIRE(p.entries.size() == 3);
    CHECK(p.entries[0].key == "data/file%20one.rss"); // still URL-encoded at this layer
    CHECK(p.entries[0].size == 9672);
    CHECK(p.entries[0].etag == "9bb58f26192e4ba00f01e2e7b136bbd8"); // quotes stripped
    CHECK_FALSE(p.entries[0].isPrefix);
    CHECK(p.entries[1].etag == "abc");
    CHECK(p.entries[2].isPrefix);
    CHECK(p.entries[2].key == "data/sub%2Fdir/");
    CHECK(p.truncated);
    CHECK(p.nextToken == "1/wJ+=abc");
}

TEST_CASE("parseListPage: empty result, not truncated") {
    ListPage p;
    REQUIRE(
        parseListPage("<ListBucketResult><IsTruncated>false</IsTruncated></ListBucketResult>", p));
    CHECK(p.entries.empty());
    CHECK_FALSE(p.truncated);
    CHECK(p.nextToken.empty());
}

TEST_CASE("parseErrorBody") {
    S3ErrorBody e;
    REQUIRE(
        parseErrorBody("<?xml version=\"1.0\"?><Error><Code>NoSuchKey</Code>"
                       "<Message>The specified key does not exist.</Message><Key>x</Key></Error>",
                       e));
    CHECK(e.code == "NoSuchKey");
    CHECK(e.message == "The specified key does not exist.");
    CHECK_FALSE(parseErrorBody("<NotAnError/>", e));
    CHECK_FALSE(parseErrorBody("plain text, no xml", e));
}

TEST_CASE("xmlUnescape: predefined entities and numeric refs") {
    CHECK(xmlUnescape("a&lt;b&gt;c&amp;d&quot;e&apos;f") == "a<b>c&d\"e'f");
    CHECK(xmlUnescape("&#65;&#x42;") == "AB");
    CHECK(xmlUnescape("no entities") == "no entities");
    CHECK(xmlUnescape("&bogus;") == "&bogus;"); // unknown entity left as-is
}

TEST_CASE("xmlUnescape: multi-byte UTF-8 numeric refs") {
    CHECK(xmlUnescape("&#233;") == "\xC3\xA9");            // e-acute, 2-byte UTF-8
    CHECK(xmlUnescape("&#20013;") == "\xE4\xB8\xAD");      // CJK "middle", 3-byte UTF-8
    CHECK(xmlUnescape("&#128512;") == "\xF0\x9F\x98\x80"); // grinning face, 4-byte UTF-8
}

TEST_CASE("xmlUnescape: UTF-16 surrogates are rejected, not encoded") {
    // Surrogates are not valid Unicode scalar values and must never be
    // encoded to UTF-8; the reference is emitted literally, same as any
    // other invalid/unknown entity.
    CHECK(xmlUnescape("&#xD800;") == "&#xD800;");
    CHECK(xmlUnescape("&#xDFFF;") == "&#xDFFF;");
}

TEST_CASE("xmlUnescape: out-of-range numeric refs are rejected") {
    CHECK(xmlUnescape("&#0;") == "&#0;");
    CHECK(xmlUnescape("&#x110000;") == "&#x110000;");
}

TEST_CASE("xmlUnescape: hostile fallbacks") {
    CHECK(xmlUnescape("a&b") == "a&b");                               // bare '&', no ';'
    CHECK(xmlUnescape("&abcdefghijklmnop;") == "&abcdefghijklmnop;"); // entity name >10 chars
}

TEST_CASE("hostile input fails cleanly") {
    ListPage p;
    CHECK_FALSE(parseListPage("<ListBucketResult><Contents><Key>a", p)); // unclosed tag
    ListPage p2;
    CHECK_FALSE(parseListPage("<ListBucketResult><CommonPrefixes><Prefix>a", p2)); // unclosed tag
    S3ErrorBody e;
    CHECK_FALSE(parseErrorBody("", e));
}

TEST_CASE("parseListPage: rejections without a ListBucketResult / missing mandatory children") {
    ListPage p;
    CHECK_FALSE(parseListPage("<NotAListing><Contents><Key>a</Key></Contents></NotAListing>", p));
    ListPage p2;
    CHECK_FALSE(parseListPage(
        "<ListBucketResult><Contents><Size>1</Size></Contents></ListBucketResult>", p2));
    ListPage p3;
    CHECK_FALSE(
        parseListPage("<ListBucketResult><CommonPrefixes><Delimiter>/</Delimiter></CommonPrefixes>"
                      "</ListBucketResult>",
                      p3));
}

TEST_CASE("parseListPage: unquoted ETag is parsed as-is") {
    ListPage p;
    REQUIRE(parseListPage("<ListBucketResult><Contents><Key>k</Key><ETag>abc123</ETag></Contents>"
                          "</ListBucketResult>",
                          p));
    REQUIRE(p.entries.size() == 1);
    CHECK(p.entries[0].etag == "abc123");
}
