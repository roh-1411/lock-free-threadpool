void wait_all() {
    // First phase: wait while tasks are being processed
    while (active_tasks > 0) {
        // Optionally yield or sleep to avoid busy waiting
    }

    // Second phase: check the queue
    while (!is_queue_empty()) {
        // Optionally yield or sleep to avoid busy waiting
    }

    // Final check to ensure no active tasks
    while (active_tasks > 0) {
        // Optionally yield or sleep to avoid busy waiting
    }
}