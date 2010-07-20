/**
 * Copyright 2010 Google Inc.
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

// Author: jmarantz@google.com (Joshua Marantz)
//     and sligocki@google.com (Shawn Ligocki)

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_MANAGER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_MANAGER_H_

#include <map>
#include <vector>
#include "base/scoped_ptr.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include <string>
#include "net/instaweb/util/public/string_util.h"

class GURL;

namespace net_instaweb {

class ContentType;
class FileSystem;
class FilenameEncoder;
class HTTPCache;
class Hasher;
class InputResource;
class MessageHandler;
class MetaData;
class OutputResource;
class Statistics;
class UrlFetcher;
class Writer;

class ResourceManager {
 public:
  ResourceManager(const StringPiece& file_prefix,
                  const StringPiece& url_prefix,
                  const int num_shards,
                  FileSystem* file_system,
                  FilenameEncoder* filename_encoder,
                  UrlFetcher* url_fetcher,
                  Hasher* hasher,
                  HTTPCache* http_cache);
  ~ResourceManager();

  // Created resources are managed by ResourceManager and eventually deleted
  // by ResourceManager's destructor.

  // Creates an output resource with a generated name.  Such a
  // resource can only be meaningfully created in a deployment with
  // shared persistent storage, such as a the local disk on a
  // single-server system, or a multi-server configuration with a
  // database, network attached storage, or a shared cache such as
  // memcached.
  //
  // If this is not available in the current deployment, then NULL is returned.
  //
  // Every time this method is called, a new resource is generated.
  OutputResource* CreateGeneratedOutputResource(
      const StringPiece& filter_prefix, const ContentType& type);

  // Creates an output resource where the name is provided by the rewriter.
  // The intent is to be able to derive the content from the name, for example,
  // by encoding URLs and metadata.
  //
  // This method is not dependent on shared persistent storage, and always
  // succeeds.
  //
  // This name is prepended with url_prefix for writing hrefs, and
  // file_prefix when working with the file system.  So files are:
  //    $(FILE_PREFIX)$(FILTER_PREFIX).$(HASH).$(NAME).$(CONTENT_TYPE_EXT)
  // and hrefs are:
  //    $(URL_PREFIX)$(FILTER_PREFIX).$(HASH).$(NAME).$(CONTENT_TYPE_EXT)
  //
  OutputResource* CreateNamedOutputResource(
      const StringPiece& filter_prefix, const StringPiece& name,
      const ContentType& type);

  // Creates a resource based on the fields extracted from a URL.  This
  // is used for serving output resources.
  OutputResource* CreateUrlOutputResource(
      const StringPiece& filter_prefix, const StringPiece& name,
      const StringPiece& hash, const ContentType& type);

  InputResource* CreateInputResource(const StringPiece& url,
                                             MessageHandler* handler);

  // Set up a basic header for a given content_type.
  void SetDefaultHeaders(const ContentType& content_type,
                                 MetaData* header);

  StringPiece file_prefix() const { return file_prefix_; }
  StringPiece url_prefix() const { return url_prefix_; }
  std::string base_url() const;

  void set_file_prefix(const StringPiece& file_prefix);
  void set_url_prefix(const StringPiece& url_prefix);
  void set_base_url(const StringPiece& url);
  Statistics* statistics() const { return statistics_; }
  void set_statistics(Statistics* s) { statistics_ = s; }

  bool FetchOutputResource(
    const OutputResource* output_resource,
    Writer* writer, MetaData* response_headers,
    MessageHandler* handler) const;

 private:
  scoped_ptr<GURL> base_url_;  // Base url to resolve relative urls against.
  std::string file_prefix_;
  std::string url_prefix_;
  int num_shards_;   // NYI: For server sharding of OutputResources.
  int resource_id_;  // Sequential ids for temporary OutputResource filenames.
  FileSystem* file_system_;
  FilenameEncoder* filename_encoder_;
  UrlFetcher* url_fetcher_;
  Hasher* hasher_;
  Statistics* statistics_;
  HTTPCache* http_cache_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_RESOURCE_MANAGER_H_
