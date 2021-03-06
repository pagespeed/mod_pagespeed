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

#pragma once

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/html/html_filter.h"

namespace net_instaweb {

class HtmlCdataNode;
class HtmlCharactersNode;
class HtmlCommentNode;
class HtmlDirectiveNode;
class HtmlElement;
class HtmlIEDirectiveNode;

// Base class for rewriting filters that don't need to be sure to
// override every filter method.  Other filters that need to be sure
// they override every method would derive directly from HtmlFilter.
class EmptyHtmlFilter : public HtmlFilter {
 public:
  EmptyHtmlFilter();
  ~EmptyHtmlFilter() override;

  void StartDocument() override;
  void EndDocument() override;
  void StartElement(HtmlElement* element) override;
  void EndElement(HtmlElement* element) override;
  void Cdata(HtmlCdataNode* cdata) override;
  void Comment(HtmlCommentNode* comment) override;
  void IEDirective(HtmlIEDirectiveNode* directive) override;
  void Characters(HtmlCharactersNode* characters) override;
  void Directive(HtmlDirectiveNode* directive) override;
  void Flush() override;
  void DetermineEnabled(GoogleString* disabled_reason) override;

  // This filter and derived classes will not rewrite urls.  If a derived filter
  // wants to rewrite urls, override this function.
  bool CanModifyUrls() override { return false; }

  // Invoked by the rewrite driver to query whether this filter may
  // inject scripts.
  ScriptUsage GetScriptUsage() const override { return kNeverInjectsScripts; }

  // Note -- this does not provide an implementation for Name().  This
  // must be supplied by derived classes.
};

}  // namespace net_instaweb
