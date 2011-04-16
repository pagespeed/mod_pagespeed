// Copyright 2010 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Author: mdsteele@google.com (Matthew D. Steele)

#include "net/instaweb/rewriter/public/css_inline_filter.h"

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/string_util.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/htmlparse/public/html_parse.h"
#include "net/instaweb/rewriter/public/css_tag_scanner.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/util/public/google_url.h"
#include "net/instaweb/util/public/string_writer.h"

namespace net_instaweb {

CssInlineFilter::CssInlineFilter(RewriteDriver* driver)
    : CommonFilter(driver),
      size_threshold_bytes_(driver->options()->css_inline_max_bytes()) {}

void CssInlineFilter::StartDocumentImpl() {
}

void CssInlineFilter::EndElementImpl(HtmlElement* element) {
  if ((element->keyword() == HtmlName::kLink) &&
      !driver_->HasChildrenInFlushWindow(element)) {
    const char* rel = element->AttributeValue(HtmlName::kRel);
    if (rel == NULL || strcmp(rel, "stylesheet")) {
      return;
    }

    // If the link tag has a media attribute whose value isn't "all", don't
    // inline.  (Note that "all" is equivalent to having no media attribute;
    // see http://www.w3.org/TR/html5/semantics.html#the-style-element)
    const char* media = element->AttributeValue(HtmlName::kMedia);
    if (media != NULL && strcmp(media, "all") != 0) {
      return;
    }

    // Get the URL where the external script is stored
    const char* href = element->AttributeValue(HtmlName::kHref);
    if (href == NULL) {
      return;  // We obviously can't inline if the URL isn't there.
    }

    // Make sure we're not moving across domains -- CSS can potentially contain
    // Javascript expressions.
    // TODO(jmaessen): Is the domain lawyer policy the appropriate one here?
    // Or do we still have to check for strict domain equivalence?
    // If so, add an inline-in-page policy to domainlawyer in some form,
    // as we make a similar policy decision in js_inline_filter.
    MessageHandler* message_handler = driver_->message_handler();
    ResourcePtr resource(CreateInputResourceAndReadIfCached(href));
    if ((resource.get() == NULL) || !resource->ContentsValid()) {
      return;
    }

    // Check that the file is small enough to inline.
    StringPiece contents = resource->contents();
    if (contents.size() > size_threshold_bytes_) {
      return;
    }

    // Check that the file does not have imports, which we cannot yet
    // correct paths yet.
    //
    // Remove this once CssTagScanner::AbsolutifyUrls handles imports.
    if (CssTagScanner::HasImport(contents, message_handler)) {
      return;
    }

    // Absolutify the URLs in the CSS -- relative URLs will break otherwise.
    GoogleString rewritten_contents;
    StringWriter writer(&rewritten_contents);
    GoogleUrl resource_url(resource->url());
    StringPiece input_dir = resource_url.AllExceptLeaf();
    StringPiece base_dir = base_url().AllExceptLeaf();
    bool written;
    if (input_dir == base_dir) {
      // We don't need to absolutify URLs if input directory is same as base.
      written = writer.Write(contents, message_handler);
    } else {
      // If they are different directories, we need to absolutify.
      // TODO(sligocki): Perhaps we should use the real CSS parser.
      written = CssTagScanner::AbsolutifyUrls(contents, resource->url(),
                                              &writer, message_handler);
    }
    if (!written) {
      return;
    }

    // Inline the CSS.
    HtmlElement* style_element =
        driver_->NewElement(element->parent(), HtmlName::kStyle);
    if (driver_->ReplaceNode(element, style_element)) {
      driver_->AppendChild(
          style_element,
          driver_->NewCharactersNode(element, rewritten_contents));
    }
  }
}

}  // namespace net_instaweb
