// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <climits>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "google/protobuf/compiler/code_generator.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_split.h"
#include "google/protobuf/compiler/objectivec/line_consumer.h"
#include "google/protobuf/compiler/objectivec/names.h"
#include "google/protobuf/compiler/objectivec/nsobject_methods.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

// NOTE: src/google/protobuf/compiler/plugin.cc makes use of cerr for some
// error cases, so it seems to be ok to use as a back door for errors.

namespace google {
namespace protobuf {
namespace compiler {
namespace objectivec {

namespace {

bool BoolFromEnvVar(const char* env_var, bool default_value) {
  const char* value = getenv(env_var);
  if (value) {
    return std::string("YES") == absl::AsciiStrToUpper(value);
  }
  return default_value;
}

class SimpleLineCollector : public LineConsumer {
 public:
  explicit SimpleLineCollector(std::unordered_set<std::string>* inout_set)
      : set_(inout_set) {}

  virtual bool ConsumeLine(const absl::string_view& line, std::string* out_error) override {
    set_->insert(std::string(line));
    return true;
  }

 private:
  std::unordered_set<std::string>* set_;
};

class PackageToPrefixesCollector : public LineConsumer {
 public:
  PackageToPrefixesCollector(const std::string &usage,
                             std::map<std::string, std::string>* inout_package_to_prefix_map)
      : usage_(usage), prefix_map_(inout_package_to_prefix_map) {}

  virtual bool ConsumeLine(const absl::string_view& line, std::string* out_error) override;

 private:
  const std::string usage_;
  std::map<std::string, std::string>* prefix_map_;
};

class PrefixModeStorage {
 public:
  PrefixModeStorage();

  std::string package_to_prefix_mappings_path() const { return package_to_prefix_mappings_path_; }
  void set_package_to_prefix_mappings_path(const std::string& path) {
    package_to_prefix_mappings_path_ = path;
    package_to_prefix_map_.clear();
  }

  std::string prefix_from_proto_package_mappings(const FileDescriptor* file);

  bool use_package_name() const { return use_package_name_; }
  void set_use_package_name(bool on_or_off) { use_package_name_ = on_or_off; }

  std::string exception_path() const { return exception_path_; }
  void set_exception_path(const std::string& path) {
    exception_path_ = path;
    exceptions_.clear();
  }

  bool is_package_exempted(const std::string& package);

  // When using a proto package as the prefix, this should be added as the
  // prefix in front of it.
  const std::string& forced_package_prefix() const { return forced_prefix_; }
  void set_forced_package_prefix(const std::string& prefix) { forced_prefix_ = prefix; }

