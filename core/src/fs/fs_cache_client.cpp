#include <userver/fs/fs_cache_client.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>

#include <userver/engine/task/task_processor_fwd.hpp>
#include <userver/fs/read.hpp>
#include <userver/rcu/rcu_map.hpp>
#include <userver/utils/async.hpp>
#include <userver/utils/periodic_task.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

std::string GetNormalizeDirectory(std::string_view dir) {
  auto slice = dir.size();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (auto it = dir.rbegin(); it != dir.rend(); ++it) {
    if (*it == '/')
      --slice;
    else
      break;
  }
  return std::string{dir.data(), slice};
}

}  // namespace

namespace fs {

#ifdef __linux__
namespace {

bool IsFilepathHidden(const std::string& path) {
  auto filename = boost::filesystem::path(path).filename().string();
  return filename[0] == '.';
}

}  // namespace
#endif  // __linux__

FsCacheClient::FsCacheClient(std::string_view dir,
                             std::chrono::milliseconds update_period,
                             engine::TaskProcessor& tp)
    : dir_(GetNormalizeDirectory(dir)), update_period_(update_period), tp_(tp) {
  UpdateCache();

  if (update_period_ == std::chrono::milliseconds(0)) {
    return;
  }

#ifdef __linux__
  inotify_task_ =
      utils::CriticalAsync("inotify_task", [this] { InotifyWork(); });
#else
  cache_updater_.Start("fs_cache_updater",
                       utils::PeriodicTask::Settings{update_period_},
                       [this] { UpdateCache(); });
#endif
}

void FsCacheClient::UpdateCache() {
  auto map = fs::ReadRecursiveFilesInfoWithData(
      tp_, dir_, {fs::SettingsReadFile::kSkipHidden});
  data_.Assign(std::move(map));
}

#ifdef __linux__
void FsCacheClient::InotifyWork() {
  namespace sys_linux = engine::io::sys_linux;
  sys_linux::Inotify inotify;

  HandleCreateDirectory(inotify, dir_);

  while (!engine::current_task::ShouldCancel()) {
    auto event = inotify.Poll({});
    LOG_INFO() << event;
    if (!event) return;

    if (event->mask & sys_linux::EventType::kMovedFrom ||
        event->mask & sys_linux::EventType::kDelete) {
      if (!(event->mask & sys_linux::EventType::kIsDir)) {
        HandleDelete(event->path);
      } else {
        HandleDeleteDirectory(inotify, event->path);
      }
    }

    if (event->mask & sys_linux::EventType::kMovedTo ||
        event->mask & sys_linux::EventType::kCreate ||
        event->mask & sys_linux::EventType::kModify) {
      if (!(event->mask & sys_linux::EventType::kIsDir)) {
        HandleCreate(event->path);
      } else {
        HandleCreateDirectory(inotify, event->path);
      }
    }
  }
}

void FsCacheClient::HandleDelete(const std::string& path) {
  data_.Erase(GetLexicallyRelative(path, dir_));
}

void FsCacheClient::HandleDeleteDirectory(
    engine::io::sys_linux::Inotify& inotify, const std::string& path) {
  LOG_INFO() << "HandleDeleteDirectory()";
  inotify.RmWatch(path);
}

void FsCacheClient::HandleCreate(const std::string& path) {
  if (IsFilepathHidden(path)) return;

  FileInfoWithData info{};
  info.extension = boost::filesystem::path(path).extension().string();
  info.data = ReadFileContents(tp_, path);
  data_.InsertOrAssign(
      GetLexicallyRelative(path, dir_),
      std::make_shared<const FileInfoWithData>(std::move(info)));
}

void FsCacheClient::HandleCreateDirectory(
    engine::io::sys_linux::Inotify& inotify, const std::string& path) {
  engine::AsyncNoSpan(tp_, [&] {
    return HandleCreateDirectoryBlocking(inotify, path);
  }).Get();
}

void FsCacheClient::HandleCreateDirectoryBlocking(
    engine::io::sys_linux::Inotify& inotify, const std::string& path) {
  LOG_INFO() << "HandleCreateDirectory(" << path << ")";
  namespace sys_linux = engine::io::sys_linux;
  inotify.AddWatch(path, {
                             sys_linux::EventType::kModify,
                             sys_linux::EventType::kMovedFrom,
                             sys_linux::EventType::kMovedTo,
                             sys_linux::EventType::kDelete,
                             sys_linux::EventType::kCreate,
                         });

  for (auto it = boost::filesystem::directory_iterator(path);
       it != boost::filesystem::directory_iterator(); ++it) {
    if (is_directory(it->status())) {
      HandleCreateDirectoryBlocking(
          inotify, path + '/' + it->path().filename().string());
    } else {
      HandleCreate(path + '/' + it->path().filename().string());
    }
  }
}
#endif

FileInfoWithDataConstPtr FsCacheClient::TryGetFile(
    std::string_view path) const {
  LOG_DEBUG() << "Find file " << path;
  auto snap = data_.GetSnapshot();
  if (auto it = snap.find(std::string{path}); it != snap.end())
    return it->second;
  return nullptr;
}

}  // namespace fs

USERVER_NAMESPACE_END
