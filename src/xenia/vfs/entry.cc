/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/entry.h"

#include <utility>

#include "xenia/base/filesystem.h"
#include "xenia/base/string.h"
#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

Entry::Entry(Device* device, Entry* parent, const std::string_view path)
    : device_(device),
      parent_(parent),
      path_(path),
      attributes_(0),
      size_(0),
      allocation_size_(0),
      create_timestamp_(0),
      access_timestamp_(0),
      write_timestamp_(0),
      delete_on_close_(false) {
  assert_not_null(device);
  absolute_path_ = xe::utf8::join_guest_paths(device->mount_path(), path);
  name_ = xe::utf8::find_name_from_guest_path(path);
}

Entry::~Entry() = default;

void Entry::Dump(xe::StringBuffer* string_buffer, int indent) {
  for (int i = 0; i < indent; ++i) {
    string_buffer->Append(' ');
  }
  string_buffer->Append(name());
  string_buffer->Append('\n');
  for (auto& child : children_) {
    child->Dump(string_buffer, indent + 2);
  }
}

bool Entry::is_read_only() const { return device_->is_read_only(); }

Entry* Entry::GetChild(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  auto it =
      std::ranges::find_if(std::as_const(children_), [&](const auto& child) {
        return xe::utf8::equal_case(child->name(), name);
      });
  if (it == children_.cend()) {
    return nullptr;
  }
  return (*it).get();
}

Entry* Entry::ResolvePath(const std::string_view path) {
  // Walk the path, one separator at a time.
  Entry* entry = this;
  for (auto& part : xe::utf8::split_path(path)) {
    entry = entry->GetChild(part);
    if (!entry) {
      // Not found.
      return nullptr;
    }
  }
  return entry;
}

Entry* Entry::IterateChildren(const xe::filesystem::WildcardEngine& engine,
                              size_t* current_index) {
  auto global_lock = global_critical_region_.Acquire();
  while (*current_index < children_.size()) {
    auto& child = children_[*current_index];
    *current_index = *current_index + 1;
    if (engine.Match(child->name())) {
      return child.get();
    }
  }
  return nullptr;
}

Entry* Entry::CreateEntry(const std::string_view name, uint32_t attributes) {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only()) {
    return nullptr;
  }
  if (GetChild(name)) {
    // Already exists.
    return nullptr;
  }
  auto entry = CreateEntryInternal(name, attributes);
  if (!entry) {
    return nullptr;
  }
  children_.push_back(std::move(entry));
  // TODO(benvanik): resort? would break iteration?
  Touch();
  return children_.back().get();
}

bool Entry::Delete(Entry* entry) {
  auto global_lock = global_critical_region_.Acquire();
  if (is_read_only()) {
    return false;
  }
  if (entry->parent() != this) {
    return false;
  }
  if (!DeleteEntryInternal(entry)) {
    return false;
  }
  for (auto it = children_.begin(); it != children_.end(); ++it) {
    if (it->get() == entry) {
      children_.erase(it);
      break;
    }
  }
  Touch();
  return true;
}

bool Entry::Delete() {
  assert_not_null(parent_);
  return parent_->Delete(this);
}

void Entry::Touch() {
  // TODO(benvanik): update timestamps.
}

void Entry::UpdatePathsFromParent() {
  path_ = xe::utf8::join_guest_paths(parent_->path(), name_);
  absolute_path_ = xe::utf8::join_guest_paths(device_->mount_path(), path_);
  OnPathChanged();
  for (auto& child : children_) {
    child->UpdatePathsFromParent();
  }
}

bool Entry::Rename(const std::filesystem::path file_path,
                   bool replace_existing, X_STATUS* out_status) {
  auto global_lock = global_critical_region_.Acquire();
  auto fail = [&](X_STATUS status) {
    if (out_status) {
      *out_status = status;
    }
    return false;
  };
  if (out_status) {
    *out_status = X_STATUS_SUCCESS;
  }
  if (is_read_only() || !parent_) {
    return fail(X_STATUS_ACCESS_DENIED);
  }

  // Store the string to ensure string_views from split_path remain valid
  const std::string path_str = xe::path_to_utf8(file_path);
  std::vector<std::string_view> path_parts = xe::utf8::split_path(path_str);

  // Remove the root (e.g., "cache:")
  if (path_parts.size() < 2) {
    return fail(X_STATUS_OBJECT_NAME_INVALID);
  }
  path_parts.erase(path_parts.begin());

  // Find the directory the entry is being moved to.
  const std::string new_name(path_parts.back());
  const std::string new_parent_path = xe::utf8::join_guest_paths(
      std::vector<std::string_view>(path_parts.begin(), path_parts.end() - 1));
  Entry* new_parent = new_parent_path.empty()
                          ? device_->ResolvePath("")
                          : device_->ResolvePath(new_parent_path);
  if (!new_parent || !(new_parent->attributes() & kFileAttributeDirectory)) {
    return fail(X_STATUS_OBJECT_PATH_NOT_FOUND);
  }
  // Refuse to move a directory into itself.
  for (Entry* e = new_parent; e; e = e->parent_) {
    if (e == this) {
      return fail(X_STATUS_INVALID_PARAMETER);
    }
  }
  Entry* existing = new_parent->GetChild(new_name);
  if (existing == this) {
    // Renaming an entry onto itself is a successful no-op.
    return true;
  }
  if (existing) {
    // POSIX rename() semantics (NtSetInformationFile with ReplaceIfExists):
    // replace an existing file. Directories are never replaced.
    if (!replace_existing) {
      return fail(X_STATUS_OBJECT_NAME_COLLISION);
    }
    if ((existing->attributes() & kFileAttributeDirectory) ||
        (attributes() & kFileAttributeDirectory)) {
      return fail(X_STATUS_ACCESS_DENIED);
    }
  }

  if (!RenameEntryInternal(path_parts)) {
    return fail(X_STATUS_ACCESS_DENIED);
  }

  // The host rename replaced the target; drop its stale cached entry.
  if (existing) {
    for (auto it = new_parent->children_.begin();
         it != new_parent->children_.end(); ++it) {
      if (it->get() == existing) {
        new_parent->children_.erase(it);
        break;
      }
    }
  }

  // Move this entry into the new parent's children list.
  if (new_parent != parent_) {
    Entry* old_parent = parent_;
    for (auto it = old_parent->children_.begin();
         it != old_parent->children_.end(); ++it) {
      if (it->get() == this) {
        new_parent->children_.push_back(std::move(*it));
        old_parent->children_.erase(it);
        break;
      }
    }
    parent_ = new_parent;
  }

  // Use the guest path since POSIX doesn't treat \ as a separator.
  name_ = new_name;
  UpdatePathsFromParent();
  return true;
}

}  // namespace vfs
}  // namespace xe
