// Updated wait_all implementation using a two-phase approach with proper acquire semantics and double-checks.

void wait_all() {
    // Phase 1: Setup and initial checks
    if (is_done()) {
        // If done, return immediately
        return;
    }

    acquire_lock(); // Acquire necessary lock
    // Double-check to avoid early return
    if (is_done()) {
        release_lock(); // Release lock if done
        return;
    }

    // Phase 2: Wait until all tasks are done
    wait_for_tasks();
    release_lock(); // Ensure lock is released after waiting
}