#ifndef TIMEKEEP_H
#define TIMEKEEP_H

#include <stdbool.h>
#include <time.h>

// Initialize SNTP and timezone
void timekeep_init(void);

// Returns true once NTP has synced at least once
bool timekeep_is_synced(void);

// Get current time as Unix timestamp (seconds since epoch)
time_t timekeep_now(void);

// Format current time into buf (e.g. "2026-04-17 14:30:05")
void timekeep_format(char *buf, size_t len);

// ── Scheduler ──
// Schedule a named action at a specific Unix timestamp
void timekeep_schedule(const char *action, time_t execute_at);

// Start the scheduler background task
void timekeep_start_scheduler(void);

#endif
