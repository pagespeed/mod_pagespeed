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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_INSERT_DNS_PREFETCH_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_INSERT_DNS_PREFETCH_FILTER_H_

#include "net/instaweb/rewriter/public/common_filter.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"  // for StringSet, etc
#include "pagespeed/kernel/html/html_element.h"

namespace net_instaweb {

class FlushEarlyInfo;
class RewriteDriver;

// Injects <link rel="dns-prefetch" href="//www.example.com"> tags in the HEAD
// to enable the browser to do DNS prefetching.
class InsertDnsPrefetchFilter : public CommonFilter {
 public:
  explicit InsertDnsPrefetchFilter(RewriteDriver* driver);
  ~InsertDnsPrefetchFilter() override;

  void StartDocumentImpl() override;
  void EndDocument() override;
  void StartElementImpl(HtmlElement* element) override;
  void EndElementImpl(HtmlElement* element) override;

  const char* Name() const override { return "InsertDnsPrefetchFilter"; }
  // Override DetermineEnabled to enable writing of the property cache DOM
  // cohort in the RewriteDriver.
  void DetermineEnabled(GoogleString* disabled_reason) override;
  virtual const char* id() const { return "idp"; }

 private:
  void Clear();

  // Add a domain found in the page to the list of domains for which DNS
  // prefetch tags can be inserted.
  void MarkAlreadyInHead(HtmlElement::Attribute* urlattr);

  // Returns true if the list of domains for DNS prefetch tags is "stable".
  // Refer to the implementation for details about stability. This filter will
  // insert the tags into the HEAD once the list is stable.
  bool IsDomainListStable(const FlushEarlyInfo& flush_early_info) const;
  void DebugPrint(const char* msg);

  // This flag is useful if multiple HEADs are present. This filter inserts the
  // DNS prefetch tags only in the first HEAD.
  bool dns_prefetch_inserted_;

  // This flag indicates if we are currently processing elements in HEAD.
  bool in_head_;

  // The set of domains that we should not insert DNS prefetch tags for. This
  // includes the current domain we are rewriting, and resource links in HEAD.
  StringSet domains_to_ignore_;

  // The set of domains seen in resource links in BODY and not already seen in
  // HEAD.
  StringSet domains_in_body_;

  // The list of domains for which DNS prefetch tags can be inserted, in the
  // order they were seen in BODY.
  StringVector dns_prefetch_domains_;

  // Whether this user agent supports dns prefetch filter.
  bool user_agent_supports_dns_prefetch_;

  DISALLOW_COPY_AND_ASSIGN(InsertDnsPrefetchFilter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_INSERT_DNS_PREFETCH_FILTER_H_
