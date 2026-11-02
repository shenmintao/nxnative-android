#ifndef VK_PACE_BUDGET_H
#define VK_PACE_BUDGET_H

// Submission spans include elapsed replay gaps between split parts, not just
// future GPU work. Detect gaps above max(2 * median, period), then reserve the
// median GPU span; the game's period still controls cadence in pace_desired.
static inline double vk_pace_budget_estimate(double latest, const double *spans, int count, double period_ms, double *trimmed_ms) {
    if (trimmed_ms) *trimmed_ms = 0;
    if (latest < 0) return latest;
    double sorted[8]; int n = 0;
    for (int i = 0; i < count && i < 8; i++) if (spans[i] >= 0) {
        int j = n++; while (j && sorted[j - 1] > spans[i]) { sorted[j] = sorted[j - 1]; j--; }
        sorted[j] = spans[i];
    }
    if (n < 3) return latest;
    double median = n & 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    double cap = 2 * median; if (cap < period_ms) cap = period_ms;
    if (latest <= cap) return latest;
    if (trimmed_ms) *trimmed_ms = latest - median;
    return median;
}

#endif
