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

// Author: lsong@google.com (Libo Song)
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

#ifndef NET_INSTAWEB_UTIL_PUBLIC_FILE_CACHE_H_
#define NET_INSTAWEB_UTIL_PUBLIC_FILE_CACHE_H_

#include "net/instaweb/util/public/cache_interface.h"
#include "net/instaweb/util/public/file_system.h"
#include "net/instaweb/util/public/filename_encoder.h"
#include <string>

namespace net_instaweb {

// Simple C++ implementation of file cache.
//
// Also of note: the Get interface allows for streaming.  To get into
// a std::string, use a StringWriter.
//
// The Put interface does not currently stream, but this should be added.
class FileCache : public CacheInterface {
 public:
  FileCache(const std::string& path, FileSystem* file_system,
            FilenameEncoder* filename_encoder,
            MessageHandler* message_handler);
  virtual ~FileCache();

  virtual bool Get(const std::string& key, SharedString* value,
                   MessageHandler* message_handler);
  virtual void Put(const std::string& key, SharedString& value,
                   MessageHandler* message_handler);
  virtual void Delete(const std::string& key,
                      MessageHandler* message_handler);
  virtual KeyState Query(const std::string& key,
                         MessageHandler* message_handler);

 private:
  bool EncodeFilename(const std::string& key, std::string* filename);

  std::string path_;
  FileSystem* file_system_;
  FilenameEncoder* filename_encoder_;
  MessageHandler* message_handler_;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_UTIL_PUBLIC_FILE_CACHE_H_
