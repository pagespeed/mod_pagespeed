/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */


#ifndef NET_INSTAWEB_REWRITER_PUBLIC_LOCAL_STORAGE_CACHE_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_LOCAL_STORAGE_CACHE_FILTER_H_

#include <set>

#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_filter.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_filter.h"

namespace net_instaweb {

/*
 * The Local Storage Cache rewriter reduces HTTP requests by inlining resources
 * and using browser-side javascript to store the inlined resources in local
 * storage. The javascript also creates a cookie that reflects the resources it
 * has in local storage. On a repeat view, the server uses the cookie to
 * determine if it should replace an inlined resource with a script snippet
 * that loads the resource from local storage. In effect, we get browser
 * caching of inlined resources, theoretically speeding up first view (by
 * inlining) and repeat view (by not resending the inlined resource).
 */
class LocalStorageCacheFilter : public RewriteFilter {
 public:
  static const char kLscCookieName[];
  static const char kLscInitializer[];  // public for the test harness only.

  // Statistics' names.
  static const char kCandidatesFound[];
  static const char kStoredTotal[];
  static const char kStoredImages[];
  static const char kStoredCss[];
  static const char kCandidatesAdded[];
  static const char kCandidatesRemoved[];

  // State information for an inline filter using LSC.
  class InlineState {
   public:
    InlineState() : initialized_(false), enabled_(false) {}

   private:
    friend class LocalStorageCacheFilter;

    bool initialized_;
    bool enabled_;
    GoogleString url_;
  };

  explicit LocalStorageCacheFilter(RewriteDriver* rewrite_driver);
  ~LocalStorageCacheFilter() override;

  // May be called multiple times, if there are multiple statistics objects.
  static void InitStats(Statistics* statistics);

  void StartDocumentImpl() override;
  void EndDocument() override;
  void StartElementImpl(HtmlElement* element) override;
  void EndElementImpl(HtmlElement* element) override;

  const char* Name() const override { return "LocalStorageCache"; }
  const char* id() const override {
    return RewriteOptions::kLocalStorageCacheId;
  }

  std::set<StringPiece>* mutable_cookie_hashes() { return &cookie_hashes_; }

  // Tell the LSC that the resource with the given url is a candidate for
  // storing in the browser's local storage. If LSC is disabled it's a no-op,
  // otherwise it determines the LSC's version of the url and, if
  // skip_cookie_check is true or the hash of the LSC's url is in the LSC's
  // cookie, adds the LSC's url as an attribute of the given element, which the
  // LSC later uses to tell that the element is suitable for storing in local
  // cache. Returns true if the attribute was added. Saves various computed
  // values in the given state variable for any subsequent call (a filter might
  // need to call this method once with skip_cookie_check false, then again
  // later with it true).
  // url is the URL from the HTML element, src from img, href from style.
  // driver is the request's context.
  // is_enabled is set to true if the local storage cache filter is enabled.
  // skip_cookie_check if true skips the checking of the cookie for the hash
  //                   and adds the LSC's url attribute if LSC is enabled.
  // element is the element to add the attribute to.
  // state is where to save the computed values.
  static bool AddStorableResource(const StringPiece& url,
                                  RewriteDriver* driver,
                                  bool skip_cookie_check,
                                  HtmlElement* element,
                                  InlineState* state);

  // Tell the LSC to add its attributes to the given element:
  // data-pagespeed-lsc-hash and, if the resource has an expiry time [in
  // cached], data-pagespeed-lsc-expiry. This is a no-op if LSC is disabled.
  // url is the URL of the resource being rewritten.
  // cached is the result of the resource rewrite.
  // driver is the request's context.
  // element is the element to update.
  // Returns true if the element was updated.
  static bool AddLscAttributes(const StringPiece url,
                               const CachedResult& cached,
                               RewriteDriver* driver,
                               HtmlElement* element);

  // Remove the LSC attributes from the given element.
  static void RemoveLscAttributes(HtmlElement* element,
                                  RewriteDriver* driver);

  ScriptUsage GetScriptUsage() const override { return kWillInjectScripts; }

 private:
  void InsertOurScriptElement(HtmlElement* before);
  static bool IsHashInCookie(const RewriteDriver* driver,
                             const StringPiece cookie_name,
                             const StringPiece hash,
                             std::set<StringPiece>* hash_set);
  static GoogleString ExtractOtherImgAttributes(const HtmlElement* element);
  static GoogleString GenerateHashFromUrlAndElement(const RewriteDriver* driver,
                                                    const StringPiece& lsc_url,
                                                    const HtmlElement* element);

  // Have we inserted the script of utility functions?
  bool script_inserted_;
  // Have we seen any inlined resources that need the utility functions?
  bool script_needs_inserting_;
  // The set of hashes in the local storage cache cookie. Each element points
  // into the rewrite driver's cookies() - that must not change underneath us.
  std::set<StringPiece> cookie_hashes_;

  // # of times an img/link was found with a data-pagespeed-lsc-url attribute.
  Variable* num_local_storage_cache_candidates_found_;
  // # of times the hash of an img/link was found in the hash cookie.
  Variable* num_local_storage_cache_stored_total_;
  // # of times an img's hash was found in the hash cookie.
  Variable* num_local_storage_cache_stored_images_;
  // # of times a link's hash was found in the hash cookie.
  Variable* num_local_storage_cache_stored_css_;
  // # of times we added the hash and expiry attributes to a candidate img/link.
  Variable* num_local_storage_cache_candidates_added_;
  // # of times we removed the lsc attributes from a candidate img/link.
  Variable* num_local_storage_cache_candidates_removed_;

  DISALLOW_COPY_AND_ASSIGN(LocalStorageCacheFilter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_LOCAL_STORAGE_CACHE_FILTER_H_