 private:
  bool use_package_name_;
  std::map<std::string, std::string> package_to_prefix_map_;
  std::string package_to_prefix_mappings_path_;
  std::string exception_path_;
  std::string forced_prefix_;
  std::unordered_set<std::string> exceptions_;
};

PrefixModeStorage::PrefixModeStorage() {
  // Even thought there are generation options, have an env back door since some
  // of these helpers could be used in other plugins.

  use_package_name_ = BoolFromEnvVar("GPB_OBJC_USE_PACKAGE_AS_PREFIX", false);

  const char* exception_path = getenv("GPB_OBJC_PACKAGE_PREFIX_EXCEPTIONS_PATH");
  if (exception_path) {
    exception_path_ = exception_path;
  }

  const char* prefix = getenv("GPB_OBJC_USE_PACKAGE_AS_PREFIX_PREFIX");
  if (prefix) {
    forced_prefix_ = prefix;
  }
}

std::string PrefixModeStorage::prefix_from_proto_package_mappings(const FileDescriptor* file) {
  if (!file) {
    return "";
  }

  if (package_to_prefix_map_.empty() && !package_to_prefix_mappings_path_.empty()) {
    std::string error_str;
    // Re use the same collector as we use for expected_prefixes_path since the file
    // format is the same.
    PackageToPrefixesCollector collector("Package to prefixes", &package_to_prefix_map_);
    if (!ParseSimpleFile(package_to_prefix_mappings_path_, &collector, &error_str)) {
      if (error_str.empty()) {
        error_str = std::string("protoc:0: warning: Failed to parse")
           + std::string(" prefix to proto package mappings file: ")
           + package_to_prefix_mappings_path_;
      }
      std::cerr << error_str << std::endl;
      std::cerr.flush();
      package_to_prefix_map_.clear();
    }
  }

  const std::string package = file->package();
  // For files without packages, the can be registered as "no_package:PATH",
  // allowing the expected prefixes file.
  static const std::string no_package_prefix("no_package:");
  const std::string lookup_key = package.empty() ? no_package_prefix + file->name() : package;

  std::map<std::string, std::string>::const_iterator prefix_lookup =
      package_to_prefix_map_.find(lookup_key);

  if (prefix_lookup != package_to_prefix_map_.end()) {
    return prefix_lookup->second;
  }

  return "";
}

bool PrefixModeStorage::is_package_exempted(const std::string& package) {
  if (exceptions_.empty() && !exception_path_.empty()) {
    std::string error_str;
    SimpleLineCollector collector(&exceptions_);
    if (!ParseSimpleFile(exception_path_, &collector, &error_str)) {
      if (error_str.empty()) {
        error_str = std::string("protoc:0: warning: Failed to parse")
           + std::string(" package prefix exceptions file: ")
           + exception_path_;
      }
      std::cerr << error_str << std::endl;
      std::cerr.flush();
      exceptions_.clear();
    }

    // If the file was empty put something in it so it doesn't get reloaded over
    // and over.
    if (exceptions_.empty()) {
      exceptions_.insert("<not a real package>");
    }
  }

  return exceptions_.count(package) != 0;
}

PrefixModeStorage g_prefix_mode;

}  // namespace

std::string GetPackageToPrefixMappingsPath() {
  return g_prefix_mode.package_to_prefix_mappings_path();
}

void SetPackageToPrefixMappingsPath(const std::string& file_path) {
  g_prefix_mode.set_package_to_prefix_mappings_path(file_path);
}

bool UseProtoPackageAsDefaultPrefix() {
  return g_prefix_mode.use_package_name();
}

void SetUseProtoPackageAsDefaultPrefix(bool on_or_off) {
  g_prefix_mode.set_use_package_name(on_or_off);
}

std::string GetProtoPackagePrefixExceptionList() {
  return g_prefix_mode.exception_path();
}

void SetProtoPackagePrefixExceptionList(const std::string& file_path) {
  g_prefix_mode.set_exception_path(file_path);
}

std::string GetForcedPackagePrefix() {
  return g_prefix_mode.forced_package_prefix();
}

void SetForcedPackagePrefix(const std::string& prefix) {
  g_prefix_mode.set_forced_package_prefix(prefix);
}

namespace {

std::unordered_set<std::string> MakeWordsMap(const char* const words[],
                                             size_t num_words) {
  std::unordered_set<std::string> result;
  for (int i = 0; i < num_words; i++) {
    result.insert(words[i]);
  }
  return result;
}

const char* const kUpperSegmentsList[] = {"url", "http", "https"};

std::unordered_set<std::string> kUpperSegments =
    MakeWordsMap(kUpperSegmentsList, ABSL_ARRAYSIZE(kUpperSegmentsList));

// Internal helper for name handing.
// Do not expose this outside of helpers, stick to having functions for specific
// cases (ClassName(), FieldName()), so there is always consistent suffix rules.
std::string UnderscoresToCamelCase(const std::string& input,
                                   bool first_capitalized) {
  std::vector<std::string> values;
  std::string current;

  bool last_char_was_number = false;
  bool last_char_was_lower = false;
  bool last_char_was_upper = false;
  for (int i = 0; i < input.size(); i++) {
    char c = input[i];
    if (absl::ascii_isdigit(c)) {
      if (!last_char_was_number) {
        values.push_back(current);
        current = "";
      }
      current += c;
      last_char_was_number = last_char_was_lower = last_char_was_upper = false;
      last_char_was_number = true;
    } else if (absl::ascii_islower(c)) {
      // lowercase letter can follow a lowercase or uppercase letter
      if (!last_char_was_lower && !last_char_was_upper) {
        values.push_back(current);
        current = "";
      }
      current += c;  // already lower
      last_char_was_number = last_char_was_lower = last_char_was_upper = false;
      last_char_was_lower = true;
    } else if (absl::ascii_isupper(c)) {
      if (!last_char_was_upper) {
        values.push_back(current);
        current = "";
      }
      current += absl::ascii_tolower(c);
      last_char_was_number = last_char_was_lower = last_char_was_upper = false;
      last_char_was_upper = true;
    } else {
      last_char_was_number = last_char_was_lower = last_char_was_upper = false;
    }
  }
  values.push_back(current);

  std::string result;
  bool first_segment_forces_upper = false;
  for (std::vector<std::string>::iterator i = values.begin(); i != values.end();
       ++i) {
    std::string value = *i;
    bool all_upper = (kUpperSegments.count(value) > 0);
    if (all_upper && (result.length() == 0)) {
      first_segment_forces_upper = true;
    }
    for (int j = 0; j < value.length(); j++) {
      if (j == 0 || all_upper) {
        value[j] = absl::ascii_toupper(value[j]);
      } else {
        // Nothing, already in lower.
      }
    }
    result += value;
  }
  if ((result.length() != 0) &&
      !first_capitalized &&
      !first_segment_forces_upper) {
    result[0] = absl::ascii_tolower(result[0]);
  }
  return result;
}

const char* const kReservedWordList[] = {
  // Note NSObject Methods:
  // These are brought in from nsobject_methods.h that is generated
  // using method_dump.sh. See kNSObjectMethods below.

  // Objective C "keywords" that aren't in C
  // From
  // http://stackoverflow.com/questions/1873630/reserved-keywords-in-objective-c
  // with some others added on.
  "id", "_cmd", "super", "in", "out", "inout", "bycopy", "byref", "oneway",
  "self", "instancetype", "nullable", "nonnull", "nil", "Nil",
  "YES", "NO", "weak",

  // C/C++ keywords (Incl C++ 0x11)
  // From http://en.cppreference.com/w/cpp/keywords
  "and", "and_eq", "alignas", "alignof", "asm", "auto", "bitand", "bitor",
  "bool", "break", "case", "catch", "char", "char16_t", "char32_t", "class",
  "compl", "const", "constexpr", "const_cast", "continue", "decltype",
  "default", "delete", "double", "dynamic_cast", "else", "enum", "explicit",
  "export", "extern ", "false", "float", "for", "friend", "goto", "if",
  "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not",
  "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
  "public", "register", "reinterpret_cast", "return", "short", "signed",
  "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
  "template", "this", "thread_local", "throw", "true", "try", "typedef",
  "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
  "volatile", "wchar_t", "while", "xor", "xor_eq",

  // C99 keywords
  // From
  // http://publib.boulder.ibm.com/infocenter/lnxpcomp/v8v101/index.jsp?topic=%2Fcom.ibm.xlcpp8l.doc%2Flanguage%2Fref%2Fkeyw.htm
  "restrict",

  // GCC/Clang extension
  "typeof",

  // Not a keyword, but will break you
  "NULL",

  // C88+ specs call for these to be macros, so depending on what they are
  // defined to be it can lead to odd errors for some Xcode/SDK versions.
  "stdin", "stdout", "stderr",

  // Objective-C Runtime typedefs
  // From <obc/runtime.h>
  "Category", "Ivar", "Method", "Protocol",

  // GPBMessage Methods
  // Only need to add instance methods that may conflict with
  // method declared in protos. The main cases are methods
  // that take no arguments, or setFoo:/hasFoo: type methods.
  "clear", "data", "delimitedData", "descriptor", "extensionRegistry",
  "extensionsCurrentlySet", "initialized", "isInitialized", "serializedSize",
  "sortedExtensionsInUse", "unknownFields",

  // MacTypes.h names
  "Fixed", "Fract", "Size", "LogicalAddress", "PhysicalAddress", "ByteCount",
  "ByteOffset", "Duration", "AbsoluteTime", "OptionBits", "ItemCount",
  "PBVersion", "ScriptCode", "LangCode", "RegionCode", "OSType",
  "ProcessSerialNumber", "Point", "Rect", "FixedPoint", "FixedRect", "Style",
  "StyleParameter", "StyleField", "TimeScale", "TimeBase", "TimeRecord",
};

// returns true is input starts with __ or _[A-Z] which are reserved identifiers
// in C/ C++. All calls should go through UnderscoresToCamelCase before getting here
// but this verifies and allows for future expansion if we decide to redefine what a
// reserved C identifier is (for example the GNU list
// https://www.gnu.org/software/libc/manual/html_node/Reserved-Names.html )
bool IsReservedCIdentifier(const std::string& input) {
  if (input.length() > 2) {
    if (input.at(0) == '_') {
      if (isupper(input.at(1)) || input.at(1) == '_') {
        return true;
      }
    }
  }
  return false;
}

std::string SanitizeNameForObjC(const std::string& prefix,
                                const std::string& input,
                                const std::string& extension,
                                std::string* out_suffix_added) {
  static const std::unordered_set<std::string> kReservedWords =
      MakeWordsMap(kReservedWordList, ABSL_ARRAYSIZE(kReservedWordList));
  static const std::unordered_set<std::string> kNSObjectMethods =
      MakeWordsMap(kNSObjectMethodsList, ABSL_ARRAYSIZE(kNSObjectMethodsList));
  std::string sanitized;
  // We add the prefix in the cases where the string is missing a prefix.
  // We define "missing a prefix" as where 'input':
  // a) Doesn't start with the prefix or
  // b) Isn't equivalent to the prefix or
  // c) Has the prefix, but the letter after the prefix is lowercase
  if (absl::StartsWith(input, prefix)) {
    if (input.length() == prefix.length() || !absl::ascii_isupper(input[prefix.length()])) {
      sanitized = prefix + input;
    } else {
      sanitized = input;
    }
  } else {
    sanitized = prefix + input;
  }
  if (IsReservedCIdentifier(sanitized) ||
      (kReservedWords.count(sanitized) > 0) ||
      (kNSObjectMethods.count(sanitized) > 0)) {
    if (out_suffix_added) *out_suffix_added = extension;
    return sanitized + extension;
  }
  if (out_suffix_added) out_suffix_added->clear();
  return sanitized;
}

std::string NameFromFieldDescriptor(const FieldDescriptor* field) {
  if (field->type() == FieldDescriptor::TYPE_GROUP) {
    return field->message_type()->name();
  } else {
    return field->name();
  }
}

void PathSplit(const std::string& path, std::string* directory,
               std::string* basename) {
  std::string::size_type last_slash = path.rfind('/');
  if (last_slash == std::string::npos) {
    if (directory) {
      *directory = "";
    }
    if (basename) {
      *basename = path;
    }
  } else {
    if (directory) {
      *directory = path.substr(0, last_slash);
    }
    if (basename) {
      *basename = path.substr(last_slash + 1);
    }
  }
}

bool IsSpecialNamePrefix(const std::string& name,
                         const std::string* special_names,
                         size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const size_t length = special_names[i].length();
    if (name.compare(0, length, special_names[i]) == 0) {
      if (name.length() > length) {
        // If name is longer than the special_names[i] that it matches
        // the next character must be not lower case (newton vs newTon vs
        // new_ton).
        return !absl::ascii_islower(name[length]);
      } else {
        return true;
      }
    }
  }
  return false;
}

void MaybeUnQuote(absl::string_view* input) {
  if ((input->length() >= 2) &&
      ((*input->data() == '\'' || *input->data() == '"')) &&
      ((*input)[input->length() - 1] == *input->data())) {
    input->remove_prefix(1);
    input->remove_suffix(1);
  }
}

}  // namespace

bool IsRetainedName(const std::string& name) {
  // List of prefixes from
  // http://developer.apple.com/library/mac/#documentation/Cocoa/Conceptual/MemoryMgmt/Articles/mmRules.html
  static const std::string retained_names[] = {"new", "alloc", "copy",
                                               "mutableCopy"};
  return IsSpecialNamePrefix(name, retained_names,
                             sizeof(retained_names) / sizeof(retained_names[0]));
}

bool IsInitName(const std::string& name) {
  static const std::string init_names[] = {"init"};
  return IsSpecialNamePrefix(name, init_names,
                             sizeof(init_names) / sizeof(init_names[0]));
}

bool IsCreateName(const std::string& name) {
  // List of segments from
  // https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFMemoryMgmt/Concepts/Ownership.html#//apple_ref/doc/uid/20001148-103029
  static const std::string create_names[] = {"Create", "Copy"};
  const size_t count = sizeof(create_names) / sizeof(create_names[0]);

  for (size_t i = 0; i < count; ++i) {
    const size_t length = create_names[i].length();
    size_t pos = name.find(create_names[i]);
    if (pos != std::string::npos) {
      // The above docs don't actually call out anything about the characters
      // before the special words. So it's not clear if something like
      // "FOOCreate" would or would not match the "The Create Rule", but by not
      // checking, and claiming it does match, then callers will annotate with
      // `cf_returns_not_retained` which will ensure things work as desired.
      //
      // The footnote here is the docs do have a passing reference to "NoCopy",
      // but again, not looking for that and just returning `true` will cause
      // callers to annotate the api as not being a Create Rule function.

      // If name is longer than the create_names[i] that it matches the next
      // character must be not lower case (Copyright vs CopyFoo vs Copy_Foo).
      if (name.length() > pos + length) {
        return !absl::ascii_islower(name[pos + length]);
      } else {
        return true;
      }
    }
  }
  return false;
}

std::string BaseFileName(const FileDescriptor* file) {
  std::string basename;
  PathSplit(file->name(), NULL, &basename);
  return basename;
}

std::string FileClassPrefix(const FileDescriptor* file) {
  // Always honor the file option.
  if (file->options().has_objc_class_prefix()) {
    return file->options().objc_class_prefix();
  }

  // If package prefix is specified in an prefix to proto mappings file then use that.
  std::string objc_class_prefix = g_prefix_mode.prefix_from_proto_package_mappings(file);
  if (!objc_class_prefix.empty()) {
    return objc_class_prefix;
  }

  // If package prefix isn't enabled, done.
  if (!g_prefix_mode.use_package_name()) {
    return "";
  }

  // If the package is in the exceptions list, done.
  if (g_prefix_mode.is_package_exempted(file->package())) {
    return "";
  }

  // Transform the package into a prefix: use the dot segments as part,
  // camelcase each one and then join them with underscores, and add an
  // underscore at the end.
  std::string result;
  const std::vector<std::string> segments = absl::StrSplit(file->package(), ".", absl::SkipEmpty());
  for (const auto& segment : segments) {
    const std::string part = UnderscoresToCamelCase(segment, true);
    if (part.empty()) {
      continue;
    }
    if (!result.empty()) {
      result.append("_");
    }
    result.append(part);
  }
  if (!result.empty()) {
    result.append("_");
  }
  return g_prefix_mode.forced_package_prefix() + result;
}

std::string FilePath(const FileDescriptor* file) {
  std::string output;
  std::string basename;
  std::string directory;
  PathSplit(file->name(), &directory, &basename);
  if (directory.length() > 0) {
    output = directory + "/";
  }
  basename = StripProto(basename);

  // CamelCase to be more ObjC friendly.
  basename = UnderscoresToCamelCase(basename, true);

  output += basename;
  return output;
}

std::string FilePathBasename(const FileDescriptor* file) {
  std::string output;
  std::string basename;
  std::string directory;
  PathSplit(file->name(), &directory, &basename);
  basename = StripProto(basename);

  // CamelCase to be more ObjC friendly.
  output = UnderscoresToCamelCase(basename, true);

  return output;
}

std::string FileClassName(const FileDescriptor* file) {
  const std::string prefix = FileClassPrefix(file);
  const std::string name =
      UnderscoresToCamelCase(StripProto(BaseFileName(file)), true) + "Root";
  // There aren't really any reserved words that end in "Root", but playing
  // it safe and checking.
  return SanitizeNameForObjC(prefix, name, "_RootClass", NULL);
}

std::string ClassNameWorker(const Descriptor* descriptor) {
  std::string name;
  if (descriptor->containing_type() != NULL) {
    name = ClassNameWorker(descriptor->containing_type());
    name += "_";
  }
  return name + descriptor->name();
}

std::string ClassNameWorker(const EnumDescriptor* descriptor) {
  std::string name;
  if (descriptor->containing_type() != NULL) {
    name = ClassNameWorker(descriptor->containing_type());
    name += "_";
  }
  return name + descriptor->name();
}

std::string ClassName(const Descriptor* descriptor) {
  return ClassName(descriptor, NULL);
}

std::string ClassName(const Descriptor* descriptor,
                      std::string* out_suffix_added) {
  // 1. Message names are used as is (style calls for CamelCase, trust it).
  // 2. Check for reserved word at the very end and then suffix things.
  const std::string prefix = FileClassPrefix(descriptor->file());
  const std::string name = ClassNameWorker(descriptor);
  return SanitizeNameForObjC(prefix, name, "_Class", out_suffix_added);
}

std::string EnumName(const EnumDescriptor* descriptor) {
  // 1. Enum names are used as is (style calls for CamelCase, trust it).
  // 2. Check for reserved word at the every end and then suffix things.
  //      message Fixed {
  //        message Size {...}
  //        enum Mumble {...}
  //      ...
  //      }
  //    yields Fixed_Class, Fixed_Size.
  const std::string prefix = FileClassPrefix(descriptor->file());
  const std::string name = ClassNameWorker(descriptor);
  return SanitizeNameForObjC(prefix, name, "_Enum", NULL);
}

std::string EnumValueName(const EnumValueDescriptor* descriptor) {
  // Because of the Switch enum compatibility, the name on the enum has to have
  // the suffix handing, so it slightly diverges from how nested classes work.
  //   enum Fixed {
  //     FOO = 1
  //   }
  // yields Fixed_Enum and Fixed_Enum_Foo (not Fixed_Foo).
  const std::string class_name = EnumName(descriptor->type());
  const std::string value_str =
      UnderscoresToCamelCase(descriptor->name(), true);
  const std::string name = class_name + "_" + value_str;
  // There aren't really any reserved words with an underscore and a leading
  // capital letter, but playing it safe and checking.
  return SanitizeNameForObjC("", name, "_Value", NULL);
}

std::string EnumValueShortName(const EnumValueDescriptor* descriptor) {
  // Enum value names (EnumValueName above) are the enum name turned into
  // a class name and then the value name is CamelCased and concatenated; the
  // whole thing then gets sanitized for reserved words.
  // The "short name" is intended to be the final leaf, the value name; but
  // you can't simply send that off to sanitize as that could result in it
  // getting modified when the full name didn't.  For example enum
  // "StorageModes" has a value "retain".  So the full name is
  // "StorageModes_Retain", but if we sanitize "retain" it would become
  // "RetainValue".
  // So the right way to get the short name is to take the full enum name
  // and then strip off the enum name (leaving the value name and anything
  // done by sanitize).
  const std::string class_name = EnumName(descriptor->type());
  const std::string long_name_prefix = class_name + "_";
  const std::string long_name = EnumValueName(descriptor);
  return std::string(absl::StripPrefix(long_name, long_name_prefix));
}

std::string UnCamelCaseEnumShortName(const std::string& name) {
  std::string result;
  for (int i = 0; i < name.size(); i++) {
    char c = name[i];
    if (i > 0 && absl::ascii_isupper(c)) {
      result += '_';
    }
    result += absl::ascii_toupper(c);
  }
  return result;
}

std::string ExtensionMethodName(const FieldDescriptor* descriptor) {
  const std::string name = NameFromFieldDescriptor(descriptor);
  const std::string result = UnderscoresToCamelCase(name, false);
  return SanitizeNameForObjC("", result, "_Extension", NULL);
}

std::string FieldName(const FieldDescriptor* field) {
  const std::string name = NameFromFieldDescriptor(field);
  std::string result = UnderscoresToCamelCase(name, false);
  if (field->is_repeated() && !field->is_map()) {
    // Add "Array" before do check for reserved worlds.
    result += "Array";
  } else {
    // If it wasn't repeated, but ends in "Array", force on the _p suffix.
    if (absl::EndsWith(result, "Array")) {
      result += "_p";
    }
  }
  return SanitizeNameForObjC("", result, "_p", NULL);
}

std::string FieldNameCapitalized(const FieldDescriptor* field) {
  // Want the same suffix handling, so upcase the first letter of the other
  // name.
  std::string result = FieldName(field);
  if (result.length() > 0) {
    result[0] = absl::ascii_toupper(result[0]);
  }
  return result;
}

std::string OneofEnumName(const OneofDescriptor* descriptor) {
  const Descriptor* fieldDescriptor = descriptor->containing_type();
  std::string name = ClassName(fieldDescriptor);
  name += "_" + UnderscoresToCamelCase(descriptor->name(), true) + "_OneOfCase";
  // No sanitize needed because the OS never has names that end in _OneOfCase.
  return name;
}

std::string OneofName(const OneofDescriptor* descriptor) {
  std::string name = UnderscoresToCamelCase(descriptor->name(), false);
  // No sanitize needed because it gets OneOfCase added and that shouldn't
  // ever conflict.
  return name;
}

std::string OneofNameCapitalized(const OneofDescriptor* descriptor) {
  // Use the common handling and then up-case the first letter.
  std::string result = OneofName(descriptor);
  if (result.length() > 0) {
    result[0] = absl::ascii_toupper(result[0]);
  }
  return result;
}

std::string UnCamelCaseFieldName(const std::string& name, const FieldDescriptor* field) {
  absl::string_view worker(name);
  if (absl::EndsWith(worker, "_p")) {
    worker = absl::StripSuffix(worker, "_p");
  }
  if (field->is_repeated() && absl::EndsWith(worker, "Array")) {
    worker = absl::StripSuffix(worker, "Array");
  }
  if (field->type() == FieldDescriptor::TYPE_GROUP) {
    if (worker.length() > 0) {
      if (absl::ascii_islower(worker[0])) {
        std::string copy(worker);
        copy[0] = absl::ascii_toupper(worker[0]);
        return copy;
      }
    }
    return std::string(worker);
  } else {
    std::string result;
    for (int i = 0; i < worker.size(); i++) {
      char c = worker[i];
      if (absl::ascii_isupper(c)) {
        if (i > 0) {
          result += '_';
        }
        result += absl::ascii_tolower(c);
      } else {
        result += c;
      }
    }
    return result;
  }
}

// Making these a generator option for folks that don't use CocoaPods, but do
// want to put the library in a framework is an interesting question. The
// problem is it means changing sources shipped with the library to actually
// use a different value; so it isn't as simple as a option.
const char* const ProtobufLibraryFrameworkName = "Protobuf";

std::string ProtobufFrameworkImportSymbol(const std::string& framework_name) {
  // GPB_USE_[framework_name]_FRAMEWORK_IMPORTS
  std::string result = std::string("GPB_USE_");
  result += absl::AsciiStrToUpper(framework_name);
  result += "_FRAMEWORK_IMPORTS";
  return result;
}

bool IsProtobufLibraryBundledProtoFile(const FileDescriptor* file) {
  // We don't check the name prefix or proto package because some files
  // (descriptor.proto), aren't shipped generated by the library, so this
  // seems to be the safest way to only catch the ones shipped.
  const std::string name = file->name();
  if (name == "google/protobuf/any.proto" ||
      name == "google/protobuf/api.proto" ||
      name == "google/protobuf/duration.proto" ||
      name == "google/protobuf/empty.proto" ||
      name == "google/protobuf/field_mask.proto" ||
      name == "google/protobuf/source_context.proto" ||
      name == "google/protobuf/struct.proto" ||
      name == "google/protobuf/timestamp.proto" ||
      name == "google/protobuf/type.proto" ||
      name == "google/protobuf/wrappers.proto") {
    return true;
  }
  return false;
}

namespace {

bool PackageToPrefixesCollector::ConsumeLine(
    const absl::string_view& line, std::string* out_error) {
  int offset = line.find('=');
  if (offset == absl::string_view::npos) {
    *out_error = usage_ + " file line without equal sign: '" + absl::StrCat(line) + "'.";
    return false;
  }
  absl::string_view package = absl::StripAsciiWhitespace(line.substr(0, offset));
  absl::string_view prefix = absl::StripAsciiWhitespace(line.substr(offset + 1));
  MaybeUnQuote(&prefix);
  // Don't really worry about error checking the package/prefix for
  // being valid.  Assume the file is validated when it is created/edited.
  (*prefix_map_)[std::string(package)] = std::string(prefix);
  return true;
}

bool LoadExpectedPackagePrefixes(const std::string& expected_prefixes_path,
                                 std::map<std::string, std::string>* prefix_map,
                                 std::string* out_error) {
  if (expected_prefixes_path.empty()) {
    return true;
  }

  PackageToPrefixesCollector collector("Expected prefixes", prefix_map);
  return ParseSimpleFile(
      expected_prefixes_path, &collector, out_error);
}

bool ValidateObjCClassPrefix(
    const FileDescriptor* file, const std::string& expected_prefixes_path,
    const std::map<std::string, std::string>& expected_package_prefixes,
    bool prefixes_must_be_registered, bool require_prefixes,
    std::string* out_error) {
  // Reminder: An explicit prefix option of "" is valid in case the default
  // prefixing is set to use the proto package and a file needs to be generated
  // without any prefix at all (for legacy reasons).

  bool has_prefix = file->options().has_objc_class_prefix();
  bool have_expected_prefix_file = !expected_prefixes_path.empty();

  const std::string prefix = file->options().objc_class_prefix();
  const std::string package = file->package();
  // For files without packages, the can be registered as "no_package:PATH",
  // allowing the expected prefixes file.
  static const std::string no_package_prefix("no_package:");
  const std::string lookup_key =
      package.empty() ? no_package_prefix + file->name() : package;

  // NOTE: src/google/protobuf/compiler/plugin.cc makes use of cerr for some
  // error cases, so it seems to be ok to use as a back door for warnings.

  // Check: Error - See if there was an expected prefix for the package and
  // report if it doesn't match (wrong or missing).
  std::map<std::string, std::string>::const_iterator package_match =
      expected_package_prefixes.find(lookup_key);
  if (package_match != expected_package_prefixes.end()) {
    // There was an entry, and...
    if (has_prefix && package_match->second == prefix) {
      // ...it matches.  All good, out of here!
      return true;
    } else {
      // ...it didn't match!
      *out_error = "error: Expected 'option objc_class_prefix = \"" +
                   package_match->second + "\";'";
      if (!package.empty()) {
        *out_error += " for package '" + package + "'";
      }
      *out_error += " in '" + file->name() + "'";
      if (has_prefix) {
        *out_error += "; but found '" + prefix + "' instead";
      }
      *out_error += ".";
      return false;
    }
  }

  // If there was no prefix option, we're done at this point.
  if (!has_prefix) {
    if (require_prefixes) {
      *out_error =
        "error: '" + file->name() + "' does not have a required 'option" +
        " objc_class_prefix'.";
      return false;
    }
    return true;
  }

  // When the prefix is non empty, check it against the expected entries.
  if (!prefix.empty() && have_expected_prefix_file) {
    // For a non empty prefix, look for any other package that uses the prefix.
    std::string other_package_for_prefix;
    for (std::map<std::string, std::string>::const_iterator i =
             expected_package_prefixes.begin();
         i != expected_package_prefixes.end(); ++i) {
      if (i->second == prefix) {
        other_package_for_prefix = i->first;
        // Stop on the first real package listing, if it was a no_package file
        // specific entry, keep looking to try and find a package one.
        if (!absl::StartsWith(other_package_for_prefix, no_package_prefix)) {
          break;
        }
      }
    }

    // Check: Error - Make sure the prefix wasn't expected for a different
    // package (overlap is allowed, but it has to be listed as an expected
    // overlap).
    if (!other_package_for_prefix.empty()) {
      *out_error =
          "error: Found 'option objc_class_prefix = \"" + prefix +
          "\";' in '" + file->name() + "'; that prefix is already used for ";
      if (absl::StartsWith(other_package_for_prefix, no_package_prefix)) {
        absl::StrAppend(
            out_error, "file '",
            absl::StripPrefix(other_package_for_prefix, no_package_prefix),
            "'.");
      } else {
        absl::StrAppend(out_error, "'package ",
                        other_package_for_prefix + ";'.");
      }
      absl::StrAppend(out_error, " It can only be reused by adding '",
                      lookup_key, " = ", prefix,
                      "' to the expected prefixes file (",
                      expected_prefixes_path, ").");
      return false;  // Only report first usage of the prefix.
    }
  } // !prefix.empty() && have_expected_prefix_file

  // Check: Warning - Make sure the prefix is is a reasonable value according
  // to Apple's rules (the checks above implicitly whitelist anything that
  // doesn't meet these rules).
  if (!prefix.empty() && !absl::ascii_isupper(prefix[0])) {
    std::cerr
         << "protoc:0: warning: Invalid 'option objc_class_prefix = \""
         << prefix << "\";' in '" << file->name() << "';"
         << " it should start with a capital letter." << std::endl;
    std::cerr.flush();
  }
  if (!prefix.empty() && prefix.length() < 3) {
    // Apple reserves 2 character prefixes for themselves. They do use some
    // 3 character prefixes, but they haven't updated the rules/docs.
    std::cerr
         << "protoc:0: warning: Invalid 'option objc_class_prefix = \""
         << prefix << "\";' in '" << file->name() << "';"
         << " Apple recommends they should be at least 3 characters long."
         << std::endl;
    std::cerr.flush();
  }

  // Check: Error/Warning - If the given package/prefix pair wasn't expected,
  // issue a error/warning to added to the file.
  if (have_expected_prefix_file) {
    if (prefixes_must_be_registered) {
      *out_error =
        "error: '" + file->name() + "' has 'option objc_class_prefix = \"" +
        prefix + "\";', but it is not registered. Add '" + lookup_key + " = " +
        (prefix.empty() ? "\"\"" : prefix) +
        "' to the expected prefixes file (" + expected_prefixes_path + ").";
      return false;
    }

    std::cerr
         << "protoc:0: warning: Found unexpected 'option objc_class_prefix = \""
         << prefix << "\";' in '" << file->name() << "'; consider adding '"
         << lookup_key << " = " << (prefix.empty() ? "\"\"" : prefix)
         << "' to the expected prefixes file (" << expected_prefixes_path
         << ")." << std::endl;
    std::cerr.flush();
  }

  return true;
}

}  // namespace

Options::Options() {
  // While there are generator options, also support env variables to help with
  // build systems where it isn't as easy to hook in for add the generation
  // options when invoking protoc.
  const char* file_path = getenv("GPB_OBJC_EXPECTED_PACKAGE_PREFIXES");
  if (file_path) {
    expected_prefixes_path = file_path;
  }
  const char* suppressions = getenv("GPB_OBJC_EXPECTED_PACKAGE_PREFIXES_SUPPRESSIONS");
  if (suppressions) {
    expected_prefixes_suppressions =
        absl::StrSplit(suppressions, ";", absl::SkipEmpty());
  }
  prefixes_must_be_registered =
      BoolFromEnvVar("GPB_OBJC_PREFIXES_MUST_BE_REGISTERED", false);
  require_prefixes = BoolFromEnvVar("GPB_OBJC_REQUIRE_PREFIXES", false);
}

bool ValidateObjCClassPrefixes(const std::vector<const FileDescriptor*>& files,
                               std::string* out_error) {
    // Options's ctor load from the environment.
    Options options;
    return ValidateObjCClassPrefixes(files, options, out_error);
}

bool ValidateObjCClassPrefixes(const std::vector<const FileDescriptor*>& files,
                               const Options& generation_options,
                               std::string* out_error) {
  // Allow a '-' as the path for the expected prefixes to completely disable
  // even the most basic of checks.
  if (generation_options.expected_prefixes_path == "-") {
    return true;
  }

  // Load the expected package prefixes, if available, to validate against.
  std::map<std::string, std::string> expected_package_prefixes;
  if (!LoadExpectedPackagePrefixes(generation_options.expected_prefixes_path,
                                   &expected_package_prefixes,
                                   out_error)) {
    return false;
  }

  for (int i = 0; i < files.size(); i++) {
    bool should_skip =
      (std::find(generation_options.expected_prefixes_suppressions.begin(),
                 generation_options.expected_prefixes_suppressions.end(),
                 files[i]->name())
          != generation_options.expected_prefixes_suppressions.end());
    if (should_skip) {
      continue;
    }

    bool is_valid =
        ValidateObjCClassPrefix(files[i],
                                generation_options.expected_prefixes_path,
                                expected_package_prefixes,
                                generation_options.prefixes_must_be_registered,
                                generation_options.require_prefixes,
                                out_error);
    if (!is_valid) {
      return false;
    }
  }
  return true;
}

}  // namespace objectivec
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
