import CExp

/// This device's experiment variants, read from the snapshot expd publishes.
///
/// ```swift
/// if Experiments.variant(for: "new_search_ranking") == "treatment" {
///     useNewRanking()
/// }
/// ```
public enum Experiments {
    /// This device's variant for `experiment`, or `defaultValue` if the device isn't in it, expd
    /// hasn't assigned it yet, or anything else goes wrong. Never throws, and never blocks once
    /// a snapshot is mapped, so it's safe on the main thread.
    ///
    /// Logs an exposure the first time each experiment is read with the current snapshot. When
    /// prefetching, pass `logExposure: false` and call ``logExposure(_:)`` when the code runs.
    public static func variant(
        for experiment: String,
        default defaultValue: String = "control",
        logExposure: Bool = true
    ) -> String {
        // A stack buffer: no heap allocation on the read path.
        withUnsafeTemporaryAllocation(of: CChar.self, capacity: Int(EXP_VARIANT_MAX)) { buffer in
            let out = buffer.baseAddress!
            let status = logExposure
                ? exp_variant(experiment, out, buffer.count)
                : exp_peek_variant(experiment, out, buffer.count)
            return status == 0 ? String(cString: out) : defaultValue
        }
    }

    /// Logs that the code depending on `experiment` ran. Once per snapshot; never blocks.
    public static func logExposure(_ experiment: String) {
        exp_log_exposure(experiment)
    }

    /// Picks up a new snapshot if expd published one. Call at a safe point, like launch or
    /// return to foreground, so a variant never changes mid-session.
    @discardableResult
    public static func refresh() -> Bool {
        exp_refresh() == 1
    }
}
