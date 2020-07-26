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

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_DECODE_REWRITTEN_URLS_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_DECODE_REWRITTEN_URLS_FILTER_H_

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/html/empty_html_filter.h"

namespace net_instaweb {

class HtmlElement;
class RewriteDriver;

// Filter that decodes rewritten (.pagespeed.) URLs in HTML to origin URLs.
// TODO(sriharis):  Also do this for URLs in CSS.
class DecodeRewrittenUrlsFilter : public EmptyHtmlFilter {
 public:
  explicit DecodeRewrittenUrlsFilter(RewriteDriver* driver) : driver_(driver) {}
  virtual ~DecodeRewrittenUrlsFilter();

  void StartElement(HtmlElement* element) override;
  const char* Name() const override { return "DecodeRewrittenUrlsFilter"; }

 private:
  RewriteDriver* driver_;

  DISALLOW_COPY_AND_ASSIGN(DecodeRewrittenUrlsFilter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_DECODE_REWRITTEN_URLS_FILTER_H_
