// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPHFS_MIRROR_PEER_REPLAYER_H
#define CEPHFS_MIRROR_PEER_REPLAYER_H

#include "common/Formatter.h"
#include "common/Thread.h"
#include "mds/FSMap.h"
#include "Types.h"

namespace cephfs {
namespace mirror {

class FSMirror;
class PeerReplayerAdminSocketHook;

class PeerReplayer {
public:
  PeerReplayer(CephContext *cct, FSMirror *fs_mirror,
               const Filesystem &filesystem, const Peer &peer,
               const std::set<std::string, std::less<>> &directories,
               MountRef mount);
  ~PeerReplayer();

  // initialize replayer for a peer
  int init();

  // shutdown replayer for a peer
  void shutdown();

  // add a directory to mirror queue
  void add_directory(string_view dir_path);

  // remove a directory from queue
  void remove_directory(string_view dir_path);

  // admin socket helpers
  void peer_status(Formatter *f);

private:
  inline static const std::string PRIMARY_SNAP_ID_KEY = "primary_snap_id";

  bool is_stopping() {
    return m_stopping;
  }

  struct Replayer;
  class SnapshotReplayerThread : public Thread {
  public:
    SnapshotReplayerThread(PeerReplayer *peer_replayer)
      : m_peer_replayer(peer_replayer) {
    }

    void *entry() override {
      m_peer_replayer->run(this);
      return 0;
    }

    void cancel() {
      canceled = true;
    }

    bool is_canceled() const {
      return canceled;
    }

  private:
    PeerReplayer *m_peer_replayer;
    bool canceled = false;
  };

  struct DirRegistry {
    int fd;
    SnapshotReplayerThread *replayer;
  };

  struct SyncEntry {
    std::string epath;
    ceph_dir_result *dirp; // valid for directories
    struct ceph_statx stx;

    SyncEntry(std::string_view path,
              const struct ceph_statx &stx)
      : epath(path),
        stx(stx) {
    }
    SyncEntry(std::string_view path,
              ceph_dir_result *dirp,
              const struct ceph_statx &stx)
      : epath(path),
        dirp(dirp),
        stx(stx) {
    }

    bool is_directory() const {
      return S_ISDIR(stx.stx_mode);
    }
  };

  using clock = ceph::coarse_mono_clock;
  using time = ceph::coarse_mono_time;

  struct SnapSyncStat {
    uint64_t nr_failures = 0; // number of consecutive failures
    boost::optional<time> last_failed; // lat failed timestamp
    bool failed = false; // hit upper cap for consecutive failures
    boost::optional<std::pair<uint64_t, std::string>> last_synced_snap;
    boost::optional<std::pair<uint64_t, std::string>> current_syncing_snap;
    uint64_t synced_snap_count = 0;
    uint64_t deleted_snap_count = 0;
    uint64_t renamed_snap_count = 0;
    time last_synced = clock::zero();
    boost::optional<double> last_sync_duration;
  };

  void _inc_failed_count(const std::string &dir_path) {
    auto max_failures = g_ceph_context->_conf.get_val<uint64_t>(
    "cephfs_mirror_max_consecutive_failures_per_directory");
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    sync_stat.last_failed = clock::now();
    if (++sync_stat.nr_failures >= max_failures) {
      sync_stat.failed = true;
    }
  }
  void _reset_failed_count(const std::string &dir_path) {
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    sync_stat.nr_failures = 0;
    sync_stat.failed = false;
    sync_stat.last_failed = boost::none;
  }

