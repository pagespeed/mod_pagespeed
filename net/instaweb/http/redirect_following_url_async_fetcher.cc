/*
 * Copyright 2016 Google Inc.
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

// Author: oschaaf@we-amp.com (Otto van der Schaaf)

#include "net/instaweb/http/public/redirect_following_url_async_fetcher.h"

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/google_url.h"

// TODO(oschaaf): unchecked spec.
// TODO(oschaaf): inlining & intent should  be persisted across redirects.
namespace net_instaweb {

const int64 RedirectFollowingUrlAsyncFetcher::kUnset =
  GOOGLE_LONGLONG(0x7FFFFFFFFFFFFFFF);

class RedirectFollowingUrlAsyncFetcher::RedirectFollowingFetch
    : public SharedAsyncFetch {
 public:
  RedirectFollowingFetch(
      RedirectFollowingUrlAsyncFetcher* redirect_following_fetcher,
      AsyncFetch* base_fetch, const GoogleString& url,
      const GoogleString& context_url, MessageHandler* message_handler)
      : SharedAsyncFetch(base_fetch),
        redirect_following_fetcher_(redirect_following_fetcher),
        received_redirect_status_code_(false),
        urls_seen_(new StringSet()),
        url_(url),
        gurl_(url),
        base_fetch_(base_fetch),
        context_url_(context_url),
        message_handler_(message_handler),
        max_age_(kUnset),
        callback_done_(false),
        prepare_ok_(false) {
    urls_seen_->insert(gurl_.UncheckedSpec().as_string());
  }

  RedirectFollowingFetch(
      RedirectFollowingUrlAsyncFetcher* redirect_following_fetcher,
      AsyncFetch* base_fetch, const GoogleString& url,
      const GoogleString& context_url, std::unique_ptr<StringSet> redirects_followed_earlier,
      MessageHandler* message_handler, int64 max_age)
      : SharedAsyncFetch(base_fetch),
        redirect_following_fetcher_(redirect_following_fetcher),
        received_redirect_status_code_(false),
        urls_seen_(std::move(redirects_followed_earlier)),
        url_(url),
        gurl_(url),
        base_fetch_(base_fetch),
        context_url_(context_url),
        message_handler_(message_handler),
        max_age_(max_age),
        callback_done_(false),
        prepare_ok_(false) {}

  bool Validate() {
    if (!gurl_.IsWebValid()) {
      response_headers()->set_status_code(HttpStatus::kBadRequest);
      return false;
    }
    return true;
  }

 protected:
  bool HandleFlush(MessageHandler* message_handler) override {
    if (!received_redirect_status_code_) {
      return SharedAsyncFetch::HandleFlush(message_handler);
    }
    return true;
  }

  void HandleHeadersComplete() override {
    // Currently we support permanent and temporary redirects.
    // TODO(oschaaf): use ResponseHeaders::IsRedirectStatus() once
    // we support all redirect codes.
    bool supported_redirect_type =
        response_headers()->status_code() == HttpStatus::kMovedPermanently ||
        response_headers()->status_code() == HttpStatus::kFound;

    if (!supported_redirect_type) {
      if (max_age_ != kUnset) {
        // We must reduce max_age_ to the minimum of max_age_ or its current
        // value. If no cc is given, we should check the default.
        if (response_headers()->cache_ttl_ms() > max_age_) {
          response_headers()->SetCacheControlMaxAge(max_age_);
        }
      }
      SharedAsyncFetch::HandleHeadersComplete();
    } else {
      received_redirect_status_code_ = true;
    }
  }

  bool HandleWrite(const StringPiece& content,
                   MessageHandler* handler) override {
    if (!received_redirect_status_code_) {
      return SharedAsyncFetch::HandleWrite(content, handler);
    }
    return true;
  }

  void HandleDone(bool success) override {
    DCHECK(gurl_.IsWebValid() || !success);

    if (!received_redirect_status_code_) {
      // For honoring Csp later on, we will have to know which urls
      // have been involved into getting to this resource. We store
      // that into the response headers here using an internal header.
      auto it = urls_seen_->begin();
      // Skip the initial url, we are only interested in actual redirects
      // followed.
      it++;
      for (; it != urls_seen_->end(); ++it) {
        response_headers()->Add("@Redirects-Followed", *it);
      }
      response_headers()->ComputeCaching();
      SharedAsyncFetch::HandleDone(success);
      delete this;
      return;
    }

    GoogleString redirect_url;

    if (success) {
      success = TryExtractRedirectUrlFromResponseHeaders(&redirect_url);
    }
    if (success) {
      success = CheckAlreadyFollowedAndMaxRedirects(redirect_url);
    }

    GoogleString mapped_redirect_url;

    if (success) {
      success = TryMapRedirect(redirect_url, &mapped_redirect_url);
    }

    // Wipe out the 3XX response. We'll either fail/404 or return 200/OK
    response_headers()->Clear();

    if (success) {
      redirect_following_fetcher_->FollowRedirect(
          mapped_redirect_url, message_handler_, base_fetch_,
          std::move(urls_seen_), max_age_);
    } else {
      response_headers()->set_status_code(HttpStatus::kNotFound);
      SharedAsyncFetch::HandleDone(false);
    }

    delete this;
  }

 private:
  void EmitRedirectWarning(const GoogleString& context_url,
                           const GoogleString& redirect_url,
                           GoogleString message) {
      message_handler_->Message(
          kWarning, "Fetch redirect: [%s] -> [%s]: %s.", context_url.c_str(),
          redirect_url.c_str(), message.c_str());
  }

  bool CheckAlreadyFollowedAndMaxRedirects(const GoogleString& redirect_url) {
    int redirects_followed = urls_seen_->size();

    if (redirects_followed > redirect_following_fetcher_->max_redirects()) {
      EmitRedirectWarning(url_, redirect_url, "Max redirects exceeded");
      return false;
    }

    std::pair<StringSet::iterator, bool> ret = urls_seen_->insert(redirect_url);

    if (!ret.second) {
      EmitRedirectWarning(url_, redirect_url, "Cyclic redirect detected");
      return false;
    }

    return true;
  }

  void PrepareRequestDone(bool success) {
    callback_done_ = true;
    prepare_ok_ = success;
  }

  bool TryMapRedirect(const GoogleString& redirect_url,
                      GoogleString* mapped_url) {
    CHECK(mapped_url);
    const RewriteOptions* options =
        redirect_following_fetcher_->rewrite_options();
    const DomainLawyer* domain_lawyer = options->domain_lawyer();

    if (redirect_following_fetcher_->follow_temp_redirects()) {
      response_headers()->set_cache_temp_redirects(true);
      response_headers()->ComputeCaching();
    }

    if (response_headers()->RequiresProxyRevalidation()) {
      EmitRedirectWarning(url_, "t.b.d.",
          "Revalidation not supported for redirects");
      return false;
    }

    bool cacheable = response_headers()->IsProxyCacheable(
        base_fetch_->request_headers()->GetProperties(),
        ResponseHeaders::GetVaryOption(options->respect_vary()),
        ResponseHeaders::kNoValidator);

    if (!cacheable) {
      EmitRedirectWarning(url_, "t.b.d.",
          "Redirect not cacheable, not following");
      return false;
    } else {
      max_age_ = std::min(max_age_, response_headers()->cache_ttl_ms());
    }
    GoogleUrl redirect_gurl(gurl_, redirect_url);
    if (!domain_lawyer->IsDomainAuthorized(GoogleUrl(context_url_),
                                           redirect_gurl)) {
      EmitRedirectWarning(context_url_, redirect_url, "Unauthorized");
      return false;
    }

    if (!options->IsAllowed(redirect_gurl.Spec())) {
      EmitRedirectWarning(context_url_, redirect_url, "Rewriting disallowed");
      return false;
    }

    mapped_url->assign(redirect_url);

    redirect_following_fetcher_->rewrite_options_manager()->PrepareRequest(
        options, request_context(), mapped_url, request_headers(),
        NewCallback(this, &RedirectFollowingFetch::PrepareRequestDone));

    // While writing this the callback will always be executed synchronously.
    // when that changes, this will need maintenance.
    CHECK(callback_done_);

    if (!prepare_ok_) {
      EmitRedirectWarning(url_, redirect_url, "Failed to prepare redirect request");
    } else {
      redirect_gurl.Reset(*mapped_url);
      if (redirect_gurl.SchemeIs("https") &&
          !redirect_following_fetcher_->SupportsHttps()) {
        EmitRedirectWarning(url_, *mapped_url, "Https not supported");
        return false;
      }
    }
    return prepare_ok_;
  }

  bool TryExtractRedirectUrlFromResponseHeaders(GoogleString* redirect_url) {
    CHECK(redirect_url);
    const char* raw_redirect_url =
        response_headers()->Lookup1(HttpAttributes::kLocation);

    if (!raw_redirect_url) {
      EmitRedirectWarning(
          url_, "none", "Failed looking up exactly one Location header");
      return false;
    } else if (!strlen(raw_redirect_url)) {
      EmitRedirectWarning(
          url_, "", "Location header has an empty value");
      return false;
    }

    GoogleUrl redirect_gurl(gurl_, raw_redirect_url);
    redirect_gurl.UncheckedSpec().CopyToString(redirect_url);

    if (redirect_url->find('#') != StringPiece::npos) {
      EmitRedirectWarning(
          url_, *redirect_url, "Location url has a fragment, not following");
      return false;
    }

    if (!redirect_gurl.IsWebValid()) {
      EmitRedirectWarning(
          url_, *redirect_url, "Invalid or unsupported url in location header");
      return false;
    }
    return true;
  }

  RedirectFollowingUrlAsyncFetcher* redirect_following_fetcher_;
  bool received_redirect_status_code_;
  std::unique_ptr<StringSet> urls_seen_;
  GoogleString url_;
  GoogleUrl gurl_;
  AsyncFetch* base_fetch_;
  const GoogleString context_url_;
  MessageHandler* message_handler_;
  int64 max_age_;
  bool callback_done_;
  bool prepare_ok_;

  DISALLOW_COPY_AND_ASSIGN(RedirectFollowingFetch);
};

RedirectFollowingUrlAsyncFetcher::RedirectFollowingUrlAsyncFetcher(
    UrlAsyncFetcher* fetcher, const GoogleString& context_url,
    ThreadSystem* thread_system, Statistics* statistics, int max_redirects,
    bool follow_temp_redirects, RewriteOptions* rewrite_options,
    RewriteOptionsManager* rewrite_options_manager)
    : base_fetcher_(fetcher),
      context_url_(context_url),
      max_redirects_(max_redirects),
      follow_temp_redirects_(follow_temp_redirects),
      rewrite_options_(rewrite_options),
      rewrite_options_manager_(rewrite_options_manager),
      simulate_https_support_(false) {
  CHECK(rewrite_options);
}

RedirectFollowingUrlAsyncFetcher::~RedirectFollowingUrlAsyncFetcher() {}

void RedirectFollowingUrlAsyncFetcher::FollowRedirect(
    const GoogleString& url, MessageHandler* message_handler, AsyncFetch* fetch,
    std::unique_ptr<StringSet> redirects_followed_earlier, int64 max_age) {
  RedirectFollowingFetch* redirect_following_fetch =
      new RedirectFollowingFetch(this, fetch, url, context_url_,
                                 std::move(redirects_followed_earlier), message_handler,
                                 max_age);

  if (redirect_following_fetch->Validate()) {
    base_fetcher_->Fetch(url, message_handler, redirect_following_fetch);
  } else {
    message_handler->Message(
        kWarning, "Decline following of bad redirect url: %s", url.c_str());
    redirect_following_fetch->Done(false);
  }
}

void RedirectFollowingUrlAsyncFetcher::Fetch(const GoogleString& url,
                                             MessageHandler* message_handler,
                                             AsyncFetch* fetch) {
  RedirectFollowingFetch* redirect_following_fetch = new RedirectFollowingFetch(
      this, fetch, url, context_url_, message_handler);
  if (redirect_following_fetch->Validate()) {
    base_fetcher_->Fetch(url, message_handler, redirect_following_fetch);
  } else {
    message_handler->Message(
        kWarning, "Decline fetching of bad url: %s", url.c_str());
    redirect_following_fetch->Done(false);
  }
}

}  // namespace net_instaweb
