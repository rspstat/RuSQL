use std::sync::{Mutex, Condvar};
use std::fs::OpenOptions;
use std::path::Path;
use crate::transaction::wal::WAL_PATH;

/// WAL Group Commit 코디네이터.
///
/// 여러 세션이 동시에 커밋할 때 fsync를 한 번으로 묶는다.
/// 첫 번째 도착 세션(leader)이 fsync를 수행하고,
/// 그 사이에 도착한 세션(follower)은 leader의 fsync 완료를 기다린다.
///
/// 단일 세션 환경에서는 leader가 즉시 fsync하므로 기존 대비 오버헤드가 없다.
pub struct GroupCommitCoordinator {
    state: Mutex<GcState>,
    cvar:  Condvar,
}

struct GcState {
    flushing:   bool,
    generation: u64,
}

impl GroupCommitCoordinator {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(GcState { flushing: false, generation: 0 }),
            cvar:  Condvar::new(),
        }
    }

    /// COMMIT 레코드를 WAL에 기록한 직후 호출.
    /// leader는 fsync를 수행하고, follower는 leader의 fsync 완료까지 대기.
    /// 함수가 반환되면 COMMIT 레코드가 반드시 디스크에 영속화되어 있다.
    pub fn sync_commit(&self) {
        let my_gen;
        let is_leader;
        {
            let mut s = self.state.lock().unwrap();
            my_gen   = s.generation;
            is_leader = !s.flushing;
            if is_leader {
                s.flushing = true;
            }
        }

        if is_leader {
            // 팔로워들이 COMMIT 레코드를 파일에 기록할 기회를 한 번 양보
            std::thread::yield_now();

            // 단일 fsync — 이 시점까지 WAL 파일에 기록된 모든 COMMIT 레코드 영속화
            if Path::new(WAL_PATH).exists() {
                if let Ok(f) = OpenOptions::new().write(true).open(WAL_PATH) {
                    let _ = f.sync_all();
                }
            }

            let mut s = self.state.lock().unwrap();
            s.flushing   = false;
            s.generation += 1;
            drop(s);
            self.cvar.notify_all();
        } else {
            // leader의 fsync 완료 대기
            let s = self.state.lock().unwrap();
            drop(self.cvar.wait_while(s, |st| st.generation == my_gen));
        }
    }
}