  void _set_last_synced_snap(const std::string &dir_path, uint64_t snap_id,
                            const std::string &snap_name) {
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    sync_stat.last_synced_snap = std::make_pair(snap_id, snap_name);
    sync_stat.current_syncing_snap = boost::none;
  }
  void set_last_synced_snap(const std::string &dir_path, uint64_t snap_id,
                            const std::string &snap_name) {
    std::scoped_lock locker(m_lock);
    _set_last_synced_snap(dir_path, snap_id, snap_name);
  }
  void set_current_syncing_snap(const std::string &dir_path, uint64_t snap_id,
                                const std::string &snap_name) {
    std::scoped_lock locker(m_lock);
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    sync_stat.current_syncing_snap = std::make_pair(snap_id, snap_name);
  }
  void clear_current_syncing_snap(const std::string &dir_path) {
    std::scoped_lock locker(m_lock);
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    sync_stat.current_syncing_snap = boost::none;
  }
  void inc_deleted_snap(const std::string &dir_path) {
    std::scoped_lock locker(m_lock);
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    ++sync_stat.deleted_snap_count;
  }
  void inc_renamed_snap(const std::string &dir_path) {
    std::scoped_lock locker(m_lock);
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    ++sync_stat.renamed_snap_count;
  }
  void set_last_synced_stat(const std::string &dir_path, uint64_t snap_id,
                            const std::string &snap_name, double duration) {
    std::scoped_lock locker(m_lock);
    _set_last_synced_snap(dir_path, snap_id, snap_name);
    auto &sync_stat = m_snap_sync_stats.at(dir_path);
    sync_stat.last_synced = clock::now();
    sync_stat.last_sync_duration = duration;
    ++sync_stat.synced_snap_count;
  }

  bool should_backoff(const std::string &dir_path, int *retval) {
    if (m_fs_mirror->is_blocklisted()) {
      *retval = -EBLOCKLISTED;
      return true;
    }

    std::scoped_lock locker(m_lock);
    if (is_stopping()) {
      // ceph defines EBLOCKLISTED to ESHUTDOWN (108). so use
      // EINPROGRESS to identify shutdown.
      *retval = -EINPROGRESS;
      return true;
    }
    auto &dr = m_registered.at(dir_path);
    if (dr.replayer->is_canceled()) {
      *retval = -ECANCELED;
      return true;
    }

    *retval = 0;
    return false;
  }

  typedef std::vector<std::unique_ptr<SnapshotReplayerThread>> SnapshotReplayers;

  CephContext *m_cct;
  FSMirror *m_fs_mirror;
  Peer m_peer;
  // probably need to be encapsulated when supporting cancelations
  std::map<std::string, DirRegistry> m_registered;
  std::vector<std::string> m_directories;
  std::map<std::string, SnapSyncStat> m_snap_sync_stats;
  MountRef m_local_mount;
  PeerReplayerAdminSocketHook *m_asok_hook = nullptr;

  ceph::mutex m_lock;
  ceph::condition_variable m_cond;
  RadosRef m_remote_cluster;
  MountRef m_remote_mount;
  bool m_stopping = false;
  SnapshotReplayers m_replayers;

  void run(SnapshotReplayerThread *replayer);

  boost::optional<std::string> pick_directory();
  int register_directory(const std::string &dir_path, SnapshotReplayerThread *replayer);
  void unregister_directory(const std::string &dir_path);
  int try_lock_directory(const std::string &dir_path, SnapshotReplayerThread *replayer,
                         DirRegistry *registry);
  void unlock_directory(const std::string &dir_path, const DirRegistry &registry);
  void sync_snaps(const std::string &dir_path, std::unique_lock<ceph::mutex> &locker);

  int do_sync_snaps(const std::string &dir_path);
  int build_snap_map(const std::string &dir_path, std::map<uint64_t, std::string> *snap_map,
                     bool is_remote=false);
  int propagate_snap_deletes(const std::string &dir_name, const std::set<std::string> &snaps);
  int propagate_snap_renames(const std::string &dir_name,
                             const std::set<std::pair<std::string,std::string>> &snaps);
  int synchronize(const std::string &dir_path, uint64_t snap_id, const std::string &snap_name);
  int do_synchronize(const std::string &path, const std::string &snap_name);

  int cleanup_remote_dir(const std::string &dir_path);
  int remote_mkdir(const std::string &local_path, const std::string &remote_path,
                   const struct ceph_statx &stx);
  int remote_file_op(const std::string &dir_path,
                     const std::string &local_path,
                     const std::string &remote_path, const struct ceph_statx &stx);
  int remote_copy(const std::string &dir_path,
                  const std::string &local_path,
                  const std::string &remote_path,
                  const struct ceph_statx &local_stx);
};

} // namespace mirror
} // namespace cephfs

#endif // CEPHFS_MIRROR_PEER_REPLAYER_H
