/*
 * Copyright 2013 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: jud@google.com (Jud Porter)

#include "net/instaweb/rewriter/public/critical_images_beacon_filter.h"

#include <set>

#include "net/instaweb/htmlparse/public/html_parse_test_base.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_test_base.h"
#include "net/instaweb/util/public/escaping.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/gtest.h"
#include "net/instaweb/util/public/scoped_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_hash.h"
#include "net/instaweb/util/public/string_util.h"

namespace {

const char kChefGifFile[] = "IronChef2.gif";
// Set image dimensions such that image will be inlined.
const char kChefGifDims[] = "width=48 height=64";

}  // namespace

namespace net_instaweb {

class CriticalImagesBeaconFilterTest : public RewriteTestBase {
 protected:
  CriticalImagesBeaconFilterTest() {}

  virtual void SetUp() {
    options()->set_beacon_url("http://example.com/beacon");
    CriticalImagesBeaconFilter::InitStats(statistics());
    // Enable a filter that uses critical images, which in turn will enable
    // beacon insertion.
    options()->EnableFilter(RewriteOptions::kLazyloadImages);
    options()->set_critical_images_beacon_enabled(true);
    RewriteTestBase::SetUp();
    https_mode_ = false;
  }

  void RunInjection() {
    rewrite_driver()->AddFilters();
    // The filter should absolutify img URLs before generating the hash of the
    // URL, so test with both a relative and absolute URL and make sure the
    // hashes match.
    GoogleUrl base(GetTestUrl());
    GoogleUrl img_gurl(base, kChefGifFile);

    AddFileToMockFetcher(img_gurl.Spec(), kChefGifFile, kContentTypeJpeg, 100);

    GoogleString html = "<head></head><body>";
    // Add the relative image URL.
    StrAppend(&html, "<img src=\"", kChefGifFile, "\" ", kChefGifDims, ">");
    // Add the absolute image URL.
    StrAppend(&html, "<img src=\"", img_gurl.Spec(), "\" ", kChefGifDims, ">");
    StrAppend(&html, "</body>");
    ParseUrl(GetTestUrl(), html);
  }

  void VerifyInjection() {
    EXPECT_EQ(1, statistics()->GetVariable(
        CriticalImagesBeaconFilter::kCriticalImagesBeaconAddedCount)->Get());
    EXPECT_TRUE(output_buffer_.find(CreateInitString()) != GoogleString::npos);
  }

  void VerifyNoInjection() {
    EXPECT_EQ(0, statistics()->GetVariable(
        CriticalImagesBeaconFilter::kCriticalImagesBeaconAddedCount)->Get());
    EXPECT_TRUE(output_buffer_.find("criticalImagesBeaconInit") ==
                GoogleString::npos);
  }

  void VerifyWithNoImageRewrite() {
    const GoogleString hash_str = ImageUrlHash(kChefGifFile);
    GoogleUrl base(GetTestUrl());
    GoogleUrl img_gurl(base, kChefGifFile);
    EXPECT_TRUE(output_buffer_.find(
        StrCat("pagespeed_url_hash=\"", hash_str)) != GoogleString::npos);
  }

  void AssumeHttps() {
    https_mode_ = true;
  }

  GoogleString GetTestUrl() {
    return StrCat((https_mode_ ? "https://example.com/" : kTestDomain),
                  "index.html?a&b");
  }

  GoogleString ImageUrlHash(StringPiece url) {
    // Absolutify the URL before hashing.
    GoogleUrl base(GetTestUrl());
    GoogleUrl img_gurl(base, url);
    unsigned int hash_val = HashString<CasePreserve, unsigned int>(
        img_gurl.spec_c_str(), strlen(img_gurl.spec_c_str()));
    return UintToString(hash_val);
  }


  GoogleString CreateInitString() {
    GoogleString url;
    EscapeToJsStringLiteral(rewrite_driver()->google_url().Spec(), false, &url);
    StringPiece beacon_url = https_mode_ ? options()->beacon_url().https :
        options()->beacon_url().http;
    GoogleString str = "pagespeed.criticalImagesBeaconInit(";
    StrAppend(&str, "'", beacon_url, "', ");
    StrAppend(&str, "'", url, "');");
    return str;
  }

  bool https_mode_;
};

TEST_F(CriticalImagesBeaconFilterTest, ScriptInjection) {
  RunInjection();
  VerifyInjection();
  VerifyWithNoImageRewrite();
}

TEST_F(CriticalImagesBeaconFilterTest, ScriptInjectionWithHttps) {
  AssumeHttps();
  RunInjection();
  VerifyInjection();
  VerifyWithNoImageRewrite();
}

TEST_F(CriticalImagesBeaconFilterTest, ScriptInjectionWithImageInlining) {
  // Verify that the URL hash is applied to the absolute image URL, and not to
  // the rewritten URL. In this case, make sure that an image inlined to a data
  // URI has the correct hash. We need to add the image hash to the critical
  // image set to make sure that the image is inlined.
  GoogleString hash_str = ImageUrlHash(kChefGifFile);
  scoped_ptr<StringSet> crit_img_set(new StringSet);
  crit_img_set->insert(hash_str);
  rewrite_driver()->set_critical_images(crit_img_set.release());
  rewrite_driver()->set_updated_critical_images(true);
  options()->set_image_inline_max_bytes(10000);
  options()->EnableFilter(RewriteOptions::kResizeImages);
  options()->EnableFilter(RewriteOptions::kInlineImages);
  options()->EnableFilter(RewriteOptions::kInsertImageDimensions);
  options()->EnableFilter(RewriteOptions::kConvertGifToPng);
  options()->DisableFilter(RewriteOptions::kLazyloadImages);
  RunInjection();
  VerifyInjection();

  EXPECT_TRUE(output_buffer_.find("data:") != GoogleString::npos);
  EXPECT_TRUE(output_buffer_.find(hash_str) != GoogleString::npos);
}

TEST_F(CriticalImagesBeaconFilterTest, UnsupportedUserAgent) {
  // Test that the filter is not applied for unsupported user agents.
  rewrite_driver()->SetUserAgent("Firefox/1.0");
  RunInjection();
  VerifyNoInjection();
}

}  // namespace net_instaweb